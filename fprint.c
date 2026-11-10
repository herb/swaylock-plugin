#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* syscall() for pidfd_open/pidfd_send_signal */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "comm.h"
#include "config.h"
#include "fprint.h"
#include "log.h"

/* Resume re-arm policy. Kept outside the PAM guard so the locker links without
 * PAM; it has no PAM dependency. */
static const int rearm_backoff_ms[FPRINT_REARM_MAX_RETRIES] = {
	1000, 2000, 4000, 8000, 8000,
};

int fprint_rearm_retry_delay_ms(int attempt) {
	if (attempt < 0 || attempt >= FPRINT_REARM_MAX_RETRIES) {
		return -1;
	}
	return rearm_backoff_ms[attempt];
}

bool fprint_resume_keeps_child(void) {
	return false;
}

bool fprint_spawn_allowed(bool sleeping, bool awaiting_sleep_state,
		bool lid_closed) {
	return !sleeping && !awaiting_sleep_state && !lid_closed;
}

bool fprint_spawn_is_too_late(int64_t spawned_ms, int64_t now_ms) {
	return now_ms - spawned_ms < FPRINT_LATE_SPAWN_MS;
}

#if HAVE_PAM

#include <security/pam_appl.h>

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

static int pidfd_open_child(pid_t pid) {
#ifdef SYS_pidfd_open
	return (int)syscall(SYS_pidfd_open, pid, 0);
#else
	(void)pid;
	errno = ENOSYS;
	return -1;
#endif
}

static int pidfd_signal(int pidfd, int sig) {
#ifdef SYS_pidfd_send_signal
	return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
#else
	(void)pidfd;
	(void)sig;
	errno = ENOSYS;
	return -1;
#endif
}

bool fprint_spawn_child(pid_t *pid_out, int *pidfd_out, int *fd_out,
		fprint_child_hook child_hook, void *child_hook_data) {
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

	/* Start gate: the child blocks on it until the parent holds a pidfd. It
	 * cannot exit (and be auto-reaped, freeing its pid for reuse) before then,
	 * so the pidfd is guaranteed to name this child. */
	/* A socketpair rather than a pipe, so releasing it can use MSG_NOSIGNAL:
	 * a child that died before the release must not SIGPIPE the locker. */
	int gate[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, gate) != 0) {
		swaylock_log_errno(LOG_ERROR, "failed to create fingerprint gate");
		close(fds[0]);
		close(fds[1]);
		return false;
	}

	pid_t child = fork();
	if (child < 0) {
		swaylock_log_errno(LOG_ERROR, "failed to fork fingerprint child");
		close(fds[0]);
		close(fds[1]);
		close(gate[0]);
		close(gate[1]);
		return false;
	} else if (child == 0) {
		close(fds[0]);
		close(gate[1]);
		/* The child inherits the parent's address space, which may hold a
		 * partially typed password (this child is respawned after resume).
		 * Give the caller its one chance to wipe that copy before anything
		 * else, in particular before PAM runs. */
		if (child_hook) {
			child_hook(child_hook_data);
		}
		char go = 0;
		ssize_t got;
		do {
			got = read(gate[0], &go, 1);
		} while (got < 0 && errno == EINTR);
		if (got != 1) {
			/* The parent could not get a pidfd and closed the gate. */
			_exit(EXIT_FAILURE);
		}
		close(gate[0]);
		close_inherited_fds(fds[1]);
		run_fprint_backend_child(fds[1]);
		_exit(EXIT_FAILURE);
	}

	close(fds[1]);
	close(gate[0]);
	/* Signal and watch the child only through a pidfd: SIGCHLD is ignored, so
	 * the kernel reaps it and a bare pid could be reused by another process.
	 * The child is still parked on the gate, so this pid is still ours. */
	int pidfd = pidfd_open_child(child);
	if (pidfd < 0 || !set_cloexec(pidfd)) {
		swaylock_log_errno(LOG_ERROR, "failed to open fingerprint child pidfd");
		if (pidfd >= 0) {
			close(pidfd);
		}
		/* Closing the gate makes the child exit by itself; no numeric-pid
		 * signal is ever sent. */
		close(gate[1]);
		close(fds[0]);
		return false;
	}
	char go = 1;
	ssize_t sent;
	do {
		sent = send(gate[1], &go, 1, MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	close(gate[1]);
	if (sent != 1) {
		swaylock_log_errno(LOG_ERROR, "failed to release fingerprint child");
		pidfd_signal(pidfd, SIGKILL);
		close(pidfd);
		close(fds[0]);
		return false;
	}
	if (pid_out) {
		*pid_out = child;
	}
	*pidfd_out = pidfd;
	*fd_out = fds[0];
	return true;
}

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool pidfd_exited(int pidfd, int timeout_ms) {
	struct pollfd pfd = { .fd = pidfd, .events = POLLIN };
	while (true) {
		int ret = poll(&pfd, 1, timeout_ms);
		if (ret > 0) {
			return true;
		}
		/* Timeout, EINTR or error: report "not yet"; the caller recomputes
		 * its remaining budget, so signals cannot stretch the deadline. */
		return false;
	}
}

bool fprint_child_alive(int pidfd) {
	return pidfd >= 0 && !pidfd_exited(pidfd, 0);
}

bool fprint_kill_child(int *pidfd, int wait_ms) {
	if (!pidfd || *pidfd < 0) {
		return true;
	}
	int fd = *pidfd;
	pidfd_signal(fd, SIGTERM);
	if (wait_ms <= 0) {
		/* Fire and forget: the kernel reaps the child (SIGCHLD is ignored). */
		close(fd);
		*pidfd = -1;
		return true;
	}
	/* Bounded wall-clock barrier: SIGTERM, SIGKILL halfway through. A dead
	 * client's fprintd claim is dropped with its bus connection. */
	long deadline = now_ms() + wait_ms;
	long escalate_at = now_ms() + wait_ms / 2;
	bool escalated = false;
	while (true) {
		long now = now_ms();
		if (now >= deadline) {
			break;
		}
		long until = escalated ? deadline : escalate_at;
		int slice = (int)(until > now ? until - now : 0);
		if (pidfd_exited(fd, slice)) {
			close(fd);
			*pidfd = -1;
			return true;
		}
		if (!escalated && now_ms() >= escalate_at) {
			pidfd_signal(fd, SIGKILL);
			escalated = true;
		}
	}
	/* Keep the pidfd: the caller must not start a replacement while this child
	 * may still hold the fprintd claim. */
	swaylock_log(LOG_ERROR,
		"fingerprint child did not exit within %d ms", wait_ms);
	return false;
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
	/* True once the first unavailable status of the current run of failures
	 * has been logged at LOG_ERROR. A mismatch or success ends the run. */
	bool logged_unavailable = false;
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
			logged_unavailable = false;
			swaylock_log(LOG_DEBUG,
				"fingerprint not recognized (pam status %d)", status);
			if (!write_byte(notify_fd, FPRINT_MSG_MISMATCH)) {
				swaylock_log_errno(LOG_ERROR,
					"fingerprint: failed to notify parent");
				_exit(EXIT_FAILURE);
			}
		} else {
			/* No device, no enrolled prints, or a transient service error:
			 * back off quietly instead of flickering the invalid indicator.
			 * Log the first status of a run so the journal shows it; repeats
			 * of the same run stay at debug level. */
			if (!logged_unavailable) {
				swaylock_log(LOG_ERROR,
					"fingerprint unavailable (pam status %d)", status);
				logged_unavailable = true;
				if (!write_byte(notify_fd, FPRINT_MSG_UNAVAILABLE)) {
					swaylock_log_errno(LOG_ERROR,
						"fingerprint: failed to report unavailable service");
					_exit(EXIT_FAILURE);
				}
			} else {
				swaylock_log(LOG_DEBUG,
					"fingerprint unavailable (pam status %d)", status);
			}
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

bool fprint_spawn_child(pid_t *pid_out, int *pidfd_out, int *fd_out,
		fprint_child_hook child_hook, void *child_hook_data) {
	(void)pid_out;
	(void)pidfd_out;
	(void)fd_out;
	(void)child_hook;
	(void)child_hook_data;
	return false;
}

bool fprint_child_alive(int pidfd) {
	(void)pidfd;
	return false;
}

bool fprint_kill_child(int *pidfd, int wait_ms) {
	(void)wait_ms;
	if (pidfd) {
		*pidfd = -1;
	}
	return true;
}

void run_fprint_backend_child(int notify_fd) {
	(void)notify_fd;
	_exit(EXIT_FAILURE);
}

#endif
