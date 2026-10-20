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
#define SLOT_EFFECT_TICKS 30

const char slot_seven[] = "7";
static const char *cross = "❌";

static const struct slot_theme themes[] = {
	{"classic", "🍒 ⭐ 💎 🍌", slot_seven, "⭐ 💎 🌟", EFFECT_BURST, {1.0, 0.8, 0.2}},
	{"fruit", "🍒 🍋 🍊 🍉 🍇", "🍒", "🍒 🍋 🍊 🍉 🍇", EFFECT_BURST, {1.0, 0.4, 0.2}},
	{"cards", "🃏 ♠ ♥ ♦ ♣", "🃏", "♠ ♥ ♦ ♣", EFFECT_RAIN, {0.9, 0.1, 0.2}},
	{"casino", "🎰 🎲 🃏 💰 🔔", "💰", "💰 💵 🪙", EFFECT_RAIN, {0.3, 0.9, 0.3}},
	{"space", "🚀 🪐 🌙 ☄ 👽", "🚀", "🚀 ⭐ ✨ 🌟", EFFECT_LAUNCH, {0.4, 0.6, 1.0}},
	{"food", "🍕 🍔 🌮 🍩 🍣", "🍕", "🍕 🍔 🌮 🍩 🍣", EFFECT_BURST, {1.0, 0.6, 0.2}},
	{"zoo", "🐸 🐙 🦊 🐼 🦄", "🦄", "🦄 🌈 💖 ✨", EFFECT_FLOAT, {1.0, 0.5, 0.9}},
};
#define THEME_COUNT (sizeof(themes) / sizeof(*themes))

static struct slot_theme custom_theme;

bool slots_theme_exists(const char *name) {
	if (strcmp(name, "random") == 0) {
		return true;
	}
	for (size_t i = 0; i < THEME_COUNT; i++) {
		if (strcmp(themes[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

static const struct slot_theme *find_theme(const char *name) {
	for (size_t i = 0; name && i < THEME_COUNT; i++) {
		if (strcmp(themes[i].name, name) == 0) {
			return &themes[i];
		}
	}
	return &themes[rand() % THEME_COUNT];
}

static const char **split(const char *list, size_t *count) {
	char *copy = strdup(list);
	const char **out = copy ? calloc(strlen(copy) / 2 + 1, sizeof(*out)) : NULL;
	*count = 0;
	if (!out) {
		free(copy);
		return NULL;
	}
	char *save = NULL;
	for (char *tok = strtok_r(copy, " ", &save); tok;
			tok = strtok_r(NULL, " ", &save)) {
		out[(*count)++] = strcmp(tok, slot_seven) == 0 ? slot_seven : tok;
	}
	return out;
}

bool slots_init(struct swaylock_state *state, const char *theme,
		const char *symbols) {
	struct slot_machine *m = &state->slots;
	if (symbols) {
		custom_theme = themes[0];
		custom_theme.name = "custom";
		custom_theme.symbols = symbols;
		m->theme = &custom_theme;
	} else {
		m->theme = find_theme(theme);
	}
	m->symbols = split(m->theme->symbols, &m->symbol_count);
	m->particle_symbols = split(m->theme->particles, &m->particle_symbol_count);
	return m->symbol_count > 0 && m->particle_symbol_count > 0;
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

static double frand(double lo, double hi) {
	return lo + (hi - lo) * rand() / (double)RAND_MAX;
}

static void spawn(struct slot_machine *m, double x, double y,
		double vx, double vy) {
	if (m->particle_count == SLOT_MAX_PARTICLES) {
		return;
	}
	m->particles[m->particle_count++] = (struct slot_particle){
		.symbol = m->particle_symbols[rand() % m->particle_symbol_count],
		.x = x, .y = y, .vx = vx, .vy = vy,
		.spin = frand(-0.15, 0.15),
	};
}

static void effect_spawn(struct slot_machine *m) {
	double bx = fmax(m->bound_x, 5), by = fmax(m->bound_y, 5);
	switch (m->theme->effect) {
	case EFFECT_BURST:
		if (m->effect_ticks % 2 == 0) {
			for (int i = 0; i < 3; i++) {
				spawn(m, (i - 1) * 1.4, 0, frand(-0.3, 0.3), frand(-0.6, -0.3));
			}
		}
		break;
	case EFFECT_RAIN:
		spawn(m, frand(-bx, bx), -by - 1, frand(-0.03, 0.03), frand(0.25, 0.4));
		break;
	case EFFECT_LAUNCH:
		spawn(m, frand(-bx, bx), by + 1, frand(-0.03, 0.03), frand(-0.15, -0.05));
		break;
	case EFFECT_FLOAT:
		spawn(m, frand(-bx, bx), by + 1, 0, frand(-0.35, -0.25));
		break;
	}
}

static void effect_step(struct slot_machine *m) {
	if (m->effect_ticks > 0) {
		effect_spawn(m);
		m->effect_ticks--;
	}
	double bx = fmax(m->bound_x, 5) + 2, by = fmax(m->bound_y, 5) + 2;
	int n = 0;
	for (int i = 0; i < m->particle_count; i++) {
		struct slot_particle p = m->particles[i];
		switch (m->theme->effect) {
		case EFFECT_BURST:
			p.vy += 0.03;
			break;
		case EFFECT_RAIN:
			p.vy += 0.004;
			break;
		case EFFECT_LAUNCH:
			p.vy -= 0.012;
			p.spin = 0;
			break;
		case EFFECT_FLOAT:
			p.vx = sin(p.age * 0.15) * 0.08;
			p.spin = 0;
			break;
		}
		p.x += p.vx;
		p.y += p.vy;
		p.rotation += p.spin;
		p.age++;
		if (p.age < SLOT_PARTICLE_LIFE && fabs(p.x) < bx && fabs(p.y) < by) {
			m->particles[n++] = p;
		}
	}
	m->particle_count = n;
}

bool slots_effect_active(struct slot_machine *m) {
	return m->effect_ticks > 0 || m->particle_count > 0;
}

static void celebrate(struct slot_machine *m) {
	const char *s = m->reels[0].symbol;
	if (s != cross && s == m->reels[1].symbol && s == m->reels[2].symbol) {
		m->glow = SLOT_GLOW_TICKS;
		m->shake = s == slot_seven || m->won;
		if (m->won) {
			m->effect_ticks = SLOT_EFFECT_TICKS;
		}
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
	effect_step(m);
	if (moving || m->glow > 0 || m->spinning || slots_effect_active(m)) {
		schedule_tick(state);
	}
	damage_state(state);
	if (m->done && m->won && m->settled && !slots_effect_active(m)) {
		void (*done)(void *data) = m->done;
		m->done = NULL;
		done(state);
	}
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

void slots_win(struct swaylock_state *state, void (*done)(void *data)) {
	state->slots.won = true;
	state->slots.done = done;
	stop(state, state->slots.theme->win);
}

void slots_lose(struct swaylock_state *state) {
	stop(state, cross);
}

void slots_clear(struct swaylock_state *state) {
	struct slot_machine *m = &state->slots;
	m->visible = m->spinning = false;
	m->stop_symbol = NULL;
	m->glow = 0;
	m->effect_ticks = m->particle_count = 0;
	if (m->timer) {
		loop_remove_timer(state->eventloop, m->timer);
		m->timer = NULL;
	}
}
