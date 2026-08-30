#pragma once
#include <stdint.h>

// On-device settings, persisted to NVS (see settings.cpp) — reachable from
// the Settings screen (tap the corner mascot on the usage screen). All
// values are cached in RAM after settings_init() and only touch NVS on a
// set() call, same convention as brightness.cpp.

enum transport_pref_t : uint8_t {
    TRANSPORT_AUTO = 0,  // today's only real behavior: daemon auto-prioritizes
                         // USB serial over BLE. USB/BLE below are stored and
                         // shown, but not yet wired to the daemon — the device
                         // has no channel to tell the host its preference yet.
    TRANSPORT_USB  = 1,
    TRANSPORT_BLE  = 2,
    TRANSPORT_COUNT,
};

void settings_init(void);

uint8_t settings_get_fps(void);
void    settings_set_fps(uint8_t fps);   // clamped to [FPS_MIN, FPS_MAX]
#define SETTINGS_FPS_MIN 5
#define SETTINGS_FPS_MAX 60
#define SETTINGS_FPS_STEP 5

// 12 or 24. Fully device-owned — the daemon's "tf" JSON field is no longer
// consulted (the device already gets the raw epoch; formatting it is a
// display preference, not something the host needs to decide).
uint8_t settings_get_clock_format(void);
void    settings_set_clock_format(uint8_t fmt);

transport_pref_t settings_get_transport(void);
void    settings_set_transport(transport_pref_t t);
const char* settings_transport_name(transport_pref_t t);
