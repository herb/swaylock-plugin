#include "config.h"

#if !HAVE_PAM

/* The retry policy is expressed in PAM status constants, so it only exists in
 * a PAM-enabled build (meson does not build this test otherwise). Keep the file
 * compilable either way so `-Dpam=disabled` never needs the PAM headers. */
#include <stdio.h>

int main(void) {
	printf("fingerprint retry logic requires PAM; nothing to test\n");
	return 0;
}

#else /* HAVE_PAM */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <security/pam_appl.h>
#include "comm.h"
#include "fprint.h"

static int failures;

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

/*
 * set_cloexec() lives in main.c, which this unit-test binary does not link
 * (main.c drags in Wayland). fprint_spawn_child() needs it for its pipe, so
 * provide the same two-line helper here.
 */
bool set_cloexec(int fd) {
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		return false;
	}
	return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

static void test_mismatch_retries_promptly(void) {
	/* A recognized failure (no match) shows the indicator and retries fast. */
	CHECK(fprint_status_is_mismatch(PAM_AUTH_ERR));
	CHECK(fprint_status_is_mismatch(PAM_MAXTRIES));
	CHECK(!fprint_status_is_mismatch(PAM_AUTHINFO_UNAVAIL));

	/* The short delay is applied even after a long backoff. */
	int delay = fprint_retry_delay_ms(120000, PAM_AUTH_ERR, 0);
	CHECK(delay == 1000);
	CHECK(fprint_retry_delay_ms(0, PAM_AUTH_ERR, 0) == 1000);
}

static void test_unavailable_backs_off(void) {
	CHECK(fprint_status_is_unavailable(PAM_AUTHINFO_UNAVAIL));
	CHECK(fprint_status_is_unavailable(PAM_USER_UNKNOWN));
	CHECK(fprint_status_is_unavailable(PAM_SYSTEM_ERR));
	CHECK(!fprint_status_is_unavailable(PAM_AUTH_ERR));

	/* First unavailable result retries after 1s, so a reader fprintd is still
	 * resuming after suspend is picked up within seconds. */
	int delay = fprint_retry_delay_ms(0, PAM_AUTHINFO_UNAVAIL, 0);
	CHECK(delay == 1000);

	/* Then it doubles, capped at 120s. */
	const int ladder[] = {2000, 4000, 8000, 16000, 32000, 64000, 120000, 120000};
	for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		delay = fprint_retry_delay_ms(delay, PAM_AUTHINFO_UNAVAIL, 0);
		CHECK(delay == ladder[i]);
	}

	/* At least four attempts fall inside the first 15s. */
	int elapsed = 0, attempts = 0;
	delay = 0;
	while (true) {
		delay = fprint_retry_delay_ms(delay, PAM_AUTHINFO_UNAVAIL, 0);
		if (elapsed + delay > 15000) {
			break;
		}
		elapsed += delay;
		attempts++;
	}
	CHECK(attempts >= 4);
}

static void test_idle_timeout_retries_promptly(void) {
	/* pam_fprintd reports its idle timeout like a missing device; an attempt
	 * that ran for a while was a timeout and must not back off. */
	CHECK(fprint_retry_delay_ms(120000, PAM_AUTHINFO_UNAVAIL, 30000) == 1000);
	CHECK(fprint_retry_delay_ms(0, PAM_AUTHINFO_UNAVAIL, 4999) == 1000);
	CHECK(fprint_retry_delay_ms(1000, PAM_AUTHINFO_UNAVAIL, 4999) == 2000);
}

static void test_never_tight_loop(void) {
	/* Every failing status must produce at least a 1s pause. */
	const int statuses[] = {
		PAM_AUTH_ERR,
		PAM_MAXTRIES,
		PAM_AUTHINFO_UNAVAIL,
		PAM_USER_UNKNOWN,
		PAM_CRED_UNAVAIL,
		PAM_SYSTEM_ERR,
		PAM_SERVICE_ERR,
		PAM_PERM_DENIED,
	};
	for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
		CHECK(fprint_retry_delay_ms(0, statuses[i], 0) >= 1000);
	}
}

static void test_rearm_backoff_schedule(void) {
	/* Resume re-arm: 1s, 2s, 4s, 8s, 8s, then the budget is spent. */
	CHECK(fprint_rearm_retry_delay_ms(0) == 1000);
	CHECK(fprint_rearm_retry_delay_ms(1) == 2000);
	CHECK(fprint_rearm_retry_delay_ms(2) == 4000);
	CHECK(fprint_rearm_retry_delay_ms(3) == 8000);
	CHECK(fprint_rearm_retry_delay_ms(4) == 8000);
	CHECK(FPRINT_REARM_MAX_RETRIES == 5);
	CHECK(fprint_rearm_retry_delay_ms(5) == -1);
	CHECK(fprint_rearm_retry_delay_ms(100) == -1);
	CHECK(fprint_rearm_retry_delay_ms(-1) == -1);
}

static void test_rearm_window_and_first_delay(void) {
	/* The window runs from resume, and the first respawn waits 1.5 s. */
	CHECK(FPRINT_REARM_WINDOW_MS == 30000);
	CHECK(FPRINT_REARM_FIRST_DELAY_MS == 1500);
	CHECK(FPRINT_REARM_FIRST_DELAY_MS != fprint_rearm_retry_delay_ms(0));
}

/* A child that survived suspend holds a stale fprintd claim, so resume must
 * never keep it: the re-arm replaces it with a fresh child. */
static void test_resume_replaces_surviving_child(void) {
	CHECK(!fprint_resume_keeps_child());
	CHECK(FPRINT_REARM_FIRST_DELAY_MS > 0);
}

static void test_protocol_bytes_are_fixed_and_distinct(void) {
	CHECK(FPRINT_MSG_SUCCESS != FPRINT_MSG_MISMATCH);
	CHECK(FPRINT_MSG_SUCCESS == 1);
	CHECK(FPRINT_MSG_MISMATCH == 2);
}

/*
 * The child-side hook contract: fprint_spawn_child() must run the caller's hook
 * in the forked child, before the child enters PAM, with the exact data pointer
 * the parent supplied. main.c uses it to wipe the inherited password buffer,
 * which is why the hook has to run at all.
 */
struct hook_probe {
	int write_fd;
	const void *expected_data;
};

static void probe_child_hook(void *data) {
	struct hook_probe *probe = data;
	char byte = probe->expected_data == data ? 'H' : 'X';
	ssize_t written = write(probe->write_fd, &byte, 1);
	(void)written;
	/* Never enter PAM from the test: the hook is the whole point. */
	_exit(0);
}

static void test_spawn_runs_the_child_hook(void) {
	int probe_fds[2];
	CHECK(pipe(probe_fds) == 0);
	if (probe_fds[0] < 0) {
		return;
	}

	struct hook_probe probe = {
		.write_fd = probe_fds[1],
		.expected_data = &probe,
	};
	pid_t pid = -1;
	int pidfd = -1;
	int notify_fd = -1;
	bool spawned = fprint_spawn_child(&pid, &pidfd, &notify_fd, probe_child_hook, &probe);
	CHECK(spawned);

	/* Only the child should hold the write end now. */
	close(probe_fds[1]);

	if (spawned) {
		struct pollfd pfd = { .fd = probe_fds[0], .events = POLLIN };
		char byte = 0;
		int ready = poll(&pfd, 1, 5000);
		CHECK(ready == 1);
		if (ready == 1) {
			CHECK(read(probe_fds[0], &byte, 1) == 1);
			CHECK(byte == 'H');
		}
	}

	close(probe_fds[0]);
	if (notify_fd >= 0) {
		close(notify_fd);
	}
	pid_t reaped = pid;
	fprint_kill_child(&pidfd, 0);
	CHECK(pidfd == -1);
	if (reaped > 0) {
		int status = 0;
		CHECK(waitpid(reaped, &status, 0) == reaped);
	}
}

static void test_spawn_accepts_no_hook(void) {
	/* The hook is optional: a NULL hook must not crash the child. */
	pid_t pid = -1;
	int pidfd = -1;
	int notify_fd = -1;
	CHECK(fprint_spawn_child(&pid, &pidfd, &notify_fd, NULL, NULL));
	if (notify_fd >= 0) {
		close(notify_fd);
	}
	pid_t reaped = pid;
	fprint_kill_child(&pidfd, 0);
	if (reaped > 0) {
		int status = 0;
		CHECK(waitpid(reaped, &status, 0) == reaped);
	}
}

/* The sleep barrier must report the child as gone and release its pidfd. */
static void test_kill_barrier_confirms_exit(void) {
	pid_t pid = -1;
	int pidfd = -1;
	int notify_fd = -1;
	if (!fprint_spawn_child(&pid, &pidfd, &notify_fd, NULL, NULL)) {
		return; /* no fingerprint service in the test environment */
	}
	CHECK(fprint_child_alive(pidfd));
	CHECK(fprint_kill_child(&pidfd, 1000));
	CHECK(pidfd == -1);
	int status;
	waitpid(pid, &status, 0);
	close(notify_fd);
}

/* A child may not be spawned while a suspend is under way, nor before the
 * sleep watcher has said whether one is. */
static void test_sleep_gate_blocks_spawn(void) {
	CHECK(fprint_spawn_allowed(false, false, false));
	CHECK(!fprint_spawn_allowed(true, false, false));
	CHECK(!fprint_spawn_allowed(false, true, false));
	CHECK(!fprint_spawn_allowed(true, true, false));
	CHECK(!fprint_spawn_allowed(false, false, true));
}

/* Only a child spawned under FPRINT_LATE_SPAWN_MS before ENTER is stopped. */
static void test_late_spawn_window(void) {
	CHECK(FPRINT_LATE_SPAWN_MS == 1000);
	CHECK(FPRINT_SLEEP_STATE_WAIT_MS == 3000);
	CHECK(fprint_spawn_is_too_late(1000, 1000));
	CHECK(fprint_spawn_is_too_late(1000, 1999));
	CHECK(!fprint_spawn_is_too_late(1000, 2000));
	CHECK(!fprint_spawn_is_too_late(1000, 9000));
}

/* The startup report must be distinct from the sleep messages it is mixed with. */
static void test_idle_byte_is_distinct(void) {
	CHECK(SLEEP_COMM_IDLE != SLEEP_COMM_ENTER);
	CHECK(SLEEP_COMM_IDLE != SLEEP_COMM_RESUME);
	CHECK(SLEEP_COMM_IDLE != SLEEP_COMM_ACK);
	CHECK(SLEEP_COMM_ENTER != SLEEP_COMM_RESUME);
	CHECK(SLEEP_COMM_ENTER != SLEEP_COMM_ACK);
	CHECK(SLEEP_COMM_RESUME != SLEEP_COMM_ACK);
}

int main(void) {
	test_sleep_gate_blocks_spawn();
	test_late_spawn_window();
	test_idle_byte_is_distinct();
	test_mismatch_retries_promptly();
	test_unavailable_backs_off();
	test_idle_timeout_retries_promptly();
	test_never_tight_loop();
	test_kill_barrier_confirms_exit();
	test_protocol_bytes_are_fixed_and_distinct();
	test_rearm_backoff_schedule();
	test_rearm_window_and_first_delay();
	test_resume_replaces_surviving_child();
	test_spawn_runs_the_child_hook();
	test_spawn_accepts_no_hook();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all fingerprint checks passed\n");
	return 0;
}

#endif /* HAVE_PAM */
