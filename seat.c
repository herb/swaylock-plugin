#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include "log.h"
#include "swaylock.h"
#include "seat.h"
#include "loop.h"

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct swaylock_seat *seat = data;
	struct swaylock_state *state = seat->state;

	struct xkb_keymap *keymap = NULL;
	struct xkb_state *xkb_state = NULL;

	switch (format) {
	case WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP:
		break;
	case WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1:;
		char *map_shm = mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
		if (map_shm == MAP_FAILED) {
			close(fd);
			swaylock_log(LOG_ERROR, "Unable to initialize keymap shm, aborting");
			exit(1);
		}
		keymap = xkb_keymap_new_from_buffer(
			state->xkb.context, map_shm, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		assert(keymap);
		munmap(map_shm, size - 1);

		xkb_state = xkb_state_new(keymap);
		assert(xkb_state);
		break;
	}

	close(fd);

	xkb_keymap_unref(state->xkb.keymap);
	xkb_state_unref(state->xkb.state);
	state->xkb.keymap = keymap;
	state->xkb.state = xkb_state;
}

static void stop_repeat(struct swaylock_seat *seat) {
	if (seat->repeat_timer) {
		loop_remove_timer(seat->state->eventloop, seat->repeat_timer);
		seat->repeat_timer = NULL;
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	// Who cares
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	// When focus leaves (e.g. the compositor takes it away across a
	// suspend/DPMS transition) the protocol guarantees no release events for
	// currently-held keys, so an armed key-repeat would otherwise linger and
	// fire on resume, injecting a phantom character into the password buffer.
	struct swaylock_seat *seat = data;
	stop_repeat(seat);
}

static void keyboard_repeat(void *data) {
	struct swaylock_seat *seat = data;
	struct swaylock_state *state = seat->state;
	seat->repeat_timer = loop_add_timer(
		state->eventloop, seat->repeat_period_ms, keyboard_repeat, seat);
	swaylock_handle_key(state, seat->repeat_sym, seat->repeat_codepoint);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t _key_state) {
	struct swaylock_seat *seat = data;
	struct swaylock_state *state = seat->state;
	if (state->xkb.state == NULL) {
		return;
	}

	enum wl_keyboard_key_state key_state = _key_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb.state, key + 8);
	uint32_t keycode = key_state == WL_KEYBOARD_KEY_STATE_PRESSED ?
		key + 8 : 0;
	uint32_t codepoint = xkb_state_key_get_utf32(state->xkb.state, keycode);
	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (state->grace_timer) {
			/* still in grace period, immediately unlock */
			swaylock_log(LOG_DEBUG, "Key press during grace period, unlocking");
			state->run_display = false;
			return;
		}
		swaylock_handle_key(state, sym, codepoint);
	}

	stop_repeat(seat);

	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED && seat->repeat_period_ms > 0) {
		seat->repeat_sym = sym;
		seat->repeat_codepoint = codepoint;
		seat->repeat_timer = loop_add_timer(
			seat->state->eventloop, seat->repeat_delay_ms, keyboard_repeat, seat);
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct swaylock_seat *seat = data;
	struct swaylock_state *state = seat->state;
	if (state->xkb.state == NULL) {
		return;
	}

	int layout_same = xkb_state_layout_index_is_active(state->xkb.state,
		group, XKB_STATE_LAYOUT_EFFECTIVE);
	xkb_state_update_mask(state->xkb.state,
		mods_depressed, mods_latched, mods_locked, 0, 0, group);
	int caps_lock = xkb_state_mod_name_is_active(state->xkb.state,
		XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED);
	if (caps_lock != state->xkb.caps_lock || !layout_same) {
		state->xkb.caps_lock = caps_lock;
		damage_state(state);
	}
	state->xkb.control = xkb_state_mod_name_is_active(state->xkb.state,
		XKB_MOD_NAME_CTRL,
		XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	struct swaylock_seat *seat = data;
	if (rate <= 0) {
		seat->repeat_period_ms = -1;
	} else {
		// Keys per second -> milliseconds between keys
		seat->repeat_period_ms = 1000 / rate;
	}
	seat->repeat_delay_ms = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static struct swaylock_surface *pointer_lookup_surface(
		struct swaylock_state *state, struct wl_surface *surface,
		bool *on_indicator) {
	*on_indicator = false;
	if (!surface) {
		return NULL;
	}
	struct swaylock_surface *candidate;
	wl_list_for_each(candidate, &state->surfaces, link) {
		if (candidate->surface == surface) {
			return candidate;
		}
		if (candidate->child == surface) {
			*on_indicator = true;
			return candidate;
		}
	}
	return NULL;
}

/* Show a normal pointer cursor when the reveal control is enabled. Prefer
 * wp_cursor_shape_v1 (compositor-provided theme) and fall back to
 * libwayland-cursor. With the option disabled this keeps the original hidden
 * cursor behaviour. */
static void set_pointer_cursor(struct swaylock_seat *seat, uint32_t serial,
		struct swaylock_surface *surface) {
	struct swaylock_state *state = seat->state;
	if (!seat->pointer) {
		return;
	}
	if (!state->args.show_password_toggle) {
		wl_pointer_set_cursor(seat->pointer, serial, NULL, 0, 0);
		return;
	}

	if (!seat->cursor_shape_pointer && state->cursor_shape_manager) {
		seat->cursor_shape_pointer = wp_cursor_shape_manager_v1_get_pointer(
			state->cursor_shape_manager, seat->pointer);
	}
	if (seat->cursor_shape_pointer) {
		wp_cursor_shape_device_v1_set_shape(seat->cursor_shape_pointer,
			serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
		return;
	}

	if (!state->shm) {
		wl_pointer_set_cursor(seat->pointer, serial, NULL, 0, 0);
		return;
	}

	int cursor_scale = (surface && surface->scale > 0) ? surface->scale : 1;
	int size = 24 * cursor_scale;
	if (!state->cursor_theme || state->cursor_theme_size != size) {
		if (state->cursor_theme) {
			wl_cursor_theme_destroy(state->cursor_theme);
		}
		state->cursor = NULL;
		state->cursor_theme = wl_cursor_theme_load(NULL, size, state->shm);
		state->cursor_theme_size = size;
	}
	if (!state->cursor && state->cursor_theme) {
		state->cursor = wl_cursor_theme_get_cursor(state->cursor_theme,
			"left_ptr");
		if (!state->cursor) {
			state->cursor = wl_cursor_theme_get_cursor(state->cursor_theme,
				"default");
		}
	}
	if (state->cursor && state->cursor->image_count > 0) {
		struct wl_cursor_image *image = state->cursor->images[0];
		if (!seat->cursor_surface) {
			seat->cursor_surface =
				wl_compositor_create_surface(state->compositor);
		}
		if (seat->cursor_surface) {
			wl_surface_set_buffer_scale(seat->cursor_surface, cursor_scale);
			wl_surface_attach(seat->cursor_surface,
				wl_cursor_image_get_buffer(image), 0, 0);
			wl_surface_damage_buffer(seat->cursor_surface,
				0, 0, INT32_MAX, INT32_MAX);
			wl_surface_commit(seat->cursor_surface);
			wl_pointer_set_cursor(seat->pointer, serial,
				seat->cursor_surface,
				image->hotspot_x / cursor_scale,
				image->hotspot_y / cursor_scale);
			return;
		}
	}
	wl_pointer_set_cursor(seat->pointer, serial, NULL, 0, 0);
}

static struct swaylock_surface *pointer_focus(struct swaylock_seat *seat,
		bool *on_indicator) {
	*on_indicator = false;
	if (!seat->pointer_wl_surface) {
		return NULL;
	}
	return pointer_lookup_surface(seat->state, seat->pointer_wl_surface,
		on_indicator);
}

static struct swaylock_surface *pointer_hits_reveal_icon(
		struct swaylock_seat *seat) {
	if (!seat->state->args.show_password_toggle) {
		return NULL;
	}
	bool on_indicator;
	struct swaylock_surface *surface = pointer_focus(seat, &on_indicator);
	if (!surface || !surface->reveal_hit_valid) {
		return NULL;
	}
	double x = wl_fixed_to_double(seat->pointer_x);
	double y = wl_fixed_to_double(seat->pointer_y);
	// The hit layout is in indicator-buffer pixels. Pointer coordinates on the
	// indicator subsurface are already buffer-local; on the parent lock
	// surface they are offset by the subsurface position.
	double origin_x = on_indicator ? 0.0 : surface->indicator_subsurf_x;
	double origin_y = on_indicator ? 0.0 : surface->indicator_subsurf_y;
	if (!reveal_hit_test(&surface->reveal_hit, x, y, origin_x, origin_y)) {
		return NULL;
	}
	return surface;
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaylock_seat *seat = data;
	seat->pointer_x = surface_x;
	seat->pointer_y = surface_y;
	seat->pointer_wl_surface = surface;
	bool on_indicator;
	set_pointer_cursor(seat, serial, pointer_focus(seat, &on_indicator));
	if (!seat->state->grace_timer) {
		return;
	}
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	seat->last_interval = current_time.tv_sec;
	seat->interval_start_x = surface_x;
	seat->interval_start_y = surface_y;
	seat->last_mouse_x = surface_x;
	seat->last_mouse_y = surface_y;
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct swaylock_seat *seat = data;
	seat->pointer_wl_surface = NULL;
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct swaylock_seat *seat = data;
	seat->pointer_x = surface_x;
	seat->pointer_y = surface_y;

	bool over_icon = pointer_hits_reveal_icon(seat) != NULL;
	if (seat->state->args.show_password_toggle) {
		show_indicator_for_pointer(seat->state);
	}

	if (!seat->state->grace_timer) {
		return;
	}
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	int64_t intv = current_time.tv_sec;
	if (intv != seat->last_interval) {
		seat->last_interval = intv;
		seat->interval_start_x = seat->last_mouse_x;
		seat->interval_start_y = seat->last_mouse_y;
	}
	float dx = wl_fixed_to_double(surface_x - seat->interval_start_x);
	float dy = wl_fixed_to_double(surface_y - seat->interval_start_y);
	float thresh = seat->state->args.grace_pointer_hysteresis;
	if (!over_icon && dx * dx + dy * dy >= thresh * thresh) {
		/* Mouse moved more than hysteresis value in a one-second time period;
		 * this is probably a deliberate action and not noise or a minor tremor */
		swaylock_log(LOG_DEBUG, "Significant mouse motion (%f logical px) during grace period, unlocking",
			sqrtf(dx * dx + dy * dy));
		seat->state->run_display = false;
	}
	seat->last_mouse_x = surface_x;
	seat->last_mouse_y = surface_y;
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t button_state) {
	struct swaylock_seat *seat = data;

	/* The first click on the eye icon hides the password on every output;
	 * later clicks toggle reveal on only the clicked output. Neither must
	 * unlock through the grace-period path. */
	struct swaylock_surface *surface = NULL;
	if (button == BTN_LEFT &&
			button_state == WL_POINTER_BUTTON_STATE_PRESSED &&
			(surface = pointer_hits_reveal_icon(seat)) != NULL) {
		if (reveal_take_global_hide(
				&seat->state->reveal_global_hide_done)) {
			struct swaylock_surface *other;
			wl_list_for_each(other, &seat->state->surfaces, link) {
				other->reveal.revealed = false;
				other->dirty = true;
				render(other);
			}
		} else {
			reveal_apply(&surface->reveal, REVEAL_EVENT_TOGGLE);
			surface->dirty = true;
			render(surface);
		}
		return;
	}

	if (seat->state->grace_timer) {
		/* unlock during grace period on click */
		swaylock_log(LOG_DEBUG, "Mouse button action during grace period, unlocking");
		seat->state->run_display = false;
	}
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	// Who cares
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
	// Who cares
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis_source) {
	// Who cares
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis) {
	// Who cares
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t discrete) {
	// Who cares
}

static void wl_pointer_axis_value120(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t value120) {
	// Who cares
}

static void wl_pointer_axis_relative_direction(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, uint32_t direction) {
	// Who cares
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
	.axis_value120 = wl_pointer_axis_value120,
	.axis_relative_direction = wl_pointer_axis_relative_direction,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	struct swaylock_seat *seat = data;
	if (seat->pointer) {
		if (seat->cursor_shape_pointer) {
			wp_cursor_shape_device_v1_destroy(seat->cursor_shape_pointer);
			seat->cursor_shape_pointer = NULL;
		}
		wl_pointer_release(seat->pointer);
		seat->pointer = NULL;
		seat->pointer_wl_surface = NULL;
		if (seat->cursor_surface) {
			wl_surface_destroy(seat->cursor_surface);
			seat->cursor_surface = NULL;
		}
	}
	if (seat->keyboard) {
		wl_keyboard_release(seat->keyboard);
		seat->keyboard = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER)) {
		seat->pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(seat->pointer, &pointer_listener, seat);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
		seat->keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
	}
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
		const char *name) {
	// Who cares
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};
