#ifndef _SWAYLOCK_FPRINT_H
#define _SWAYLOCK_FPRINT_H

#include <stdbool.h>
#include <sys/types.h>

/*
 * Concurrent fingerprint authentication backend.
 *
 * The locker starts a child which loops pam_authenticate() against the
 * dedicated PAM service below. The child never receives password data; it only
 * sends one of the fixed bytes in enum fprint_message to the parent, whose read
 * end is registered in the event loop. The parent only treats a success byte
 * arriving on that specific descriptor as an unlock request.
 */

#define FPRINT_PAM_SERVICE "swaylock-plugin-fprint"
/* Upper bound on blocking the event loop at suspend. */
#define FPRINT_SLEEP_KILL_WAIT_MS 1000
#define FPRINT_PAM_SERVICE_PATH "/etc/pam.d/" FPRINT_PAM_SERVICE

enum fprint_message {
	FPRINT_MSG_SUCCESS = 1,
	FPRINT_MSG_MISMATCH = 2,
};

/* True when the fingerprint PAM service is installed on this system. */
bool fprint_pam_service_available(void);

/*
 * Hook that runs in the forked fingerprint child, once, before the child enters
 * PAM. Since the child inherits the parent's address space it must wipe any
 * parent state it should never hold; main.c passes its password buffer. The
 * hook runs between fork() and exec-free PAM code, so it may only touch memory
 * it was handed — no allocation, no logging setup, no event loop.
 */
typedef void (*fprint_child_hook)(void *data);

/*
 * Fork the fingerprint child. On success stores its pid (optional, for tests),
 * a pidfd, and the read end of its notification pipe. `child_hook` (optional)
 * runs in the child before PAM. The child is signalled and watched only via
 * the pidfd, so pid reuse after the kernel reaps it is harmless. The caller
 * must eventually call fprint_kill_child() and close the pipe descriptor.
 */
bool fprint_spawn_child(pid_t *pid_out, int *pidfd_out, int *fd_out,
		fprint_child_hook child_hook, void *child_hook_data);

/* SIGTERM the child. With wait_ms <= 0, close the pidfd and return true at
 * once. With wait_ms > 0, wait (wall-clock bound, SIGKILL halfway) for it to
 * exit: returns true and closes the pidfd once it has, or false and keeps the
 * pidfd if it is still alive at the deadline. */
bool fprint_kill_child(int *pidfd, int wait_ms);

/* True while the child behind `pidfd` has not exited. */
bool fprint_child_alive(int pidfd);

/* Entry point for the forked child; never returns. */
void run_fprint_backend_child(int notify_fd);

/* Retry policy (pure logic, unit tested). */
bool fprint_status_is_unavailable(int pam_status);
bool fprint_status_is_mismatch(int pam_status);

/*
 * Delay in milliseconds before the next authentication attempt. Consecutive
 * unavailable-service statuses back off from 30s up to a 120s cap; a
 * mismatch/no-match retries promptly (1s).
 */
int fprint_retry_delay_ms(int previous_ms, int pam_status, long elapsed_ms);

#endif
