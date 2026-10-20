#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <wayland-client.h>
#include "cairo.h"
#include "background-image.h"
#include "swaylock.h"
#include "log.h"


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
	struct swaylock_state *state = surface->state;

	int buffer_width = surface->width * surface->scale;
	int buffer_height = surface->height * surface->scale;
	if (buffer_width == 0 || buffer_height == 0) {
		return; // not yet configured
	}

	if (!surface->dirty || surface->frame) {
		// Nothing to do or frame already pending
		return;
	}

	bool need_destroy = false;
	struct pool_buffer buffer;

	if (buffer_width != surface->last_buffer_width ||
			buffer_height != surface->last_buffer_height) {
		need_destroy = true;
		if (!create_buffer(state->shm, &buffer, buffer_width, buffer_height,
				WL_SHM_FORMAT_ARGB8888)) {
			swaylock_log(LOG_ERROR,
				"Failed to create new buffer for frame background.");
			return;
		}

		cairo_t *cairo = buffer.cairo;
		cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

		cairo_save(cairo);
		cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_u32(cairo, state->args.colors.background);
		cairo_paint(cairo);
		if (surface->image && state->args.mode != BACKGROUND_MODE_SOLID_COLOR) {
			cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
			render_background_image(cairo, surface->image,
				state->args.mode, buffer_width, buffer_height);
		}
		cairo_restore(cairo);
		cairo_identity_matrix(cairo);

		wl_surface_attach(surface->surface, buffer.buffer, 0, 0);
		wl_surface_damage_buffer(surface->surface, 0, 0, INT32_MAX, INT32_MAX);
		need_destroy = true;

		surface->last_buffer_width = buffer_width;
		surface->last_buffer_height = buffer_height;
	}

	// It is possible for the surface scale to change even if the wl_buffer size hasn't
	wl_surface_set_buffer_scale(surface->surface, surface->scale);

	render_frame(surface);
	surface->dirty = false;
	surface->frame = wl_surface_frame(surface->surface);
	wl_callback_add_listener(surface->frame, &surface_frame_listener, surface);
	wl_surface_commit(surface->surface);

	if (need_destroy) {
		destroy_buffer(&buffer);
	}
}

static void draw_symbol(cairo_t *cairo, const char *symbol,
		double cx, double cy, double size) {
	cairo_text_extents_t extents;
	cairo_save(cairo);
	if (symbol == slot_seven) {
		cairo_select_font_face(cairo, "serif",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cairo, size * 1.3);
	} else {
		cairo_select_font_face(cairo, "emoji",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cairo, size);
	}
	cairo_text_extents(cairo, symbol, &extents);
	cairo_move_to(cairo, cx - (extents.width / 2 + extents.x_bearing),
		cy - (extents.height / 2 + extents.y_bearing));
	if (symbol == slot_seven) {
		cairo_text_path(cairo, symbol);
		cairo_set_source_rgb(cairo, 0.86, 0.1, 0.1);
		cairo_fill_preserve(cairo);
		cairo_set_line_width(cairo, size * 0.06);
		cairo_set_source_rgb(cairo, 1.0, 0.8, 0.2);
		cairo_stroke(cairo);
	} else {
		cairo_show_text(cairo, symbol);
	}
	cairo_new_path(cairo);
	cairo_restore(cairo);
}

static void draw_glow(cairo_t *cairo, double width, double height,
		const double color[3], double strength) {
	cairo_save(cairo);
	cairo_translate(cairo, width / 2, height / 2);
	cairo_scale(cairo, width / height, 1);
	cairo_pattern_t *glow = cairo_pattern_create_radial(0, 0, 0, 0, 0, height / 2);
	cairo_pattern_add_color_stop_rgba(glow, 0,
		color[0], color[1], color[2], 0.7 * strength);
	cairo_pattern_add_color_stop_rgba(glow, 1, color[0], color[1], color[2], 0);
	cairo_set_source(cairo, glow);
	cairo_paint(cairo);
	cairo_pattern_destroy(glow);
	cairo_restore(cairo);
}

// Particles reuse pre-rendered symbols; scaling color emoji every frame is slow
static cairo_surface_t *sprite(const char *symbol, int size) {
	static struct {
		const char *symbol;
		int size;
		cairo_surface_t *surface;
	} cache[16];
	static int next;
	for (size_t i = 0; i < sizeof(cache) / sizeof(*cache); i++) {
		if (cache[i].surface && cache[i].symbol == symbol && cache[i].size == size) {
			return cache[i].surface;
		}
	}
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size * 2, size * 2);
	cairo_t *cairo = cairo_create(surface);
	draw_symbol(cairo, symbol, size, size, size);
	cairo_destroy(cairo);
	if (cache[next].surface) {
		cairo_surface_destroy(cache[next].surface);
	}
	cache[next].symbol = symbol;
	cache[next].size = size;
	cache[next].surface = surface;
	next = (next + 1) % (sizeof(cache) / sizeof(*cache));
	return surface;
}

static void draw_particles(cairo_t *cairo, struct slot_machine *m,
		double cx, double cy, double size) {
	int sprite_size = size * 0.6;
	for (int i = 0; i < m->particle_count; i++) {
		struct slot_particle *p = &m->particles[i];
		cairo_save(cairo);
		cairo_translate(cairo, cx + p->x * size, cy + p->y * size);
		cairo_rotate(cairo, p->rotation);
		cairo_set_source_surface(cairo, sprite(p->symbol, sprite_size),
			-sprite_size, -sprite_size);
		cairo_paint(cairo);
		cairo_restore(cairo);
	}
}

static void draw_slots(cairo_t *cairo, struct swaylock_state *state,
		double width, double height, double size, double spacing) {
	struct slot_machine *m = &state->slots;
	if (!m->visible) {
		return;
	}
	double cx = width / 2, cy = height / 2;
	double travel = height / 2 + size;
	double strength = (double)m->glow / SLOT_GLOW_TICKS;
	draw_particles(cairo, m, cx, cy, size);
	cairo_save(cairo);
	cairo_rectangle(cairo, 0, 0, width, height);
	cairo_clip(cairo);
	if (m->glow > 0) {
		draw_glow(cairo, width, height, m->theme->glow, strength);
		if (m->shake) {
			cx += sin(m->glow * 1.7) * size * 0.1 * strength;
		}
	}
	set_color_for_state(cairo, state, &state->args.colors.text);
	for (int i = 0; i < 3; i++) {
		struct slot_reel *r = &m->reels[i];
		double x = cx + (i - 1) * spacing;
		if (r->prev) {
			draw_symbol(cairo, r->prev, x, cy + r->prev_offset * travel, size);
		}
		draw_symbol(cairo, r->symbol, x, cy + r->offset * travel, size);
	}
	cairo_restore(cairo);
}

static void configure_font_drawing(cairo_t *cairo, struct swaylock_state *state,
		enum wl_output_subpixel subpixel, int arc_radius) {
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
	cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(fo, to_cairo_subpixel_order(subpixel));

	cairo_set_font_options(cairo, fo);
	cairo_select_font_face(cairo, state->args.font,
		CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	if (state->args.font_size > 0) {
		cairo_set_font_size(cairo, state->args.font_size);
	} else {
		cairo_set_font_size(cairo, arc_radius / 3.0f);
	}
	cairo_font_options_destroy(fo);
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

	if (draw_indicator) {
		if (state->input_state == INPUT_STATE_CLEAR) {
			// This message has highest priority
			text = "clr";
		} else if (state->auth_state == AUTH_STATE_VALIDATING) {
			text = "...";
		} else if (state->auth_state == AUTH_STATE_INVALID) {
			text = "f";
		} else if (state->auth_state == AUTH_STATE_VALID) {
			text = "$$$";
		} else {
			// Caps Lock has higher priority
			if (state->xkb.caps_lock && state->args.show_caps_lock_text) {
				text = "caps";
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
	double slot_size = arc_radius * 0.7;
	double slot_spacing = slot_size * 1.4;
	bool text_below = text && state->slots.visible;
	if (buffer_width < 3 * slot_spacing) {
		buffer_width = 3 * slot_spacing;
	}
	// Room for win particles around the reels, kept a multiple of the scale
	int margin = 0;
	if (slots_effect_active(&state->slots)) {
		margin = (int)(SLOT_EFFECT_MARGIN * slot_size / surface->scale) * surface->scale;
	}

	if (text || layout_text) {
		cairo_set_antialias(state->test_cairo, CAIRO_ANTIALIAS_BEST);
		configure_font_drawing(state->test_cairo, state, surface->subpixel, arc_radius);

		if (text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			cairo_text_extents(state->test_cairo, text, &extents);
			cairo_font_extents(state->test_cairo, &fe);
			if (buffer_width < extents.width) {
				buffer_width = extents.width;
			}
			if (text_below) {
				buffer_height += fe.height;
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
	buffer_width += 2 * margin;
	buffer_height += 2 * margin;
	// Ensure buffer size is multiple of buffer scale - required by protocol
	buffer_height += surface->scale - (buffer_height % surface->scale);
	buffer_width += surface->scale - (buffer_width % surface->scale);

	int subsurf_xpos;
	int subsurf_ypos;

	// Center the indicator unless overridden by the user
	if (state->args.override_indicator_x_position) {
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
	subsurf_ypos -= margin / surface->scale;

	struct pool_buffer *buffer = get_next_buffer(state->shm,
			surface->indicator_buffers, buffer_width, buffer_height);
	if (buffer == NULL) {
		swaylock_log(LOG_ERROR, "No buffer");
		return false;
	}

	// Render the buffer
	cairo_t *cairo = buffer->cairo;
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);

	cairo_identity_matrix(cairo);

	// Clear
	cairo_save(cairo);
	cairo_set_source_rgba(cairo, 0, 0, 0, 0);
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cairo);
	cairo_restore(cairo);
	cairo_translate(cairo, 0, margin);

	if (draw_indicator) {
		draw_slots(cairo, state, buffer_width, buffer_diameter,
			slot_size, slot_spacing);

		// Draw a message
		configure_font_drawing(cairo, state, surface->subpixel, arc_radius);
		set_color_for_state(cairo, state, &state->args.colors.text);

		double text_height = 0;
		if (text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double x, y;
			cairo_text_extents(cairo, text, &extents);
			cairo_font_extents(cairo, &fe);
			x = (buffer_width / 2) -
				(extents.width / 2 + extents.x_bearing);
			if (text_below) {
				y = buffer_diameter + fe.height - fe.descent;
				text_height = fe.height;
			} else {
				y = (buffer_diameter / 2) +
					(fe.height / 2 - fe.descent);
			}

			cairo_move_to(cairo, x, y);
			cairo_show_text(cairo, text);
			cairo_close_path(cairo);
			cairo_new_sub_path(cairo);
		}

		// display layout text separately
		if (layout_text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			double x, y;
			double box_padding = 4.0 * surface->scale;
			cairo_text_extents(cairo, layout_text, &extents);
			cairo_font_extents(cairo, &fe);
			// upper left coordinates for box
			x = (buffer_width / 2) - (extents.width / 2) - box_padding;
			y = buffer_diameter + text_height;

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
