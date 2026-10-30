#ifndef _SWAY_REVEAL_H
#define _SWAY_REVEAL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Geometry ratios for the password reveal control, relative to the indicator
 * ring radius. The control is drawn beneath the indicator circle.
 */
#define REVEAL_ICON_WIDTH_RATIO 0.90
#define REVEAL_ICON_HEIGHT_RATIO 0.55
#define REVEAL_ICON_GAP_RATIO 0.25
#define REVEAL_ICON_HIT_PADDING_RATIO 0.15

/* Per-surface password reveal state. */
struct reveal_state {
	bool revealed;
};

enum reveal_event {
	REVEAL_EVENT_TOGGLE,
	REVEAL_EVENT_EDIT,
	REVEAL_EVENT_SUBMIT,
	REVEAL_EVENT_CLEAR,
	REVEAL_EVENT_AUTH_FAILURE,
};

/*
 * Apply an event and return the new reveal state.
 *
 * TOGGLE flips the state, EDIT leaves it alone, and SUBMIT, CLEAR and
 * AUTH_FAILURE always hide the password.
 */
bool reveal_apply(struct reveal_state *state, enum reveal_event event);

struct reveal_rect {
	double x, y, width, height;
};

/*
 * Geometry of the reveal control in output-buffer device pixels.
 *
 * screen_width_px is the full output width; the reveal buffer spans it so the
 * password text can always be centered on the screen rather than around a
 * potentially off-center indicator.
 */
struct reveal_layout {
	double scale;
	double screen_width_px;
	double circle_cx_px;
	double circle_cy_px;
	double radius_px;
	double thickness_px;
};

struct reveal_rect reveal_icon_rect(const struct reveal_layout *layout);
struct reveal_rect reveal_icon_hit_rect(const struct reveal_layout *layout);
bool reveal_rect_contains(struct reveal_rect rect, double x, double y);

/*
 * Hit-test a pointer position given in surface-local logical coordinates.
 *
 * The layout is in indicator-buffer device pixels. indicator_origin_x/y is the
 * logical position of the indicator buffer in the pointer's surface: 0,0 when
 * the pointer is on the indicator subsurface itself, or the subsurface offset
 * when it is on the parent lock surface. It is subtracted before applying the
 * buffer scale. scale is the buffer scale (device pixels per logical pixel);
 * fractional values are accepted for completeness.
 */
bool reveal_hit_test(const struct reveal_layout *layout,
		double surface_x, double surface_y,
		double indicator_origin_x, double indicator_origin_y);

/* Horizontal origin that centers text on the screen, independent of the
 * indicator's x position. */
double reveal_text_x(const struct reveal_layout *layout,
		double text_width_px, double x_bearing);

/*
 * Return the byte offset of the longest suffix of text that fits in max_width.
 *
 * Clipping happens only on UTF-8 codepoint boundaries. measure() receives a
 * NUL-terminated suffix and returns its rendered width. If no suffix fits,
 * the final codepoint is returned so the newest character is still visible.
 * text must be NUL-terminated; len is the number of valid bytes to consider.
 */
size_t reveal_tail_offset(const char *text, size_t len, double max_width,
		double (*measure)(const char *suffix, void *context), void *context);

#endif
