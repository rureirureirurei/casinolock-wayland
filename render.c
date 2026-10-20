#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <wayland-client.h>
#include "cairo.h"
#include "background-image.h"
#include "swaylock.h"
#include "log.h"

#define M_PI 3.14159265358979323846
const float TYPE_INDICATOR_RANGE = M_PI / 3.0f;
const float EMOJI_DROP_SPEED = 1800.0; // pixels per second
const float EMOJI_SETTLE_THRESHOLD = 2.0; // pixels

// Particle physics constants
const float PARTICLE_GRAVITY = 500.0; // pixels per second squared
const float PARTICLE_DELTA_TIME = 0.016; // 60 FPS
const float BOUNCE_DAMPENING = 1; // Velocity multiplier on bounce
const int MAX_BOUNCES = 1; // Number of bounces before particle dies

// Forward declarations
static void update_particle_system(struct particle_system *system, int screen_width, int screen_height);

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

	// Only re-render background when size changes (not for particles!)
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

static void update_emoji_animation(struct swaylock_state *state, float target_y, int screen_height) {
	if (!state->emoji_animating && !state->has_old_emojis) {
		return;
	}

	// Use fixed timestep (assuming ~60 FPS = 16.67ms)
	float delta_time = 0.016;
	float movement = EMOJI_DROP_SPEED * delta_time;
	bool all_settled = true;
	bool old_still_visible = false;

	state->emoji_target_y = target_y;

	// Update new emojis (dropping from top to center)
	if (state->emoji_animating) {
		for (int i = 0; i < 3; i++) {
			float distance = state->emoji_target_y - state->emoji_y_positions[i];

			if (fabs(distance) > EMOJI_SETTLE_THRESHOLD) {
				// Move toward target
				state->emoji_y_positions[i] += movement;
				if (state->emoji_y_positions[i] > state->emoji_target_y) {
					state->emoji_y_positions[i] = state->emoji_target_y;
				}
				all_settled = false;
			} else {
				state->emoji_y_positions[i] = state->emoji_target_y;
			}
		}

		if (all_settled) {
			state->emoji_animating = false;
		}
	}

	// Update old emojis (falling off bottom)
	if (state->has_old_emojis) {
		for (int i = 0; i < 3; i++) {
			state->old_emoji_y_positions[i] += movement;
			// Check if still on screen (with some margin)
			if (state->old_emoji_y_positions[i] < screen_height + 100) {
				old_still_visible = true;
			}
		}

		if (!old_still_visible) {
			state->has_old_emojis = false;
		}
	}
}

// Particle physics engine
static void update_particle_physics(struct emoji_particle *particle, int screen_width, int screen_height) {
	// Apply velocity to position
	particle->x += particle->vx * PARTICLE_DELTA_TIME;
	particle->y += particle->vy * PARTICLE_DELTA_TIME;

	// Apply gravity to velocity
	particle->vy += PARTICLE_GRAVITY * PARTICLE_DELTA_TIME;

	// Update rotation
	particle->rotation += particle->rotation_speed * PARTICLE_DELTA_TIME;

	// Increment age
	particle->age++;

	// Check for wall collisions and bounce
	bool bounced = false;

	// Left wall collision
	if (particle->x < 0) {
		particle->x = 0;
		particle->vx = -particle->vx * BOUNCE_DAMPENING;
		bounced = true;
	}

	// Right wall collision
	if (particle->x > screen_width) {
		particle->x = screen_width;
		particle->vx = -particle->vx * BOUNCE_DAMPENING;
		bounced = true;
	}

	// Bottom wall collision
	if (particle->y > screen_height) {
		particle->y = screen_height;
		particle->vy = -particle->vy * BOUNCE_DAMPENING;
		bounced = true;
	}

	// Increment bounce counter
	if (bounced) {
		particle->bounce_count++;
		// Deactivate after max bounces
		if (particle->bounce_count >= MAX_BOUNCES) {
			particle->active = false;
		}
	}

	// Check if off-screen at top (particles flying upward off screen)
	if (particle->y < -100) {
		particle->active = false;
	}
}

static void spawn_particles_for_frame(struct particle_system *system) {
	if (!system->strategy) {
		return;
	}

	// Check if any particles should spawn this frame
	for (int i = 0; i < system->strategy->particle_count; i++) {
		struct particle_spawn_def *def = &system->strategy->spawn_defs[i];

		if (def->spawn_frame == system->current_frame) {
			// Find an inactive particle slot
			for (int j = 0; j < MAX_PARTICLES; j++) {
				if (!system->particles[j].active) {
					// Spawn particle
					system->particles[j].x = def->spawn_x;
					system->particles[j].y = def->spawn_y;
					system->particles[j].vx = def->velocity_x;
					system->particles[j].vy = def->velocity_y;
					system->particles[j].rotation = 0.0;
					system->particles[j].rotation_speed = def->rotation_speed;
					strcpy(system->particles[j].emoji, def->emoji);
					system->particles[j].age = 0;
					system->particles[j].bounce_count = 0;
					system->particles[j].active = true;
					system->active_count++;
					break;
				}
			}
		}
	}
}

static void update_particle_system(struct particle_system *system, int screen_width, int screen_height) {
	if (!system->active) {
		return;
	}

	// Spawn particles for current frame
	spawn_particles_for_frame(system);

	// Update all active particles
	system->active_count = 0;
	for (int i = 0; i < MAX_PARTICLES; i++) {
		if (system->particles[i].active) {
			update_particle_physics(&system->particles[i], screen_width, screen_height);
			if (system->particles[i].active) {
				system->active_count++;
			}
		}
	}

	// Advance frame counter
	system->current_frame++;

	// Check if animation is complete
	if (system->current_frame > system->strategy->total_frames && system->active_count == 0) {
		system->active = false;
	}
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
	char *emoji_display = NULL;

	bool draw_indicator = state->args.show_indicator &&
		(state->auth_state != AUTH_STATE_IDLE ||
			state->input_state != INPUT_STATE_IDLE ||
			state->args.indicator_idle_visible);

	// Create emoji display string (3 emojis with spaces)
	if (state->has_emojis) {
		// Allocate space for: emoji1 + space + emoji2 + space + emoji3 + null
		// Max 4 bytes per emoji * 3 + 2 spaces + 1 null = 15 bytes
		emoji_display = malloc(20);
		if (emoji_display) {
			snprintf(emoji_display, 20, "%s %s %s",
				state->slot_emojis[0],
				state->slot_emojis[1],
				state->slot_emojis[2]);
		}
	}

	if (draw_indicator) {
		if (state->input_state == INPUT_STATE_CLEAR) {
			// This message has highest priority
			text = "clr";
		} else if (state->auth_state == AUTH_STATE_VALIDATING) {
			text = "...";
		} else if (state->auth_state == AUTH_STATE_INVALID) {
			text = "f";
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

	// Check if particles are active - if so, expand buffer to full screen
	bool particles_active = state->celebration_particles.active ||
	                        state->celebration_particles.active_count > 0;

	// Compute the size of the buffer needed
	int arc_radius = state->args.radius * surface->scale;
	int arc_thickness = state->args.thickness * surface->scale;
	int buffer_diameter = (arc_radius + arc_thickness) * 2;
	int buffer_width, buffer_height;

	if (particles_active) {
		// Expand to full screen for particles
		buffer_width = surface->width * surface->scale;
		buffer_height = surface->height * surface->scale;
		// Update particle physics
		update_particle_system(&state->celebration_particles, buffer_width, buffer_height);
	} else {
		// Normal indicator size
		buffer_width = buffer_diameter;
		buffer_height = buffer_diameter;
	}

	if (text || layout_text || emoji_display) {
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
			float box_padding = 4.0 * surface->scale;
			cairo_text_extents(state->test_cairo, layout_text, &extents);
			cairo_font_extents(state->test_cairo, &fe);
			buffer_height += fe.height + 2 * box_padding;
			if (buffer_width < extents.width + 2 * box_padding) {
				buffer_width = extents.width + 2 * box_padding;
			}
		}
		if (emoji_display) {
			// Use emoji font for size calculation too
			cairo_select_font_face(state->test_cairo, "Noto Color Emoji",
				CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
			uint32_t emoji_size = state->args.font_size > 0 ? state->args.font_size * 3 : (uint32_t)arc_radius;
			cairo_set_font_size(state->test_cairo, emoji_size);

			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			cairo_text_extents(state->test_cairo, emoji_display, &extents);
			cairo_font_extents(state->test_cairo, &fe);
			if (buffer_width < extents.width) {
				buffer_width = extents.width;
			}
			// Add height for emojis if indicator is hidden
			if (!draw_indicator) {
				buffer_height = fe.height * 2;
			}
		}
	}
	// Ensure buffer size is multiple of buffer scale - required by protocol
	buffer_height += surface->scale - (buffer_height % surface->scale);
	buffer_width += surface->scale - (buffer_width % surface->scale);

	int subsurf_xpos;
	int subsurf_ypos;

	// Position subsurface
	if (particles_active) {
		// Full screen overlay - position at top-left
		subsurf_xpos = 0;
		subsurf_ypos = 0;
	} else {
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
	}

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

	if (draw_indicator) {
		// Fill inner circle
		cairo_set_line_width(cairo, 0);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2,
				arc_radius - arc_thickness / 2, 0, 2 * M_PI);
		set_color_for_state(cairo, state, &state->args.colors.inside);
		cairo_fill_preserve(cairo);
		cairo_stroke(cairo);

		// Draw ring
		cairo_set_line_width(cairo, arc_thickness);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2, arc_radius,
				0, 2 * M_PI);
		set_color_for_state(cairo, state, &state->args.colors.ring);
		cairo_stroke(cairo);

		// Draw a message
		configure_font_drawing(cairo, state, surface->subpixel, arc_radius);
		set_color_for_state(cairo, state, &state->args.colors.text);

		if (text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			float x, y;
			cairo_text_extents(cairo, text, &extents);
			cairo_font_extents(cairo, &fe);
			x = (buffer_width / 2) -
				(extents.width / 2 + extents.x_bearing);
			y = (buffer_diameter / 2) +
				(fe.height / 2 - fe.descent);

			cairo_move_to(cairo, x, y);
			cairo_show_text(cairo, text);
			cairo_close_path(cairo);
			cairo_new_sub_path(cairo);
		}

		// Typing indicator: Highlight random part on keypress
		if (state->input_state == INPUT_STATE_LETTER ||
				state->input_state == INPUT_STATE_BACKSPACE) {
			float highlight_start = state->highlight_start * (M_PI / 1024.0);
			cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2,
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
			float inner_radius = buffer_diameter / 2.0 - arc_thickness * 1.5;
			float outer_radius = buffer_diameter / 2.0 - arc_thickness / 2.0;

			cairo_set_line_width(cairo, 2.0 * surface->scale);
			cairo_set_source_u32(cairo, state->args.colors.separator);
			cairo_move_to(cairo,
				buffer_width / 2.0 + cos(highlight_start) * inner_radius,
				buffer_diameter / 2.0 + sin(highlight_start) * inner_radius
			);
			cairo_line_to(cairo,
				buffer_width / 2.0 + cos(highlight_start) * outer_radius,
				buffer_diameter / 2.0 + sin(highlight_start) * outer_radius
			);
			cairo_stroke(cairo);

			cairo_move_to(cairo,
				buffer_width / 2.0 + cos(highlight_start + TYPE_INDICATOR_RANGE) * inner_radius,
				buffer_diameter / 2.0 + sin(highlight_start + TYPE_INDICATOR_RANGE) * inner_radius
			);
			cairo_line_to(cairo,
				buffer_width / 2.0 + cos(highlight_start + TYPE_INDICATOR_RANGE) * outer_radius,
				buffer_diameter / 2.0 + sin(highlight_start + TYPE_INDICATOR_RANGE) * outer_radius
			);
			cairo_stroke(cairo);
		}

		// Draw inner + outer border of the circle
		set_color_for_state(cairo, state, &state->args.colors.line);
		cairo_set_line_width(cairo, 2.0 * surface->scale);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2,
				arc_radius - arc_thickness / 2, 0, 2 * M_PI);
		cairo_stroke(cairo);
		cairo_arc(cairo, buffer_width / 2, buffer_diameter / 2,
				arc_radius + arc_thickness / 2, 0, 2 * M_PI);
		cairo_stroke(cairo);

		// display layout text separately
		if (layout_text) {
			cairo_text_extents_t extents;
			cairo_font_extents_t fe;
			float x, y;
			float box_padding = 4.0 * surface->scale;
			cairo_text_extents(cairo, layout_text, &extents);
			cairo_font_extents(cairo, &fe);
			// upper left coordinates for box
			x = (buffer_width / 2) - (extents.width / 2) - box_padding;
			y = buffer_diameter;

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

	// Draw slot machine emojis with animation
	if (state->has_emojis) {
		// Configure font for emojis - try common emoji fonts
		cairo_font_options_t *fo = cairo_font_options_create();
		cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
		cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_SUBPIXEL);
		cairo_font_options_set_subpixel_order(fo, to_cairo_subpixel_order(surface->subpixel));
		cairo_set_font_options(cairo, fo);
		cairo_font_options_destroy(fo);

		// Try multiple emoji fonts until one works
		const char *emoji_fonts[] = {
			"Noto Color Emoji",
			"Apple Color Emoji",
			"Segoe UI Emoji",
			"Symbola",
			"emoji",
			NULL
		};

		for (int i = 0; emoji_fonts[i] != NULL; i++) {
			cairo_select_font_face(cairo, emoji_fonts[i],
				CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
			break; // Use first font
		}

		// Make emojis bigger - 3x the normal text size
		uint32_t emoji_size = state->args.font_size > 0 ? state->args.font_size * 3 : (uint32_t)arc_radius;
		cairo_set_font_size(cairo, emoji_size);

		cairo_font_extents_t fe;
		cairo_font_extents(cairo, &fe);

		// Calculate target y position for animation
		// Use screen height, not buffer_height (which changes during particle animation)
		float target_y;
		if (draw_indicator) {
			target_y = buffer_diameter + fe.height;
		} else {
			// Use actual screen height for consistent positioning
			// float screen_center_y = (surface->height * surface->scale) / 2;
			// target_y = screen_center_y + (fe.height / 2 - fe.descent);
			target_y = (buffer_height / 2) + (fe.height / 2 - fe.descent);
		}

		// Update animation
		update_emoji_animation(state, target_y, buffer_height);

		float emoji_spacing = emoji_size * 1.5;
		float start_x = (buffer_width / 2) - emoji_spacing;

		// Draw old emojis falling off screen (if any)
		if (state->has_old_emojis) {
			for (int i = 0; i < 3; i++) {
				cairo_text_extents_t extents;
				cairo_text_extents(cairo, state->old_slot_emojis[i], &extents);

				float x = start_x + (i * emoji_spacing) - (extents.width / 2 + extents.x_bearing);
				float y = state->old_emoji_y_positions[i];

				// Only draw if still visible
				if (y < buffer_height + 100) {
					cairo_move_to(cairo, x, y);
					set_color_for_state(cairo, state, &state->args.colors.text);
					cairo_show_text(cairo, state->old_slot_emojis[i]);
				}
			}
		}

		// Draw new emojis at their animated positions
		for (int i = 0; i < 3; i++) {
			cairo_text_extents_t extents;
			cairo_text_extents(cairo, state->slot_emojis[i], &extents);

			float x = start_x + (i * emoji_spacing) - (extents.width / 2 + extents.x_bearing);
			float y = state->emoji_y_positions[i];

			cairo_move_to(cairo, x, y);
			set_color_for_state(cairo, state, &state->args.colors.text);
			cairo_show_text(cairo, state->slot_emojis[i]);
		}

		cairo_close_path(cairo);
		cairo_new_sub_path(cairo);
	}

	if (emoji_display) {
		free(emoji_display);
	}

	// Render particles if active
	if (particles_active) {
		// Configure font for particles (smaller = faster)
		cairo_select_font_face(cairo, "Noto Color Emoji",
			CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cairo, 30);

		// Fixed offset for emoji centering (avoids expensive text_extents per particle)
		const float emoji_offset = 15.0;

		// Render all active particles
		for (int i = 0; i < MAX_PARTICLES; i++) {
			if (state->celebration_particles.particles[i].active) {
				struct emoji_particle *p = &state->celebration_particles.particles[i];

				cairo_save(cairo);
				cairo_translate(cairo, p->x, p->y);
				cairo_rotate(cairo, p->rotation);

				// Use fixed offset instead of cairo_text_extents (major speedup)
				cairo_move_to(cairo, -emoji_offset, emoji_offset);

				cairo_set_source_u32(cairo, state->args.colors.text.input);
				cairo_show_text(cairo, p->emoji);

				cairo_restore(cairo);
			}
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
