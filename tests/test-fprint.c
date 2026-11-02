#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
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

int main(void) {
	test_mismatch_retries_promptly();
	test_unavailable_backs_off();
	test_idle_timeout_retries_promptly();
	test_never_tight_loop();
	test_protocol_bytes_are_fixed_and_distinct();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all fingerprint checks passed\n");
	return 0;
}
