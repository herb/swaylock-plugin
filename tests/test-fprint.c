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

	/* First unavailable result jumps straight to the 30s floor. */
	int delay = fprint_retry_delay_ms(0, PAM_AUTHINFO_UNAVAIL, 0);
	CHECK(delay == 30000);
	CHECK(fprint_retry_delay_ms(1000, PAM_AUTHINFO_UNAVAIL, 0) == 30000);

	/* Then it doubles, capped at 120s. */
	delay = fprint_retry_delay_ms(delay, PAM_AUTHINFO_UNAVAIL, 0);
	CHECK(delay == 60000);
	delay = fprint_retry_delay_ms(delay, PAM_AUTHINFO_UNAVAIL, 0);
	CHECK(delay == 120000);
	delay = fprint_retry_delay_ms(delay, PAM_AUTHINFO_UNAVAIL, 0);
	CHECK(delay == 120000);
}

static void test_idle_timeout_retries_promptly(void) {
	/* pam_fprintd reports its idle timeout like a missing device; an attempt
	 * that ran for a while was a timeout and must not back off. */
	CHECK(fprint_retry_delay_ms(120000, PAM_AUTHINFO_UNAVAIL, 30000) == 1000);
	CHECK(fprint_retry_delay_ms(0, PAM_AUTHINFO_UNAVAIL, 4999) == 30000);
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
	int notify_fd = -1;
	bool spawned = fprint_spawn_child(&pid, &notify_fd, probe_child_hook, &probe);
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
	fprint_kill_child(&pid);
	CHECK(pid == -1);
	if (reaped > 0) {
		int status = 0;
		CHECK(waitpid(reaped, &status, 0) == reaped);
	}
}

static void test_spawn_accepts_no_hook(void) {
	/* The hook is optional: a NULL hook must not crash the child. */
	pid_t pid = -1;
	int notify_fd = -1;
	CHECK(fprint_spawn_child(&pid, &notify_fd, NULL, NULL));
	if (notify_fd >= 0) {
		close(notify_fd);
	}
	pid_t reaped = pid;
	fprint_kill_child(&pid);
	if (reaped > 0) {
		int status = 0;
		CHECK(waitpid(reaped, &status, 0) == reaped);
	}
}

int main(void) {
	test_mismatch_retries_promptly();
	test_unavailable_backs_off();
	test_idle_timeout_retries_promptly();
	test_never_tight_loop();
	test_protocol_bytes_are_fixed_and_distinct();
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
