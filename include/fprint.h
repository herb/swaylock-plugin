#ifndef _SWAYLOCK_FPRINT_H
#define _SWAYLOCK_FPRINT_H

#include <stdbool.h>
#include <stdint.h>
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
#define FPRINT_PAM_SERVICE_PATH "/etc/pam.d/" FPRINT_PAM_SERVICE

enum fprint_message {
	FPRINT_MSG_SUCCESS = 1,
	FPRINT_MSG_MISMATCH = 2,
	FPRINT_MSG_UNAVAILABLE = 3,
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
 * unavailable-service statuses back off from 1s, doubling up to a 120s cap; a
 * mismatch/no-match retries promptly (1s).
 */
int fprint_retry_delay_ms(int previous_ms, int pam_status, long elapsed_ms);

/*
 * Resume re-arm policy (pure logic, unit tested). fprintd suspends and resumes
 * the device itself, so the child is left running across suspend. On resume the
 * locker opens a FPRINT_REARM_WINDOW_MS window. Inside it, a child that has
 * exited, or exits for any reason other than a successful auth, is respawned.
 *
 * The first respawn after resume waits FPRINT_REARM_FIRST_DELAY_MS. Each later
 * respawn waits the backoff ladder: `attempt` counts retries already scheduled
 * (0 for the first ladder step). The function returns that delay in ms, or -1
 * once the FPRINT_REARM_MAX_RETRIES budget is spent.
 */
#define FPRINT_REARM_WINDOW_MS 30000
#define FPRINT_REARM_FIRST_DELAY_MS 1500
#define FPRINT_REARM_MAX_RETRIES 5
int fprint_rearm_retry_delay_ms(int attempt);

/*
 * Whether a fingerprint child that survived suspend is kept on resume. It is
 * not: such a child holds a stale fprintd claim, so resume always replaces it
 * (see rearm_fprint_on_resume in main.c). Pure logic, unit tested.
 */
bool fprint_resume_keeps_child(void);

/*
 * Sleep-state gating of the fingerprint child (pure logic, unit tested).
 *
 * No child is spawned while a suspend is in progress (PrepareForSleep(true)
 * seen, no resume yet). A locker started while one is under way learns it from
 * the sleep watcher's startup report, so the first spawn also waits for that
 * report, for at most FPRINT_SLEEP_STATE_WAIT_MS, after which the locker spawns
 * anyway and logs it.
 *
 * A child spawned within FPRINT_LATE_SPAWN_MS before an ENTER is stopped: the
 * sleep began while it was still starting, and RESUME respawns it.
 */
#define FPRINT_SLEEP_STATE_WAIT_MS 3000
#define FPRINT_LATE_SPAWN_MS 1000
bool fprint_spawn_allowed(bool sleeping, bool awaiting_sleep_state,
		bool lid_closed);
bool fprint_spawn_is_too_late(int64_t spawned_ms, int64_t now_ms);

#endif
