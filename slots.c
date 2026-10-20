#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "loop.h"
#include "slots.h"
#include "swaylock.h"

#define SLOT_TICK_MS 16
#define SLOT_DROP_SPEED 0.12 // window heights per tick
#define SLOT_SPIN_SPEED 0.25
#define SLOT_STAGGER 0.25 // drop offset between reels
#define SLOT_MIN_SPIN 12 // ticks before the first reel may stop
#define SLOT_STOP_STAGGER 7 // ticks between reel stops
#define SLOT_SEVEN_ODDS 16 // 1 in N per reel
#define SLOT_SPRING 0.3
#define SLOT_DAMPING 0.35

const char slot_seven[] = "7";
static const char *cross = "❌";

static const struct {
	const char *name, *symbols;
} themes[] = {
	{"classic", "🍒 ⭐ 💎 🍌"},
	{"fruit", "🍒 🍋 🍊 🍉 🍇"},
	{"cards", "🃏 ♠ ♥ ♦ ♣"},
	{"casino", "🎰 🎲 🃏 💰 🔔"},
	{"space", "🚀 🪐 🌙 ☄ 👽"},
	{"food", "🍕 🍔 🌮 🍩 🍣"},
	{"zoo", "🐸 🐙 🦊 🐼 🦄"},
};

const char *slots_theme(const char *name) {
	for (size_t i = 0; i < sizeof(themes) / sizeof(*themes); i++) {
		if (strcmp(themes[i].name, name) == 0) {
			return themes[i].symbols;
		}
	}
	return NULL;
}

bool slots_init(struct swaylock_state *state, const char *symbols) {
	struct slot_machine *m = &state->slots;
	char *copy = strdup(symbols ? symbols : themes[0].symbols);
	if (!copy) {
		return false;
	}
	size_t cap = strlen(copy) / 2 + 1;
	m->symbols = calloc(cap, sizeof(*m->symbols));
	if (!m->symbols) {
		free(copy);
		return false;
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, " ", &save); tok;
			tok = strtok_r(NULL, " ", &save)) {
		m->symbols[m->symbol_count++] =
			strcmp(tok, slot_seven) == 0 ? slot_seven : tok;
	}
	return m->symbol_count > 0;
}

static const char *random_symbol(struct slot_machine *m) {
	if (rand() % SLOT_SEVEN_ODDS == 0) {
		return slot_seven;
	}
	return m->symbols[rand() % m->symbol_count];
}

static void slots_tick(void *data);

static void schedule_tick(struct swaylock_state *state) {
	if (!state->slots.timer) {
		state->slots.timer = loop_add_timer(
			state->eventloop, SLOT_TICK_MS, slots_tick, state);
	}
}

static void reel_push(struct slot_reel *r, const char *symbol, double offset) {
	r->prev = r->symbol;
	r->prev_offset = r->offset;
	r->symbol = symbol;
	r->offset = offset;
}

static void drop(struct swaylock_state *state, const char *symbol) {
	struct slot_machine *m = &state->slots;
	for (int i = 0; i < 3; i++) {
		struct slot_reel *r = &m->reels[i];
		reel_push(r, symbol ? symbol : random_symbol(m), -1 - i * SLOT_STAGGER);
		if (!m->visible) {
			r->prev = NULL;
		}
		r->mode = REEL_MOVE;
		r->speed = SLOT_DROP_SPEED;
	}
	m->visible = true;
	m->glow = 0;
	schedule_tick(state);
}

static bool reel_step(struct slot_machine *m, struct slot_reel *r) {
	if (r->prev) {
		r->prev_offset += r->speed;
		if (r->prev_offset >= 1) {
			r->prev = NULL;
		}
	}
	switch (r->mode) {
	case REEL_MOVE:
		r->offset += r->speed;
		if (r->offset < 0) {
			break;
		}
		if (m->spinning && !r->landing) {
			reel_push(r, r->stop ? r->stop : random_symbol(m), r->offset - 1);
			r->landing = r->stop != NULL;
		} else {
			r->mode = REEL_SETTLE;
			r->velocity = r->speed;
		}
		break;
	case REEL_SETTLE:
		r->velocity += -SLOT_SPRING * r->offset - SLOT_DAMPING * r->velocity;
		r->offset += r->velocity;
		if (fabs(r->offset) < 0.01 && fabs(r->velocity) < 0.01) {
			r->offset = 0;
			r->mode = REEL_REST;
		}
		break;
	case REEL_REST:
		break;
	}
	return r->mode != REEL_REST || r->prev;
}

static void celebrate(struct slot_machine *m) {
	const char *s = m->reels[0].symbol;
	if (s != cross && s == m->reels[1].symbol && s == m->reels[2].symbol) {
		m->glow = SLOT_GLOW_TICKS;
		m->jackpot = s == slot_seven;
	}
}

static void slots_tick(void *data) {
	struct swaylock_state *state = data;
	struct slot_machine *m = &state->slots;
	m->timer = NULL;
	m->ticks++;

	bool moving = false;
	for (int i = 0; i < 3; i++) {
		struct slot_reel *r = &m->reels[i];
		if (m->stop_symbol && !r->stop &&
				m->ticks >= m->stop_tick + i * SLOT_STOP_STAGGER) {
			r->stop = m->stop_symbol;
		}
		moving |= reel_step(m, r);
	}

	if (moving) {
		m->settled = false;
	} else if (!m->settled) {
		m->settled = true;
		m->spinning = false;
		m->stop_symbol = NULL;
		celebrate(m);
	} else if (m->glow > 0) {
		m->glow--;
	}
	if (moving || m->glow > 0 || m->spinning) {
		schedule_tick(state);
	}
	damage_state(state);
}

void slots_roll(struct swaylock_state *state) {
	if (!state->slots.spinning) {
		drop(state, NULL);
	}
}

void slots_spin(struct swaylock_state *state) {
	struct slot_machine *m = &state->slots;
	if (!m->visible) {
		drop(state, NULL);
	}
	m->spinning = true;
	m->stop_symbol = NULL;
	m->ticks = 0;
	m->glow = 0;
	for (int i = 0; i < 3; i++) {
		struct slot_reel *r = &m->reels[i];
		r->mode = REEL_MOVE;
		r->speed = SLOT_SPIN_SPEED;
		r->stop = NULL;
		r->landing = false;
	}
	schedule_tick(state);
}

static void stop(struct swaylock_state *state, const char *symbol) {
	struct slot_machine *m = &state->slots;
	if (!m->spinning) {
		drop(state, symbol);
		return;
	}
	m->stop_symbol = symbol;
	m->stop_tick = m->ticks > SLOT_MIN_SPIN ? m->ticks : SLOT_MIN_SPIN;
	schedule_tick(state);
}

void slots_win(struct swaylock_state *state) {
	stop(state, slot_seven);
}

void slots_lose(struct swaylock_state *state) {
	stop(state, cross);
}

void slots_clear(struct swaylock_state *state) {
	struct slot_machine *m = &state->slots;
	m->visible = m->spinning = false;
	m->stop_symbol = NULL;
	m->glow = 0;
	if (m->timer) {
		loop_remove_timer(state->eventloop, m->timer);
		m->timer = NULL;
	}
}
