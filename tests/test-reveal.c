#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "reveal.h"
#include "unicode.h"

static int failures;

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

static double measure_ten_per_codepoint(const char *suffix, void *context) {
	(void)context;
	double width = 0.0;
	for (size_t pos = 0; suffix[pos] != '\0'; ) {
		width += 10.0;
		int size = utf8_size(suffix + pos);
		pos += size > 0 ? (size_t)size : 1;
	}
	return width;
}

static double measure_wide_emoji(const char *suffix, void *context) {
	(void)context;
	double width = 0.0;
	for (size_t pos = 0; suffix[pos] != '\0'; ) {
		int size = utf8_size(suffix + pos);
		width += size == 4 ? 40.0 : 10.0;
		pos += size > 0 ? (size_t)size : 1;
	}
	return width;
}

static struct reveal_layout test_layout(double scale) {
	return (struct reveal_layout){
		.scale = scale,
		.screen_width_px = 1000.0,
		.circle_cx_px = 500.0,
		.circle_cy_px = 70.0,
		.radius_px = 50.0,
		.thickness_px = 10.0,
	};
}

static void test_toggle_and_reset_events(void) {
	struct reveal_state state = {0};
	CHECK(!state.revealed);
	CHECK(reveal_apply(&state, REVEAL_EVENT_TOGGLE));
	CHECK(state.revealed);
	CHECK(reveal_apply(&state, REVEAL_EVENT_EDIT));
	CHECK(state.revealed);
	CHECK(!reveal_apply(&state, REVEAL_EVENT_TOGGLE));
	CHECK(!state.revealed);

	CHECK(reveal_apply(&state, REVEAL_EVENT_TOGGLE));
	CHECK(!reveal_apply(&state, REVEAL_EVENT_SUBMIT));
	CHECK(!state.revealed);

	CHECK(reveal_apply(&state, REVEAL_EVENT_TOGGLE));
	CHECK(!reveal_apply(&state, REVEAL_EVENT_CLEAR));
	CHECK(!state.revealed);

	CHECK(reveal_apply(&state, REVEAL_EVENT_TOGGLE));
	CHECK(!reveal_apply(&state, REVEAL_EVENT_AUTH_FAILURE));
	CHECK(!state.revealed);
}

static void test_editing_keeps_state_and_updates_text(void) {
	static const char *samples[] = {"a", "ab", "abc", "ab"};
	for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
		struct reveal_state state = {0};
		reveal_apply(&state, REVEAL_EVENT_TOGGLE);
		CHECK(reveal_apply(&state, REVEAL_EVENT_EDIT));
		CHECK(state.revealed);
		const char *text = samples[i];
		size_t offset = reveal_tail_offset(text, strlen(text), 5.0,
			measure_ten_per_codepoint, NULL);
		/* max 5 with 10-per-codepoint means no suffix fits; the newest
		 * codepoint is still shown. */
		CHECK(offset == strlen(text) - 1);
	}
}

static void test_centering_is_independent_of_indicator_position(void) {
	struct reveal_layout left = test_layout(1.0);
	struct reveal_layout right = test_layout(1.0);
	left.circle_cx_px = 100.0;
	right.circle_cx_px = 900.0;

	CHECK(reveal_text_x(&left, 200.0, 0.0) == 400.0);
	CHECK(reveal_text_x(&right, 200.0, 0.0) == 400.0);
	CHECK(reveal_text_x(&left, 200.0, 12.0) == 388.0);
	CHECK(reveal_text_x(&right, 200.0, 12.0) == 388.0);
}

static void test_tail_clipping_ascii(void) {
	const char *text = "abcdefghij";
	size_t offset = reveal_tail_offset(text, strlen(text), 50.0,
		measure_ten_per_codepoint, NULL);
	CHECK(offset == 5);
	CHECK(strcmp(text + offset, "fghij") == 0);

	offset = reveal_tail_offset(text, strlen(text), 1000.0,
		measure_ten_per_codepoint, NULL);
	CHECK(offset == 0);

	offset = reveal_tail_offset("", 0, 50.0,
		measure_ten_per_codepoint, NULL);
	CHECK(offset == 0);
}

static void test_tail_clipping_utf8_boundaries(void) {
	const char *text = "aé中🙂xyz";
	size_t offset = reveal_tail_offset(text, strlen(text), 40.0,
		measure_ten_per_codepoint, NULL);
	CHECK(strcmp(text + offset, "🙂xyz") == 0);
	CHECK(((unsigned char)text[offset] & 0xc0) != 0x80);

	offset = reveal_tail_offset(text, strlen(text), 15.0,
		measure_ten_per_codepoint, NULL);
	CHECK(strcmp(text + offset, "z") == 0);
	CHECK(((unsigned char)text[offset] & 0xc0) != 0x80);

	const char *wide = "abc🙂xyz";
	offset = reveal_tail_offset(wide, strlen(wide), 50.0,
		measure_wide_emoji, NULL);
	CHECK(strcmp(wide + offset, "xyz") == 0);
	CHECK(((unsigned char)wide[offset] & 0xc0) != 0x80);

	offset = reveal_tail_offset("🙂", strlen("🙂"), 10.0,
		measure_wide_emoji, NULL);
	CHECK(offset == 0);
}

static void test_hit_testing_at_integer_scale(void) {
	struct reveal_layout layout = test_layout(1.0);
	struct reveal_rect icon = reveal_icon_rect(&layout);
	double cx = icon.x + icon.width / 2.0;
	double cy = icon.y + icon.height / 2.0;

	CHECK(reveal_hit_test(&layout, cx, cy, 0.0, 0.0));
	CHECK(!reveal_hit_test(&layout, 100.0, 100.0, 0.0, 0.0));

	/* The hit rect is padded, so a point just outside the drawn icon still
	 * counts, but a point beyond the padding does not. */
	double padding = layout.radius_px * REVEAL_ICON_HIT_PADDING_RATIO;
	CHECK(reveal_hit_test(&layout,
		cx + icon.width / 2.0 + padding / 2.0, cy, 0.0, 0.0));
	CHECK(!reveal_hit_test(&layout,
		cx + icon.width / 2.0 + padding * 3.0, cy, 0.0, 0.0));
}

static void test_hit_testing_respects_scale_and_offset(void) {
	for (double scale = 1.0; scale <= 2.0; scale += 0.5) {
		struct reveal_layout layout = test_layout(scale);
		struct reveal_rect icon = reveal_icon_rect(&layout);
		double device_cx = icon.x + icon.width / 2.0;
		double device_cy = icon.y + icon.height / 2.0;
		double logical_cx = device_cx / scale;
		double logical_cy = device_cy / scale;

		CHECK(reveal_hit_test(&layout, logical_cx, logical_cy, 0.0, 0.0));

		double offset_x = 100.0;
		double offset_y = 100.0;
		CHECK(reveal_hit_test(&layout,
			logical_cx - offset_x, logical_cy - offset_y,
			offset_x, offset_y));
		CHECK(!reveal_hit_test(&layout,
			logical_cx - offset_x, logical_cy - offset_y,
			0.0, 0.0));
	}
}

int main(void) {
	test_toggle_and_reset_events();
	test_editing_keeps_state_and_updates_text();
	test_centering_is_independent_of_indicator_position();
	test_tail_clipping_ascii();
	test_tail_clipping_utf8_boundaries();
	test_hit_testing_at_integer_scale();
	test_hit_testing_respects_scale_and_offset();

	if (failures != 0) {
		fprintf(stderr, "%d reveal test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
