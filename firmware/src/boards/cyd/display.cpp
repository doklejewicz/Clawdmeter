#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// ILI9341 over plain 4-wire SPI. Arduino_ILI9341 always drives MADCTL with
// the BGR bit set (matches this panel's wiring — same as the hardware-
// verified TFT_eSPI build, which needed no RGB-order override for this
// variant). ips=true — hardware-verified: without it the panel renders
// fully bit-inverted (black background shows white, and hues flip to their
// complement — e.g. orange renders as blue).
//
// Constructor width/height are the *native* (rotation-0/2) panel dimensions;
// Arduino_GFX swaps them internally for odd rotation indices, so passing
// 240x320 here with LCD_ROTATION=1 yields the 320x240 landscape reported by
// board_caps().

static Arduino_DataBus* bus = nullptr;
static Arduino_ILI9341* gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI,
                               GFX_NOT_DEFINED /* MISO unused for write-only flush */);
    gfx = new Arduino_ILI9341(bus, LCD_RESET, LCD_ROTATION, true /* ips */,
                              240 /* native width */, 320 /* native height */);
}

void display_hal_begin(void) {
    gfx->begin(55000000);   // matches the verified TFT_eSPI SPI_FREQUENCY
    gfx->fillScreen(0x0000);
    ledcAttach(LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 200);
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (gfx) gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
}

void display_hal_tick(void) {
    // No rotation cycle on this board.
}

// Plain SPI ILI9341 has no flush-region alignment requirement.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}
