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
#define FPRINT_PAM_SERVICE_PATH "/etc/pam.d/" FPRINT_PAM_SERVICE

enum fprint_message {
	FPRINT_MSG_SUCCESS = 1,
	FPRINT_MSG_MISMATCH = 2,
};

/* True when the fingerprint PAM service is installed on this system. */
bool fprint_pam_service_available(void);

/*
 * Fork the fingerprint child. On success stores the child pid and the read end
 * of its notification pipe. The caller owns both and must eventually call
 * fprint_kill_child() and close the descriptor.
 */
bool fprint_spawn_child(pid_t *pid_out, int *fd_out);

/* Kill the child (if any) and reset *pid to -1. */
void fprint_kill_child(pid_t *pid);

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
int fprint_retry_delay_ms(int previous_ms, int pam_status);

#endif
