#define _POSIX_C_SOURCE 200809L
#include "config.h"

#if HAVE_PAM

/* This file is only built with PAM support, but keep the PAM header and the
 * constants it provides behind the same guard as fprint.c so a
 * `-Dpam=disabled` tree never needs the PAM development headers. */
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
/* An unavailable reader backs off from 1s, doubling up to the cap. Starting
 * low matters after resume: fprintd briefly reports the reader unavailable
 * while it resumes the device, and a large first step left fingerprint
 * unlock dead for the seconds the user actually tries it. */
#define FPRINT_RETRY_BACKOFF_MS 1000
#define FPRINT_RETRY_BACKOFF_MAX_MS 120000
/* pam_fprintd reports its idle verify timeout with the same status as a
 * missing device. A missing device fails at once, so an attempt that ran at
 * least this long was a timeout and must be retried promptly; otherwise a
 * user walking up to the lock would be ignored for up to the backoff cap. */
#define FPRINT_TIMEOUT_THRESHOLD_MS 5000

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

int fprint_retry_delay_ms(int previous_ms, int pam_status, long elapsed_ms) {
	if (!fprint_status_is_unavailable(pam_status) ||
			elapsed_ms >= FPRINT_TIMEOUT_THRESHOLD_MS) {
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

#endif /* HAVE_PAM */
