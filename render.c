#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "cairo.h"
#include "background-image.h"
#include "swaylock.h"
#include "log.h"
#include "reveal.h"

#define M_PI 3.14159265358979323846
const float TYPE_INDICATOR_RANGE = M_PI / 3.0f;

static void set_color_for_state(cairo_t *cairo, struct swaylock_state *state,
		struct swaylock_colorset *colorset) {
	if (state->input_state == INPUT_STATE_CLEAR) {
		cairo_set_source_u32(cairo, colorset->cleared);
	} else if (state->auth_state == AUTH_STATE_VALIDATING) {
		cairo_set_source_u32(cairo, colorset->verifying);
	} else if (state->auth_state == AUTH_STATE_INVALID) {
		cairo_set_source_u32(cairo, colorset->wrong);
	} else {
		if (state->xkb.caps_lock && state->args.show_caps_lock_indicator) {
			cairo_set_source_u32(cairo, colorset->caps_lock);
		} else if (state->xkb.caps_lock && !state->args.show_caps_lock_indicator &&
				state->args.show_caps_lock_text) {
			uint32_t inputtextcolor = state->args.colors.text.input;
			state->args.colors.text.input = state->args.colors.text.caps_lock;
			cairo_set_source_u32(cairo, colorset->input);
			state->args.colors.text.input = inputtextcolor;
		} else {
			cairo_set_source_u32(cairo, colorset->input);
		}
	}
}

static void surface_frame_handle_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	struct swaylock_surface *surface = data;

	wl_callback_destroy(callback);
	surface->frame = NULL;

	render(surface);
}

static const struct wl_callback_listener surface_frame_listener = {
	.done = surface_frame_handle_done,
};

static bool render_frame(struct swaylock_surface *surface);

void render(struct swaylock_surface *surface) {
	int buffer_width = surface->width * surface->scale;
	int buffer_height = surface->height * surface->scale;
	if (buffer_width == 0 || buffer_height == 0) {
		return; // not yet configured
	}

	if (!surface->has_buffer) {
		return; // background not yet ready
	}

	if (!surface->dirty || surface->frame) {
		// Nothing to do or frame already pending
		return;
	}

	render_frame(surface);
	surface->dirty = false;
	surface->frame = wl_surface_frame(surface->surface);
	wl_callback_add_listener(surface->frame, &surface_frame_listener, surface);
	wl_surface_commit(surface->surface);
}

static void configure_font_drawing_size(cairo_t *cairo,
		struct swaylock_state *state, enum wl_output_subpixel subpixel,
		double font_size) {
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(fo, to_cairo_subpixel_order(subpixel));

	cairo_set_font_options(cairo, fo);
	cairo_select_font_face(cairo, state->args.font,
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cairo, font_size);
	cairo_font_options_destroy(fo);
}

static void configure_font_drawing(cairo_t *cairo, struct swaylock_state *state,
		enum wl_output_subpixel subpixel, int arc_radius) {
	double font_size = state->args.font_size > 0
		? state->args.font_size : arc_radius / 3.0f;
	configure_font_drawing_size(cairo, state, subpixel, font_size);
}

static double reveal_font_size(struct swaylock_state *state, int arc_radius) {
	double indicator_size = state->args.font_size > 0
		? state->args.font_size : arc_radius / 3.0;
	/* Smaller than the indicator text, but never invisibly small. */
	double size = indicator_size * 0.72;
	return size < 8.0 ? 8.0 : size;
}

/* Build the eye glyph path. An open eye means the password is hidden; a slash
 * through it means the password is currently revealed. */
static void reveal_eye_path(cairo_t *cairo, struct reveal_rect rect,
		bool revealed) {
	double cx = rect.x + rect.width / 2.0;
	double cy = rect.y + rect.height / 2.0;
	double hw = rect.width / 2.0;
	double hh = rect.height / 2.0;

	cairo_new_sub_path(cairo);
	cairo_move_to(cairo, cx - hw, cy);
	cairo_curve_to(cairo, cx - hw * 0.5, cy - hh,
		cx + hw * 0.5, cy - hh, cx + hw, cy);
	cairo_curve_to(cairo, cx + hw * 0.5, cy + hh,
		cx - hw * 0.5, cy + hh, cx - hw, cy);
	cairo_close_path(cairo);

	cairo_new_sub_path(cairo);
	cairo_arc(cairo, cx, cy, hh * 0.42, 0, 2 * M_PI);

	if (revealed) {
		cairo_new_sub_path(cairo);
		cairo_move_to(cairo, cx - hw * 0.75, cy + hh * 0.85);
		cairo_line_to(cairo, cx + hw * 0.75, cy - hh * 0.85);
	}
}

static void draw_reveal_icon(cairo_t *cairo, struct reveal_rect rect,
		bool revealed, double scale) {
	double line_width = rect.height * 0.11;
	if (line_width < 1.5 * scale) {
		line_width = 1.5 * scale;
	}
	double shadow = 1.5 * scale;

	/* Subtle dark shadow so the control remains readable over video. */
	cairo_save(cairo);
	cairo_translate(cairo, shadow, shadow);
	cairo_set_line_width(cairo, line_width);
	cairo_set_source_rgba(cairo, 0, 0, 0, 0.65);
	reveal_eye_path(cairo, rect, revealed);
	cairo_stroke(cairo);
	cairo_restore(cairo);

	cairo_set_line_width(cairo, line_width);
	cairo_set_source_rgba(cairo, 1, 1, 1, 1);
	reveal_eye_path(cairo, rect, revealed);
	cairo_stroke(cairo);
}

static double measure_password_suffix(const char *suffix, void *context) {
	cairo_t *cairo = context;
	cairo_text_extents_t extents;
	cairo_text_extents(cairo, suffix, &extents);
	return extents.width;
}

static bool render_frame(struct swaylock_surface *surface) {
	struct swaylock_state *state = surface->state;

	// First, compute the text that will be drawn, if any, since this
	// determines the size/positioning of the surface

	char attempts[4]; // like i3lock: count no more than 999
	char *text = NULL;
	const char *layout_text = NULL;

	bool draw_indicator = state->args.show_indicator &&
		(state->auth_state != AUTH_STATE_IDLE ||
			state->input_state != INPUT_STATE_IDLE ||
			state->args.indicator_idle_visible);

	bool reveal_enabled = state->args.show_password_toggle;

	// The reveal control only exists while the indicator is visible. Drop the
	// revealed state with it so a later indicator does not resurrect stale
	// plaintext from an earlier interaction.
	if (!draw_indicator && surface->reveal.revealed) {
		reveal_apply(&surface->reveal, REVEAL_EVENT_CLEAR);
	}

	bool draw_reveal = reveal_enabled && draw_indicator;

	if (draw_indicator) {
		if (state->input_state == INPUT_STATE_CLEAR) {
			// This message has highest priority
			text = "Cleared";
		} else if (state->auth_state == AUTH_STATE_VALIDATING) {
			text = "Verifying";
		} else if (state->auth_state == AUTH_STATE_INVALID) {
			text = "Wrong";
		} else {
			// Caps Lock has higher priority
			if (state->xkb.caps_lock && state->args.show_caps_lock_text) {
				text = "Caps Lock";
			} else if (state->args.show_failed_attempts &&
					state->failed_attempts > 0) {
				if (state->failed_attempts > 999) {
					text = "999+";
				} else {
					snprintf(attempts, sizeof(attempts), "%d", state->failed_attempts);
					text = attempts;
				}
			}

			if (state->xkb.keymap) {
				xkb_layout_index_t num_layout = xkb_keymap_num_layouts(state->xkb.keymap);
				if (!state->args.hide_keyboard_layout &&
						(state->args.show_keyboard_layout || num_layout > 1)) {
					xkb_layout_index_t curr_layout = 0;

					// advance to the first active layout (if any)
					while (curr_layout < num_layout &&
						xkb_state_layout_index_is_active(state->xkb.state,
							curr_layout, XKB_STATE_LAYOUT_EFFECTIVE) != 1) {
						++curr_layout;
					}
					// will handle invalid index if none are active
					layout_text = xkb_keymap_layout_get_name(state->xkb.keymap, curr_layout);
				}
			}
		}
	}

	// Compute the size of the buffer needed
	int arc_radius = state->args.radius * surface->scale;
	int arc_thickness = state->args.thickness * surface->scale;
	int buffer_diameter = (arc_radius + arc_thickness) * 2;
	int buffer_width = buffer_diameter;
	int buffer_height = buffer_diameter;

	struct reveal_layout reveal_layout = {
		.scale = surface->scale,
		.screen_width_px = buffer_width,
		.circle_cx_px = buffer_width / 2.0,
		.circle_cy_px = buffer_diameter / 2.0,
		.radius_px = arc_radius,
		.thickness_px = arc_thickness,
	};
	double reveal_font_px = reveal_font_size(state, arc_radius);
	double reveal_text_top = 0.0;
	double layout_box_y = buffer_diameter;

	if (draw_reveal) {
		// Draw into a full-output-width subsurface so revealed text can be
		// centered on the screen even when --indicator-x-position moves the
		// indicator away from the center.
		buffer_width = surface->width * surface->scale;
		if (buffer_width < buffer_diameter) {
			buffer_width = buffer_diameter;
		}
		reveal_layout.screen_width_px = buffer_width;
		reveal_layout.circle_cx_px =
			(state->args.override_indicator_x_position
				? state->args.indicator_x_position
				: surface->width / 2.0) * surface->scale;

		struct reveal_rect icon = reveal_icon_rect(&reveal_layout);
		double gap = reveal_layout.radius_px * REVEAL_ICON_GAP_RATIO;
		reveal_text_top = icon.y + icon.height + gap;

		configure_font_drawing_size(state->test_cairo, state,
			surface->subpixel, reveal_font_px);
		cairo_font_extents_t reveal_fe;
		cairo_font_extents(state->test_cairo, &reveal_fe);
		double extra = reveal_text_top - buffer_diameter + reveal_fe.height +
			reveal_layout.radius_px * 0.2;

		if (layout_text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double box_padding = 4.0 * surface->scale;
			configure_font_drawing(state->test_cairo, state,
				surface->subpixel, arc_radius);
			cairo_text_extents(state->test_cairo, layout_text, &extents);
			cairo_font_extents(state->test_cairo, &fe);
			layout_box_y = buffer_diameter + extra;
			extra += fe.height + 2 * box_padding;
		}

		buffer_height = buffer_diameter + (int)ceil(extra);
	} else if (text || layout_text) {
		cairo_set_antialias(state->test_cairo, CAIRO_ANTIALIAS_BEST);
		configure_font_drawing(state->test_cairo, state, surface->subpixel, arc_radius);

		if (text) {
			cairo_text_extents_t extents;
			cairo_text_extents(state->test_cairo, text, &extents);
			if (buffer_width < extents.width) {
				buffer_width = extents.width;
			}
		}
		if (layout_text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double box_padding = 4.0 * surface->scale;
			cairo_text_extents(state->test_cairo, layout_text, &extents);
			cairo_font_extents(state->test_cairo, &fe);
			buffer_height += fe.height + 2 * box_padding;
			if (buffer_width < extents.width + 2 * box_padding) {
				buffer_width = extents.width + 2 * box_padding;
			}
		}
	}
	// Ensure buffer size is multiple of buffer scale - required by protocol
	if (draw_reveal) {
		int width_rem = buffer_width % surface->scale;
		if (width_rem != 0) {
			buffer_width += surface->scale - width_rem;
		}
		int height_rem = buffer_height % surface->scale;
		if (height_rem != 0) {
			buffer_height += surface->scale - height_rem;
		}
	} else {
		buffer_height += surface->scale - (buffer_height % surface->scale);
		buffer_width += surface->scale - (buffer_width % surface->scale);
	}

	int subsurf_xpos;
	int subsurf_ypos;

	// Center the indicator unless overridden by the user
	if (draw_reveal) {
		subsurf_xpos = 0;
	} else if (state->args.override_indicator_x_position) {
		subsurf_xpos = state->args.indicator_x_position -
			buffer_width / (2 * surface->scale) + 2 / surface->scale;
	} else {
		subsurf_xpos = surface->width / 2 -
			buffer_width / (2 * surface->scale) + 2 / surface->scale;
	}

	if (state->args.override_indicator_y_position) {
		subsurf_ypos = state->args.indicator_y_position -
			(state->args.radius + state->args.thickness);
	} else {
		subsurf_ypos = surface->height / 2 -
			(state->args.radius + state->args.thickness);
	}

	surface->indicator_subsurf_x = subsurf_xpos;
	surface->indicator_subsurf_y = subsurf_ypos;
	surface->reveal_hit = reveal_layout;

	struct pool_buffer *buffer = get_next_buffer(state->shm,
			surface->indicator_buffers, buffer_width, buffer_height);
	if (buffer == NULL) {
		swaylock_log(LOG_ERROR, "No buffer");
		return false;
	}
	surface->reveal_hit_valid = draw_reveal;

	// Render the buffer
	cairo_t *cairo = buffer->cairo;
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	cairo_identity_matrix(cairo);

	// Clear, including any region that held revealed text in a previous frame.
	cairo_save(cairo);
	cairo_set_source_rgba(cairo, 0, 0, 0, 0);
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cairo);
	cairo_restore(cairo);

	double circle_cx = draw_reveal
		? reveal_layout.circle_cx_px : (double)(buffer_width / 2);
	double circle_cy = buffer_diameter / 2.0;

	if (draw_indicator) {
		// Fill inner circle
		cairo_set_line_width(cairo, 0);
		cairo_arc(cairo, circle_cx, circle_cy,
				arc_radius - arc_thickness / 2, 0, 2 * M_PI);
		set_color_for_state(cairo, state, &state->args.colors.inside);
		cairo_fill_preserve(cairo);
		cairo_stroke(cairo);

		// Draw ring
		cairo_set_line_width(cairo, arc_thickness);
		cairo_arc(cairo, circle_cx, circle_cy, arc_radius,
				0, 2 * M_PI);
		set_color_for_state(cairo, state, &state->args.colors.ring);
		cairo_stroke(cairo);

		// Draw a message
		configure_font_drawing(cairo, state, surface->subpixel, arc_radius);
		set_color_for_state(cairo, state, &state->args.colors.text);

		if (text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double x, y;
			cairo_text_extents(cairo, text, &extents);
			cairo_font_extents(cairo, &fe);
			x = circle_cx -
				(extents.width / 2 + extents.x_bearing);
			y = circle_cy +
				(fe.height / 2 - fe.descent);

			cairo_move_to(cairo, x, y);
			cairo_show_text(cairo, text);
			cairo_close_path(cairo);
			cairo_new_sub_path(cairo);
		}

		// Typing indicator: Highlight random part on keypress
		if (state->input_state == INPUT_STATE_LETTER ||
				state->input_state == INPUT_STATE_BACKSPACE) {
			double highlight_start = state->highlight_start * (M_PI / 1024.0);
			cairo_arc(cairo, circle_cx, circle_cy,
					arc_radius, highlight_start,
					highlight_start + TYPE_INDICATOR_RANGE);
			if (state->input_state == INPUT_STATE_LETTER) {
				if (state->xkb.caps_lock && state->args.show_caps_lock_indicator) {
					cairo_set_source_u32(cairo, state->args.colors.caps_lock_key_highlight);
				} else {
					cairo_set_source_u32(cairo, state->args.colors.key_highlight);
				}
			} else {
				if (state->xkb.caps_lock && state->args.show_caps_lock_indicator) {
					cairo_set_source_u32(cairo, state->args.colors.caps_lock_bs_highlight);
				} else {
					cairo_set_source_u32(cairo, state->args.colors.bs_highlight);
				}
			}
			cairo_stroke(cairo);

			// Draw borders
			double inner_radius = buffer_diameter / 2.0 - arc_thickness * 1.5;
			double outer_radius = buffer_diameter / 2.0 - arc_thickness / 2.0;

			cairo_set_line_width(cairo, 2.0 * surface->scale);
			cairo_set_source_u32(cairo, state->args.colors.separator);
			cairo_move_to(cairo,
				circle_cx + cos(highlight_start) * inner_radius,
				circle_cy + sin(highlight_start) * inner_radius
			);
			cairo_line_to(cairo,
				circle_cx + cos(highlight_start) * outer_radius,
				circle_cy + sin(highlight_start) * outer_radius
			);
			cairo_stroke(cairo);

			cairo_move_to(cairo,
				circle_cx + cos(highlight_start + TYPE_INDICATOR_RANGE) * inner_radius,
				circle_cy + sin(highlight_start + TYPE_INDICATOR_RANGE) * inner_radius
			);
			cairo_line_to(cairo,
				circle_cx + cos(highlight_start + TYPE_INDICATOR_RANGE) * outer_radius,
				circle_cy + sin(highlight_start + TYPE_INDICATOR_RANGE) * outer_radius
			);
			cairo_stroke(cairo);
		}

		// Draw inner + outer border of the circle
		set_color_for_state(cairo, state, &state->args.colors.line);
		cairo_set_line_width(cairo, 2.0 * surface->scale);
		cairo_arc(cairo, circle_cx, circle_cy,
				arc_radius - arc_thickness / 2, 0, 2 * M_PI);
		cairo_stroke(cairo);
		cairo_arc(cairo, circle_cx, circle_cy,
				arc_radius + arc_thickness / 2, 0, 2 * M_PI);
		cairo_stroke(cairo);

		if (draw_reveal) {
			struct reveal_rect icon = reveal_icon_rect(&reveal_layout);
			draw_reveal_icon(cairo, icon, surface->reveal.revealed,
				surface->scale);

			if (surface->reveal.revealed && state->password.len > 0) {
				configure_font_drawing_size(cairo, state, surface->subpixel,
					reveal_font_px);
				double max_width = buffer_width * 0.8;
				size_t offset = reveal_tail_offset(state->password.buffer,
					state->password.len, max_width,
					measure_password_suffix, cairo);
				if (offset < state->password.len) {
					cairo_text_extents_t extents;
					cairo_font_extents_t fe;
					cairo_text_extents(cairo,
						state->password.buffer + offset, &extents);
					cairo_font_extents(cairo, &fe);
					double x = reveal_text_x(&reveal_layout,
						extents.width, extents.x_bearing);
					double y = reveal_text_top + fe.ascent;
					double shadow = 1.5 * surface->scale;

					cairo_save(cairo);
					cairo_translate(cairo, shadow, shadow);
					cairo_set_source_rgba(cairo, 0, 0, 0, 0.65);
					cairo_move_to(cairo, x, y);
					cairo_show_text(cairo,
						state->password.buffer + offset);
					cairo_restore(cairo);

					cairo_set_source_rgba(cairo, 1, 1, 1, 1);
					cairo_move_to(cairo, x, y);
					cairo_show_text(cairo,
						state->password.buffer + offset);
					cairo_new_sub_path(cairo);
				}
			}
		}

		// display layout text separately
		if (layout_text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double x, y;
			double box_padding = 4.0 * surface->scale;
			configure_font_drawing(cairo, state, surface->subpixel, arc_radius);
			cairo_text_extents(cairo, layout_text, &extents);
			cairo_font_extents(cairo, &fe);
			// upper left coordinates for box
			x = circle_cx - (extents.width / 2) - box_padding;
			y = layout_box_y;

			// background box
			cairo_rectangle(cairo, x, y,
				extents.width + 2.0 * box_padding,
				fe.height + 2.0 * box_padding);
			cairo_set_source_u32(cairo, state->args.colors.layout_background);
			cairo_fill_preserve(cairo);
			// border
			cairo_set_source_u32(cairo, state->args.colors.layout_border);
			cairo_stroke(cairo);

			// take font extents and padding into account
			cairo_move_to(cairo,
				x - extents.x_bearing + box_padding,
				y + (fe.height - fe.descent) + box_padding);
			cairo_set_source_u32(cairo, state->args.colors.layout_text);
			cairo_show_text(cairo, layout_text);
			cairo_new_sub_path(cairo);
		}
	}

	// Send Wayland requests
	wl_subsurface_set_position(surface->subsurface, subsurf_xpos, subsurf_ypos);

	wl_surface_set_buffer_scale(surface->child, surface->scale);
	wl_surface_attach(surface->child, buffer->buffer, 0, 0);
	wl_surface_damage_buffer(surface->child, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface->child);

	return true;
}
