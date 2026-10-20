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

struct slot_machine {
	const char **symbols;
	size_t symbol_count;
	struct slot_reel reels[3];
	bool visible, spinning, settled;
	const char *stop_symbol;
	int ticks, stop_tick;
	int glow; // remaining celebration ticks
	bool jackpot; // celebration is 777
	struct loop_timer *timer;
};

#define SLOT_GLOW_TICKS 45
#define SLOT_UNLOCK_DELAY_MS 1500

const char *slots_theme(const char *name);
bool slots_init(struct swaylock_state *state, const char *symbols);
void slots_roll(struct swaylock_state *state);
void slots_spin(struct swaylock_state *state);
void slots_win(struct swaylock_state *state);
void slots_lose(struct swaylock_state *state);
void slots_clear(struct swaylock_state *state);

#endif
