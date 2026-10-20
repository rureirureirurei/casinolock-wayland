#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "loop.h"
#include "slots.h"
#include "swaylock.h"

#define SLOT_TICK_MS 16
#define SLOT_SPEED 0.08 // window heights per tick
#define SLOT_STAGGER 0.25
#define SLOT_SPIN_TICKS 6 // re-roll interval while verifying

static const char *symbols[] = {
	"\xF0\x9F\x8D\x92", // cherry
	"\xE2\xAD\x90", // star
	"\xF0\x9F\x92\x8E", // diamond
	"\xF0\x9F\x8D\x8C", // banana
};
static const char *cross = "\xE2\x9D\x8C";

static void slots_tick(void *data);

static void schedule_tick(struct swaylock_state *state) {
	if (!state->slots.timer) {
		state->slots.timer = loop_add_timer(
			state->eventloop, SLOT_TICK_MS, slots_tick, state);
	}
}

static void drop(struct swaylock_state *state, const char *symbol) {
	struct slot_machine *m = &state->slots;
	if (m->visible) {
		memcpy(m->old_reels, m->reels, sizeof(m->reels));
		m->old_visible = true;
	}
	for (int i = 0; i < 3; i++) {
		m->reels[i].symbol = symbol ? symbol :
			symbols[rand() % (sizeof(symbols) / sizeof(*symbols))];
		m->reels[i].offset = -1 - i * SLOT_STAGGER;
	}
	m->visible = true;
	schedule_tick(state);
}

static bool step(struct slot_machine *m) {
	bool moving = false;
	for (int i = 0; i < 3; i++) {
		if (m->reels[i].offset < 0) {
			m->reels[i].offset = fmin(m->reels[i].offset + SLOT_SPEED, 0);
			moving = true;
		}
	}
	if (m->old_visible) {
		m->old_visible = false;
		for (int i = 0; i < 3; i++) {
			m->old_reels[i].offset += SLOT_SPEED;
			if (m->old_reels[i].offset < 1) {
				m->old_visible = true;
			}
		}
	}
	return moving || m->old_visible;
}

static void slots_tick(void *data) {
	struct swaylock_state *state = data;
	struct slot_machine *m = &state->slots;
	m->timer = NULL;

	bool spinning = state->auth_state == AUTH_STATE_VALIDATING;
	if (spinning && ++m->ticks % SLOT_SPIN_TICKS == 0) {
		drop(state, NULL);
	}
	if (step(m) || spinning) {
		schedule_tick(state);
	}
	damage_state(state);
}

void slots_spin(struct swaylock_state *state) {
	drop(state, NULL);
}

void slots_lose(struct swaylock_state *state) {
	drop(state, cross);
}

void slots_clear(struct swaylock_state *state) {
	struct slot_machine *m = &state->slots;
	m->visible = m->old_visible = false;
	if (m->timer) {
		loop_remove_timer(state->eventloop, m->timer);
		m->timer = NULL;
	}
}
