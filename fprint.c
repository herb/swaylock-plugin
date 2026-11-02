#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "comm.h"
#include "config.h"
#include "fprint.h"
#include "log.h"
#include "swaylock.h"

#if HAVE_PAM

static bool write_byte(int fd, char byte) {
	while (true) {
		ssize_t n = write(fd, &byte, 1);
		if (n == 1) {
			return true;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		return false;
	}
}

static void sleep_ms(int ms) {
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

/*
 * The child needs nothing from the parent except its notification pipe. In
 * particular it must not keep password-backend descriptors around. Only stdin,
 * stdout, stderr and the notification pipe survive.
 */
static void close_inherited_fds(int keep_fd) {
#ifdef __linux__
	DIR *dir = opendir("/proc/self/fd");
	if (dir) {
		int dir_fd = dirfd(dir);
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			char *end = NULL;
			long fd = strtol(entry->d_name, &end, 10);
			if (end == entry->d_name || *end != '\0') {
				continue;
			}
			if (fd < 3 || fd == keep_fd || fd == dir_fd) {
				continue;
			}
			close((int)fd);
		}
		closedir(dir);
		return;
	}
#endif
	for (int fd = 3; fd < 1024; fd++) {
		if (fd != keep_fd) {
			close(fd);
		}
	}
}

/*
 * The fingerprint service must never prompt for a password. pam_fprintd only
 * emits informational messages, but if any module asks for a response we abort
 * rather than supply anything.
 */
static int handle_conversation(int num_msg, const struct pam_message **msg,
		struct pam_response **resp, void *data) {
	(void)data;
	struct pam_response *reply = calloc((size_t)num_msg, sizeof(*reply));
	if (!reply) {
		return PAM_BUF_ERR;
	}
	*resp = reply;
	for (int i = 0; i < num_msg; ++i) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
		case PAM_PROMPT_ECHO_ON:
			/* pam_fprintd never prompts, and this child never holds a
			 * password. Abort rather than answer with anything. */
			return PAM_ABORT;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			break;
		}
	}
	return PAM_SUCCESS;
}

static const struct pam_conv fprint_conv = {
	.conv = handle_conversation,
	.appdata_ptr = NULL,
};

bool fprint_pam_service_available(void) {
	return access(FPRINT_PAM_SERVICE_PATH, F_OK) == 0;
}

bool fprint_spawn_child(pid_t *pid_out, int *fd_out) {
	int fds[2];
	if (pipe(fds) != 0) {
		swaylock_log_errno(LOG_ERROR, "failed to create fingerprint pipe");
		return false;
	}
	if (!set_cloexec(fds[0]) || !set_cloexec(fds[1])) {
		swaylock_log_errno(LOG_ERROR, "failed to set cloexec on fingerprint pipe");
		close(fds[0]);
		close(fds[1]);
		return false;
	}

	pid_t child = fork();
	if (child < 0) {
		swaylock_log_errno(LOG_ERROR, "failed to fork fingerprint child");
		close(fds[0]);
		close(fds[1]);
		return false;
	} else if (child == 0) {
		close(fds[0]);
		close_inherited_fds(fds[1]);
		run_fprint_backend_child(fds[1]);
		_exit(EXIT_FAILURE);
	}

	close(fds[1]);
	*pid_out = child;
	*fd_out = fds[0];
	return true;
}

void fprint_kill_child(pid_t *pid) {
	if (!pid || *pid <= 0) {
		return;
	}
	/* The parent ignores SIGCHLD, so the kernel reaps the child once it exits.
	 * Also try to reap here for callers which do not ignore SIGCHLD, without
	 * ever blocking the password path or the exit path. */
	kill(*pid, SIGTERM);
	int status;
	(void)waitpid(*pid, &status, WNOHANG);
	*pid = -1;
}

void run_fprint_backend_child(int notify_fd) {
#ifdef __linux__
	prctl(PR_SET_PDEATHSIG, SIGTERM);
	if (getppid() == 1) {
		/* The parent exited before prctl() took effect. */
		_exit(EXIT_FAILURE);
	}
#endif

	struct passwd *passwd = getpwuid(getuid());
	if (!passwd) {
		swaylock_log_errno(LOG_ERROR, "fingerprint: getpwuid failed");
		_exit(EXIT_FAILURE);
	}
	char *username = strdup(passwd->pw_name);
	if (!username) {
		swaylock_log(LOG_ERROR, "fingerprint: allocation failed");
		_exit(EXIT_FAILURE);
	}

	swaylock_log(LOG_DEBUG, "fingerprint: prepared to authenticate user %s",
		username);

	int delay_ms = 0;
	while (true) {
		pam_handle_t *auth_handle = NULL;
		struct timespec started, finished;
		clock_gettime(CLOCK_MONOTONIC, &started);
		int status = pam_start(FPRINT_PAM_SERVICE, username, &fprint_conv,
			&auth_handle);
		if (status == PAM_SUCCESS) {
			status = pam_authenticate(auth_handle, 0);
		}
		if (auth_handle) {
			pam_end(auth_handle, status);
		}

		if (status == PAM_SUCCESS) {
			/* Only this fixed byte ever leaves the child. */
			if (!write_byte(notify_fd, FPRINT_MSG_SUCCESS)) {
				swaylock_log_errno(LOG_ERROR,
					"fingerprint: failed to notify parent");
			}
			_exit(EXIT_SUCCESS);
		}

		if (fprint_status_is_mismatch(status)) {
			swaylock_log(LOG_DEBUG,
				"fingerprint not recognized (pam status %d)", status);
			if (!write_byte(notify_fd, FPRINT_MSG_MISMATCH)) {
				swaylock_log_errno(LOG_ERROR,
					"fingerprint: failed to notify parent");
				_exit(EXIT_FAILURE);
			}
		} else {
			/* No device, no enrolled prints, or a transient service error:
			 * back off quietly instead of flickering the invalid indicator. */
			swaylock_log(LOG_DEBUG,
				"fingerprint unavailable (pam status %d)", status);
		}

		clock_gettime(CLOCK_MONOTONIC, &finished);
		long elapsed_ms = (long)(finished.tv_sec - started.tv_sec) * 1000L +
			(finished.tv_nsec - started.tv_nsec) / 1000000L;
		delay_ms = fprint_retry_delay_ms(delay_ms, status, elapsed_ms);
		sleep_ms(delay_ms);
	}
}

#else /* !HAVE_PAM */

bool fprint_pam_service_available(void) {
	return false;
}

bool fprint_spawn_child(pid_t *pid_out, int *fd_out) {
	(void)pid_out;
	(void)fd_out;
	return false;
}

void fprint_kill_child(pid_t *pid) {
	if (pid) {
		*pid = -1;
	}
}

void run_fprint_backend_child(int notify_fd) {
	(void)notify_fd;
	_exit(EXIT_FAILURE);
}

#endif
