#ifndef _SWAYLOCK_SLOTS_H
#define _SWAYLOCK_SLOTS_H
#include <stdbool.h>
#include <stddef.h>

struct swaylock_state;

extern const char slot_seven[]; // drawn as a classic red 7, not an emoji

enum slot_reel_mode {
	REEL_REST,
	REEL_MOVE,
	REEL_SETTLE,
};

struct slot_reel {
	const char *symbol, *prev; // prev falls out below
	double offset, prev_offset; // -1 above window, 0 centered, 1 below
	double speed, velocity;
	const char *stop; // symbol to land on when spinning ends
	bool landing;
	enum slot_reel_mode mode;
};

enum slot_effect {
	EFFECT_BURST, // particles explode out of the reels
	EFFECT_RAIN, // particles fall from above
	EFFECT_LAUNCH, // particles accelerate upwards
	EFFECT_FLOAT, // particles drift up, wobbling
};

struct slot_theme {
	const char *name;
	const char *symbols; // space-separated
	const char *win; // symbol the reels land on when unlocking
	const char *particles; // space-separated
	enum slot_effect effect;
	double glow[3];
};

// Positions in symbol sizes relative to the middle reel, y down
struct slot_particle {
	const char *symbol;
	double x, y, vx, vy, rotation, spin;
	int age;
};

#define SLOT_MAX_PARTICLES 32

struct slot_machine {
	const struct slot_theme *theme;
	const char **symbols, **particle_symbols;
	size_t symbol_count, particle_symbol_count;
	struct slot_reel reels[3];
	bool visible, spinning, settled, won;
	const char *stop_symbol;
	int ticks, stop_tick;
	int glow; // remaining celebration ticks
	bool shake;
	int effect_ticks; // remaining particle spawn ticks
	struct slot_particle particles[SLOT_MAX_PARTICLES];
	int particle_count;
	struct loop_timer *timer;
};

#define SLOT_GLOW_TICKS 45
#define SLOT_UNLOCK_DELAY_MS 2000
#define SLOT_EFFECT_MARGIN 2.5 // extra room around the reels, in symbol sizes

bool slots_theme_exists(const char *name);
bool slots_init(struct swaylock_state *state, const char *theme,
	const char *symbols);
bool slots_effect_active(struct slot_machine *m);
void slots_roll(struct swaylock_state *state);
void slots_spin(struct swaylock_state *state);
void slots_win(struct swaylock_state *state);
void slots_lose(struct swaylock_state *state);
void slots_clear(struct swaylock_state *state);

#endif
