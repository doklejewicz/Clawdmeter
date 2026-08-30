#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

// XPT2046 resistive touch, on its own SPI bus (VSPI) separate from the
// display's HSPI bus — same wiring and auto-calibration scheme as the
// hardware-verified ClawdMeter.ino / LVGL_Arduino.ino build in the
// ESP32-Cheap-Yellow-Display repo. `touchscreen.setRotation(3)` is the
// verified value for this exact wiring; it's an XPT2046_Touchscreen library
// rotation index, unrelated to the display's Arduino_GFX LCD_ROTATION.
//
// The raw ADC bounds below aren't a factory calibration — they self-widen on
// out-of-range reads, same as the verified build, so accuracy improves after
// the first few taps near the panel edges.
//
// touch_hal_read() maps the ADC range to the INVERTED display range (see the
// map() calls below) — confirmed empirically (a 4-corner tap test) that this
// board's touch axes land exactly 180° opposite the display's without it:
// every physical corner tap was reported as its diagonal opposite. Harmless
// for "tap anywhere" gestures (the whole panel was one big hit target,
// which is presumably why this was never caught before), but broke any
// feature needing a specific screen region (e.g. the Settings-screen tap
// zone) until this was found and fixed.

static SPIClass touch_spi(VSPI);
static XPT2046_Touchscreen touchscreen(TP_CS, TP_IRQ);

static uint16_t cal_min_x = 200, cal_max_x = 3700;
static uint16_t cal_min_y = 240, cal_max_y = 3800;

static uint16_t touch_x = 0;
static uint16_t touch_y = 0;
static bool     touch_pressed = false;

void touch_hal_init(void) {
    touch_spi.begin(TP_CLK, TP_MISO, TP_MOSI, TP_CS);
    touchscreen.begin(touch_spi);
    touchscreen.setRotation(3);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();
        if (p.x < cal_min_x) cal_min_x = p.x;
        if (p.x > cal_max_x) cal_max_x = p.x;
        if (p.y < cal_min_y) cal_min_y = p.y;
        if (p.y > cal_max_y) cal_max_y = p.y;
        touch_x = map(p.x, cal_min_x, cal_max_x, LCD_WIDTH, 1);
        touch_y = map(p.y, cal_min_y, cal_max_y, LCD_HEIGHT, 1);
        touch_pressed = true;
    } else {
        touch_pressed = false;
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
