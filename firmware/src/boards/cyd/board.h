#pragma once

// "Cheap Yellow Display" (ESP32-2432S028) — classic dual-core ESP32, no
// PSRAM, 2.8" ILI9341 SPI TFT + XPT2046 resistive touch on its own SPI bus.
// Not to be confused with the similarly-named ESP32-2432S024C (2.4",
// capacitive I2C touch, ST7789) added in a separate PR — different board.
//
// Pin map taken from the hardware-verified TFT_eSPI/XPT2046_Touchscreen
// build in the ESP32-Cheap-Yellow-Display repo's own examples (LVGL_Arduino,
// ClawdMeter), which this port replaces with Arduino_GFX to fit this
// project's HAL contract.

#define BOARD_NAME           "Cheap Yellow Display"

// ---- Display geometry ----
// Panel is native 240x320 portrait; landscape is reached via the driver's
// rotation index below (Arduino_GFX MADCTL convention: odd = landscape).
// NOT verified against physical hardware yet — if the image comes up
// sideways/mirrored on first flash, try LCD_ROTATION 3 instead of 1 (see
// docs/porting/adding-a-board.md's bring-up pitfalls).
#define LCD_WIDTH            320
#define LCD_HEIGHT           240
#define LCD_ROTATION         1

// ---- SPI display pins (ILI9341, 4-wire SPI, HSPI) ----
#define LCD_CS               15
#define LCD_SCLK             14
#define LCD_MOSI             13
#define LCD_MISO             12
#define LCD_DC               2
#define LCD_RESET            -1    // not wired
#define LCD_BL                21   // backlight, LEDC PWM

// ---- Touch (XPT2046, resistive, own SPI bus — VSPI, non-default pins) ----
#define TP_CLK                25
#define TP_MOSI                32
#define TP_MISO                39
#define TP_CS                  33
#define TP_IRQ                 36

// ---- Buttons ----
#define BTN_BACK_GPIO         0     // BOOT — primary, Space (PTT)

// ---- Capability flags ----
// No secondary button, no IMU-driven rotation, no battery/PMU, no IO
// expander, no populated speaker (the board exposes an unpopulated I2S-amp
// header — see PINS.md — out of scope for this port).
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0
