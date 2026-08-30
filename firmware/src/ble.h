#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
// Stops/resumes advertising — used to honor the Settings screen's transport
// preference (USB-only turns the radio off; this also silences the HID
// keyboard shortcuts, since they ride the same BLE connection). Existing
// connections aren't force-dropped, only new ones are prevented; disabling
// just stops advertising rather than tearing down NimBLE entirely, so
// re-enabling is instant. Safe to call redundantly (no-ops if already in
// the requested state).
void ble_set_enabled(bool enabled);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
// Session-awareness feed (issue #135) — mirrors ble_has_data/ble_get_data for
// the SS characteristic. Present on every board; boards without
// BOARD_HAS_SESSION_VIEWS simply never drain it.
bool ble_has_session_data(void);
const char* ble_get_session_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

void ble_set_battery_level(int pct);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
