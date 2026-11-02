#define _POSIX_C_SOURCE 200809L
#include <security/pam_appl.h>
#include "fprint.h"

/*
 * Retry pacing for the fingerprint child.
 *
 * A sensor which is absent or has no enrolled prints must not be polled in a
 * tight loop: pam_fprintd returns PAM_AUTHINFO_UNAVAIL (or a similar service
 * error) immediately in that case. A plain no-match returns PAM_AUTH_ERR and
 * the user may simply try again, so a short pause is enough.
 */

#define FPRINT_RETRY_PROMPT_MS 1000
#define FPRINT_RETRY_BACKOFF_MS 30000
#define FPRINT_RETRY_BACKOFF_MAX_MS 120000

bool fprint_status_is_unavailable(int pam_status) {
	switch (pam_status) {
	case PAM_AUTHINFO_UNAVAIL:
	case PAM_USER_UNKNOWN:
	case PAM_CRED_UNAVAIL:
	case PAM_SYSTEM_ERR:
	case PAM_SERVICE_ERR:
		return true;
	default:
		return false;
	}
}

bool fprint_status_is_mismatch(int pam_status) {
	/* A finger that is not recognized, or the module giving up after too many
	 * failed attempts; both deserve the visible "invalid" indicator. */
	return pam_status == PAM_AUTH_ERR || pam_status == PAM_MAXTRIES;
}

int fprint_retry_delay_ms(int previous_ms, int pam_status) {
	if (!fprint_status_is_unavailable(pam_status)) {
		return FPRINT_RETRY_PROMPT_MS;
	}

	int delay = previous_ms;
	if (delay < FPRINT_RETRY_BACKOFF_MS) {
		delay = FPRINT_RETRY_BACKOFF_MS;
	} else if (delay < FPRINT_RETRY_BACKOFF_MAX_MS) {
		delay *= 2;
		if (delay > FPRINT_RETRY_BACKOFF_MAX_MS) {
			delay = FPRINT_RETRY_BACKOFF_MAX_MS;
		}
	}
	return delay;
}
