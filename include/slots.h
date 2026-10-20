#ifndef _SWAYLOCK_SLOTS_H
#define _SWAYLOCK_SLOTS_H
#include <stdbool.h>

struct swaylock_state;

struct slot_reel {
	const char *symbol;
	double offset; // -1 above window, 0 centered, 1 below window
};

struct slot_machine {
	struct slot_reel reels[3];
	struct slot_reel old_reels[3]; // previous roll, falling out
	bool visible, old_visible;
	int ticks;
	struct loop_timer *timer;
};

void slots_spin(struct swaylock_state *state);
void slots_lose(struct swaylock_state *state);
void slots_clear(struct swaylock_state *state);

#endif
