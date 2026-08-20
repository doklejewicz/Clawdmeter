#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// No PMU, no battery, no dedicated PWR button on this board — BOOT (GPIO0)
// is the only button and it's owned by input_hal, not power_hal.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }
bool power_hal_pwr_pressed(void) { return false; }
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void) { return false; }
