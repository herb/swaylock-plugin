#include <string.h>
#include "reveal.h"
#include "unicode.h"

bool reveal_apply(struct reveal_state *state, enum reveal_event event) {
	switch (event) {
	case REVEAL_EVENT_TOGGLE:
		state->revealed = !state->revealed;
		break;
	case REVEAL_EVENT_EDIT:
		/* Typing or deleting changes the text, not the reveal state. */
		break;
	case REVEAL_EVENT_SUBMIT:
	case REVEAL_EVENT_CLEAR:
	case REVEAL_EVENT_AUTH_FAILURE:
		state->revealed = false;
		break;
	}
	return state->revealed;
}

bool reveal_take_global_hide(bool *global_hide_done) {
	if (*global_hide_done) {
		return false;
	}
	*global_hide_done = true;
	return true;
}

struct reveal_rect reveal_icon_rect(const struct reveal_layout *layout) {
	double gap = layout->radius_px * REVEAL_ICON_GAP_RATIO;
	double width = layout->radius_px * REVEAL_ICON_WIDTH_RATIO;
	double height = layout->radius_px * REVEAL_ICON_HEIGHT_RATIO;
	double center_x = layout->circle_cx_px;
	double center_y = layout->circle_cy_px + layout->radius_px +
		layout->thickness_px / 2.0 + gap + height / 2.0;
	return (struct reveal_rect){
		.x = center_x - width / 2.0,
		.y = center_y - height / 2.0,
		.width = width,
		.height = height,
	};
}

struct reveal_rect reveal_icon_hit_rect(const struct reveal_layout *layout) {
	struct reveal_rect rect = reveal_icon_rect(layout);
	double padding = layout->radius_px * REVEAL_ICON_HIT_PADDING_RATIO;
	rect.x -= padding;
	rect.y -= padding;
	rect.width += 2.0 * padding;
	rect.height += 2.0 * padding;
	return rect;
}

bool reveal_rect_contains(struct reveal_rect rect, double x, double y) {
	return x >= rect.x && x < rect.x + rect.width &&
		y >= rect.y && y < rect.y + rect.height;
}

bool reveal_hit_test(const struct reveal_layout *layout,
		double surface_x, double surface_y,
		double indicator_origin_x, double indicator_origin_y) {
	double x = (surface_x - indicator_origin_x) * layout->scale;
	double y = (surface_y - indicator_origin_y) * layout->scale;
	return reveal_rect_contains(reveal_icon_hit_rect(layout), x, y);
}

double reveal_text_x(const struct reveal_layout *layout,
		double text_width_px, double x_bearing) {
	return (layout->screen_width_px - text_width_px) / 2.0 - x_bearing;
}

size_t reveal_tail_offset(const char *text, size_t len, double max_width,
		double (*measure)(const char *suffix, void *context), void *context) {
	if (text == NULL || len == 0) {
		return 0;
	}

	size_t last = 0;
	for (size_t pos = 0; pos < len; ) {
		last = pos;
		if (measure(text + pos, context) <= max_width) {
			return pos;
		}
		int size = utf8_size(text + pos);
		pos += size > 0 ? (size_t)size : 1;
	}

	/* Even the final codepoint is wider than the limit; show it rather than
	 * hiding all input. */
	return last;
}
