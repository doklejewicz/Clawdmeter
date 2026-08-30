#include "settings.h"
#include <Preferences.h>

static uint8_t fps           = 30;
static uint8_t clock_format  = 24;
static uint8_t transport     = TRANSPORT_AUTO;

void settings_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint8_t v;
    v = prefs.getUChar("fps", 0xFF);
    if (v >= SETTINGS_FPS_MIN && v <= SETTINGS_FPS_MAX) fps = v;
    v = prefs.getUChar("clockfmt", 0xFF);
    if (v == 12 || v == 24) clock_format = v;
    v = prefs.getUChar("transport", 0xFF);
    if (v < TRANSPORT_COUNT) transport = v;
    prefs.end();
}

uint8_t settings_get_fps(void) { return fps; }

void settings_set_fps(uint8_t new_fps) {
    if (new_fps < SETTINGS_FPS_MIN) new_fps = SETTINGS_FPS_MIN;
    if (new_fps > SETTINGS_FPS_MAX) new_fps = SETTINGS_FPS_MAX;
    fps = new_fps;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("fps", fps);
    prefs.end();
}

uint8_t settings_get_clock_format(void) { return clock_format; }

void settings_set_clock_format(uint8_t fmt) {
    clock_format = (fmt == 12) ? 12 : 24;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("clockfmt", clock_format);
    prefs.end();
}

transport_pref_t settings_get_transport(void) { return (transport_pref_t)transport; }

void settings_set_transport(transport_pref_t t) {
    transport = t;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("transport", transport);
    prefs.end();
}

const char* settings_transport_name(transport_pref_t t) {
    switch (t) {
        case TRANSPORT_USB: return "USB";
        case TRANSPORT_BLE: return "BLE";
        default:            return "Auto";
    }
}
