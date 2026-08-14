#include "board.h"

// No I2C devices on this board (touch is SPI, no PMU/IMU/IO expander) —
// nothing to bring up before display/touch HAL init.
extern "C" void board_init(void) {}
