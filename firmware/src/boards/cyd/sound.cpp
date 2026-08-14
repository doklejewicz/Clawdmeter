#include "../../hal/sound_hal.h"

// No populated speaker on this board (there's an unpopulated I2S-amp header
// per PINS.md, but wiring it up is a separate follow-up) — no-op, same
// pattern as boards/waveshare_amoled_18_c6/sound.cpp. main.cpp calls these
// symbols unconditionally, so they must exist even with BOARD_HAS_SOUND=0.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
