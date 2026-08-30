#include "ui.h"
#include "splash.h"
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "clawd_still.h"
#include "clawd_settings.h"
#include "clawd_sessions.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "settings.h"
#include "ble.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock — same size as "Usage"
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math
    int16_t conn_icon_gap;           // spacing between the battery/BLE/serial icon row
    int16_t conn_row_margin;         // right-edge inset for that row — tighter than
                                      // L.margin so the cluster hugs the corner and
                                      // leaves the title/clock more room to center in

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.conn_icon_gap = 4;
    L.conn_row_margin = 8;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.conn_icon_gap = 3;
        L.conn_row_margin = 4;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_PURPLE    THEME_PURPLE
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24 — a device-owned display setting
                                   // (Settings screen); loaded in ui_init(),
                                   // NOT read from the daemon's "tf" field —
                                   // the device already has the raw epoch, so
                                   // formatting it is purely a local concern.
static long     clock_last_epoch = -1;   // last rendered second; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Transport status icons (shared, on top, left of the battery icon) ----
// Both transports are always live on the firmware side (see check_serial_cmd()
// and the ble_has_data() branch in main.cpp's loop()) — these two icons just
// report which one(s) a host is actually using right now. Bluetooth reflects
// the real GATT connection state; USB serial has no such notion (any line
// received is just processed), so its state is inferred from how recently a
// valid payload arrived, using the same DATA_FRESH_MS margin BLE payloads are
// judged "live" by.
static lv_obj_t* bt_icon_img;
static lv_obj_t* serial_icon_img;
static lv_image_dsc_t bt_icon_dsc;
static lv_image_dsc_t serial_icon_dsc;
static bool     s_ble_ever_connected = false;  // gray (never used) vs red (used, now down)
static bool     s_serial_ever_active = false;  // gray (never used) vs red (used, now stale)
static uint32_t s_last_serial_ms = 0;
static lv_color_t s_bt_icon_col;
static lv_color_t s_serial_icon_col;
static bool     s_icon_col_init = false;

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static lv_image_dsc_t settings_icon_dsc;
#if BOARD_HAS_SESSION_VIEWS
#ifndef BOARD_HAS_PSRAM
static lv_image_dsc_t sessions_icon_dsc;   // corner-mascot swap while a session view is active — see clawd_sessions.h
#endif
#endif
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// ---- Settings screen (tap the corner mascot from the usage screen) ----
static lv_obj_t* settings_container;
static lv_obj_t* settings_icon_img;   // corner icon, fixed position on `scr` — see init_settings_screen()
static lv_obj_t* settings_tap_zone;   // enlarged invisible tap target — see ui_init()
static lv_obj_t* lbl_set_email;
static lv_obj_t* lbl_set_org;
static lv_obj_t* lbl_set_account;
static lv_obj_t* lbl_set_device;   // set once at init — ble_get_device_name() doesn't change at runtime
static lv_obj_t* lbl_set_mac;      // set once at init — ble_get_mac_address() is factory-burned per-chip
static lv_obj_t* lbl_fps_value;
static lv_obj_t* lbl_transport_value;
static lv_obj_t* lbl_clockfmt_value;
// Always kept current regardless of BOARD_HAS_SESSION_VIEWS (unlike
// s_usage_cache below, which only tracks it on boards with session views) —
// the Settings screen's usage summary needs to work on every board.
static UsageData s_settings_usage = {};

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating", "Let me out", 
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming", "Make it stop", 
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing", "Please help me", 
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling", "Feeling pain", 
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling", "ඞ"
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void open_settings_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// Always the compact 24px size, even at the large breakpoint — these are
// secondary status glyphs, not primary content like the battery gauge, and
// staying small leaves more room for the title/clock text between them and
// the corner logo (see the title-box math next to lbl_title's creation).
static void init_conn_icons(void) {
    init_icon_dsc_rgb565a8(&bt_icon_dsc, ICON_BLUETOOTH_SMALL_W, ICON_BLUETOOTH_SMALL_H, icon_bluetooth_small_data);
    init_icon_dsc_rgb565a8(&serial_icon_dsc, ICON_SERIAL_SMALL_W, ICON_SERIAL_SMALL_H, icon_serial_small_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

// ======== Live session awareness (issue #135) ========
// Two additional usage-screen sub-views — ONE-CHAT (§1.3) and SEVERAL-CHATS
// (§1.4) — compiled only on boards whose panel can host them
// (BOARD_HAS_SESSION_VIEWS). No new screen_t: update_view_state() picks them
// exactly like it picks pairing / no-data / quota.

static void update_view_state(void);       // defined below ui_update
static void apply_anim_visibility(void);   // status-line rule (§2.3)
static void update_connection_icons(void); // defined below, near ui_update_ble_status
static void refresh_settings_usage_labels(void);  // defined below, near init_settings_screen

#if BOARD_HAS_SESSION_VIEWS

// How long the chat view is held after the last live chat disappears before
// returning to RESTING (§2.1). A waiting chat pins the view past this timer.
#ifndef CHAT_LINGER_MS
#define CHAT_LINGER_MS (10u * 60u * 1000u)
#endif

// Geometry — the capability gate originally limited these views to
// 480×480-class panels (the S3 2.16 and the sim); other geometries need a
// layout pass before their flag can flip, so these were tuned constants, not
// breakpoints. The CYD's 320x240 landscape panel is the first much-smaller
// screen to get one. ui.cpp never includes board.h (shared code can't see
// LCD_HEIGHT), so — same exception BOARD_HAS_SESSION_VIEWS itself already
// makes — SESSION_COMPACT must come in as its own global build flag
// (-DSESSION_COMPACT=1 in the board's platformio.ini env), not a header
// constant. Unverified without a screenshot-capable board; expect a
// flash-and-look iteration pass on real hardware.
#ifndef SESSION_COMPACT
#define SESSION_COMPACT 0
#endif

#if SESSION_COMPACT
// Quota strip + vertical rhythm ported from PR #129's combined 5h/7d row
// (the visual language issue #135 credits): a 30px band at content_y with
// dim styrene_20 tags, styrene_24 percentages in a fixed right-aligned
// column, 10px bars, and a 14px(+4) gap down to the first card. Scaled ~0.6x
// here to fit a 240px-tall screen instead of 480px.
#define CHAT_ROW_H        22    // strip band height; text/bar vcentered in it
#define CHAT_ROW_BAR_H    6
#define CHAT_ROW_LBL_W    24    // "5h"/"7d" column
#define CHAT_ROW_PCT_W    40    // percentage column (right-aligned, fixed)
#define CHAT_ROW_COL_GAP  6     // label|bar|pct column gap
#define CHAT_ROW_HALF_GAP 10    // between the 5h and 7d halves
#define CHAT_ROW_GAP      8     // strip band ↓ card list (plus 4, per #129)
#define CHAT_CARD_H       64
#define CHAT_CARD_GAP     6     // between cards (#129 ch_card_gap)
#define CHAT_CARD_PITCH   (CHAT_CARD_H + CHAT_CARD_GAP)
#define CHAT_CARD_PAD_Y   4
// Chat cards (and the ONE-CHAT quota box) bleed to the physical left/right
// edges — the card's own corner radius is the relief at the glass edge. The
// inner side padding keeps text at the same inset the screen margin
// provided, clear of the panel's rounded corners.
#define CHAT_CARD_PAD_X   10
#define CHAT_FADE_H       24    // bottom fade band: transparent → panel black
// ONE-CHAT: two boxes — the 5h quota panel (exact RESTING "Current" panel)
// on top, the chat card below it.
#define FOCUS_CARD_H      90
#define FOCUS_PANEL_GAP   6     // 5h panel ↔ chat card
#define SESSION_FONT_NAME_FOCUS font_styrene_20
#define SESSION_FONT_NAME_LIST  font_styrene_14
#define SESSION_FONT_LINE       font_styrene_12
#define SESSION_FONT_STRIP_TAG  font_styrene_14
#define SESSION_FONT_STRIP_PCT  font_styrene_16
#define SESSION_FONT_MODEL      font_styrene_14
#define SESSION_DOT_SZ          8
#define SESSION_BAR_H_FOCUS     6
#define SESSION_BAR_H_LIST      4
#define SESSION_BAR_OFFSET_FOCUS (-20)
#define SESSION_BAR_OFFSET_LIST  (-18)
#define SESSION_ICON_TODO(focus)    (&icon_todo_small_dsc)
#define SESSION_ICON_AGENTS(focus)  (&icon_agents_small_dsc)
#define SESSION_STATE_LABEL_W       120
#define SESSION_FOCUS_ROW2_Y        26
#else
#define CHAT_ROW_H        30    // strip band height; text/bar vcentered in it
#define CHAT_ROW_BAR_H    10
#define CHAT_ROW_LBL_W    40    // "5h"/"7d" column
#define CHAT_ROW_PCT_W    72    // percentage column (right-aligned, fixed)
#define CHAT_ROW_COL_GAP  12    // label|bar|pct column gap
#define CHAT_ROW_HALF_GAP 20    // between the 5h and 7d halves
#define CHAT_ROW_GAP      14    // strip band ↓ card list (plus 4, per #129)
#define CHAT_CARD_H       108
#define CHAT_CARD_GAP     10    // between cards (#129 ch_card_gap)
#define CHAT_CARD_PITCH   (CHAT_CARD_H + CHAT_CARD_GAP)
#define CHAT_CARD_PAD_Y   8
// Chat cards (and the ONE-CHAT quota box) bleed to the physical left/right
// edges — the card's own corner radius is the relief at the glass edge. The
// inner side padding keeps text at the same 20px inset the old screen margin
// provided, clear of the panel's rounded corners.
#define CHAT_CARD_PAD_X   20
#define CHAT_FADE_H       60    // bottom fade band: transparent → panel black
// ONE-CHAT: two boxes — the 5h quota panel (exact RESTING "Current" panel)
// on top, the chat card below it.
#define FOCUS_CARD_H      176
#define FOCUS_PANEL_GAP   16    // 5h panel ↔ chat card
#define SESSION_FONT_NAME_FOCUS font_styrene_48
#define SESSION_FONT_NAME_LIST  font_styrene_28
#define SESSION_FONT_LINE       font_styrene_24
#define SESSION_FONT_STRIP_TAG  font_styrene_24
#define SESSION_FONT_STRIP_PCT  font_styrene_28
#define SESSION_FONT_MODEL      font_styrene_24
#define SESSION_DOT_SZ          14
#define SESSION_BAR_H_FOCUS     12
#define SESSION_BAR_H_LIST      8
#define SESSION_BAR_OFFSET_FOCUS (-40)
#define SESSION_BAR_OFFSET_LIST  (-38)
#define SESSION_ICON_TODO(focus)    ((focus) ? &icon_todo_dsc : &icon_todo_small_dsc)
#define SESSION_ICON_AGENTS(focus)  ((focus) ? &icon_agents_dsc : &icon_agents_small_dsc)
#define SESSION_STATE_LABEL_W       200
#define SESSION_FOCUS_ROW2_Y        64
#endif  // SESSION_COMPACT

// LVGL's built-in scroll pause (LV_LABEL_SCROLL_DELAY, lv_label.c) is a fixed
// 300ms baked into the vendored library — not ours to edit (firmware/.pio is
// regenerated, gitignored). The public override is a style-level anim
// template: lv_label_refr_text() copies reverse_delay/repeat_delay/repeat_cnt
// out of whatever lv_obj_set_style_anim() attached, when the label is in
// LV_LABEL_LONG_SCROLL mode. Longer than the default so a name is readable
// before it moves.
#define SESSION_NAME_SCROLL_PAUSE_MS 2500

// One chat card's widget set. Cards keep stable identity: each card widget is
// bound to a chat (keyed by sid), not to a slot, so a reorder moves the widget
// instead of mutating every row's contents (§2.3).
struct ChatCard {
    lv_obj_t* card;
    lv_obj_t* lbl_name;
    lv_obj_t* lbl_ctx;      // ctx% top-right (list cards only; focus has the big pct)
    lv_obj_t* lbl_model;    // model tag, left of ctx% (list cards only; focus has the pill)
    lv_obj_t* lbl_effort;   // effort tag, left of the model tag (list cards only)
    lv_obj_t* bar;          // context bar — hidden entirely when ctx is unknown
    lv_obj_t* dot;          // state indicator; pulses when waiting
    lv_obj_t* lbl_state;
    lv_obj_t* img_todo;
    lv_obj_t* lbl_todo;
    lv_obj_t* img_agents;
    lv_obj_t* lbl_agents;
    lv_obj_t* lbl_elapsed;
    const lv_font_t* name_font;  // for measuring the dynamic scroll width
    int  name_w;
    char sid[3];
    int  target_y;          // slide destination (list cards)
    bool used;
    bool waiting;
    bool claimed;           // per-update matching scratch
    bool name_scrolling;    // current lbl_name long_mode: SCROLL vs parked DOT
};

static lv_obj_t* focus_group = nullptr;   // ONE-CHAT (§1.3)
static lv_obj_t* chats_group = nullptr;   // SEVERAL-CHATS (§1.4)
static lv_obj_t* cards_cont  = nullptr;   // clipping viewport for the card list
static ChatCard  chat_cards[SESSION_MAX_ROWS];
static ChatCard  focus_card;
static lv_obj_t* focus_lbl_model  = nullptr;
static lv_obj_t* focus_lbl_effort = nullptr;  // effort pill, left of the model pill
static lv_obj_t* focus_lbl_ctx   = nullptr;   // context % (left, on its own row)
static lv_obj_t* focus_lbl_tok   = nullptr;   // token counter (right of the % row)

// Full-size 5h quota panel on the ONE-CHAT view — same anatomy and weight as
// the RESTING "Current" panel. The 7d row is deliberately absent here: it
// moves on a scale of days, stays one glance away on RESTING / SEVERAL-CHATS,
// and a slim extra row would crowd the chat card against the status line.
static lv_obj_t* f5_pct, *f5_pill, *f5_bar, *f5_reset;

// One-line quota strip (chats view)
static lv_obj_t* cq_tag[2], *cq_bar[2], *cq_pct[2];

static lv_image_dsc_t icon_todo_dsc, icon_todo_small_dsc;
static lv_image_dsc_t icon_agents_dsc, icon_agents_small_dsc;

// Resolver inputs (§2.1), fed by ui_update_sessions()
static uint8_t  s_live_count    = 0;      // rows in the last received list
static bool     s_any_waiting   = false;  // any row in the waiting bucket
static bool     s_any_active    = false;  // any row not idle (working or waiting)
static bool     s_focus_waiting = false;  // rows[0] waiting → status line hides
static bool     s_focus_active  = false;  // rows[0] not idle → status line shows
static bool     s_chats_linger  = false;  // holding a chat view after the last chat closed
static uint32_t s_chats_gone_ms = 0;
static int      s_linger_view   = 2;
static UsageData s_usage_cache  = {};     // latest quota payload, for the mini bars

enum {
    SESSION_BUCKET_IDLE    = 0,
    SESSION_BUCKET_WORKING = 1,
    SESSION_BUCKET_WAITING = 2,
};

static int session_bucket(uint8_t state) {
    if (state >= SESSION_WAITING_PERMISSION && state <= SESSION_ERROR)
        return SESSION_BUCKET_WAITING;
    if (state >= SESSION_THINKING && state <= SESSION_COMPACTING)
        return SESSION_BUCKET_WORKING;
    if (state <= SESSION_IDLE)
        return SESSION_BUCKET_IDLE;
    return SESSION_BUCKET_WORKING;  // unknown future codes render neutral, never alarming
}

static const char* const session_tool_names[] = {
    "tool", "Bash", "Read", "Edit", "Write",
    "Grep", "Glob", "Task", "WebFetch", "WebSearch",
};

static void session_state_text(const SessionRow* r, char* buf, size_t n) {
    switch (r->state) {
    case SESSION_STARTING:   snprintf(buf, n, "starting");   return;
    case SESSION_IDLE:       snprintf(buf, n, "idle");       return;
    case SESSION_THINKING:   snprintf(buf, n, "thinking");   return;
    case SESSION_RESPONDING: snprintf(buf, n, "responding"); return;
    case SESSION_RUNNING_TOOL:
        if (r->ntools > 1) snprintf(buf, n, "%d tools", r->ntools);
        else snprintf(buf, n, "running %s",
                      session_tool_names[r->tool <= SESSION_TOOL_WEBSEARCH ? r->tool : 0]);
        return;
    case SESSION_COMPACTING:         snprintf(buf, n, "compacting");       return;
    case SESSION_WAITING_PERMISSION: snprintf(buf, n, "needs permission"); return;
    case SESSION_WAITING_QUESTION:   snprintf(buf, n, "asking you");       return;
    case SESSION_WAITING_INPUT:      snprintf(buf, n, "needs input");      return;
    case SESSION_ERROR:              snprintf(buf, n, "error");            return;
    default:                         snprintf(buf, n, "busy");             return;
    }
}

// Render elapsed_s as-received: 8s / 40s / 1m / 4m / 2h / 3d.
static void session_elapsed_text(int32_t s, char* buf, size_t n) {
    if (s < 0) s = 0;
    if (s < 60)          snprintf(buf, n, "%ds", (int)s);
    else if (s < 3600)   snprintf(buf, n, "%dm", (int)(s / 60));
    else if (s < 86400)  snprintf(buf, n, "%dh", (int)(s / 3600));
    else                 snprintf(buf, n, "%dd", (int)(s / 86400));
}

// Render a token count (1k units): 412 → "412K", 1000 → "1.0M", 1234 → "1.2M".
static void session_tok_text(int32_t tok_k, char* buf, size_t n) {
    if (tok_k < 1000) snprintf(buf, n, "%dK", (int)tok_k);
    else              snprintf(buf, n, "%d.%dM", (int)(tok_k / 1000),
                               (int)((tok_k % 1000) / 100));
}

static const char* const session_model_names[] = { "", "opus", "sonnet", "haiku", "fable" };
static const char* session_model_text(uint8_t model) {
    return model <= SESSION_MODEL_FABLE ? session_model_names[model] : "";
}

// Short forms for the reasoning-effort pill — "xhigh" reads as a typo at this
// size, so it gets its own word instead of the raw wire name.
static const char* const session_effort_names[] = {
    "", "low", "mid", "high", "ultra", "max"
};
static const char* session_effort_text(uint8_t effort) {
    return effort <= SESSION_EFFORT_MAX ? session_effort_names[effort] : "";
}

// Style-level override for the name label's scroll pause — see
// SESSION_NAME_SCROLL_PAUSE_MS. lv_anim_init() zeroes fields we don't touch
// (act_time, exec/completed cb, values/duration) — lv_label_refr_text()
// fills those in itself and only borrows repeat_cnt/repeat_delay/
// reverse_delay from this template.
static lv_anim_t s_name_scroll_anim;
static bool      s_name_scroll_anim_ready = false;
static const lv_anim_t* name_scroll_anim_template(void) {
    if (!s_name_scroll_anim_ready) {
        lv_anim_init(&s_name_scroll_anim);
        lv_anim_set_repeat_count(&s_name_scroll_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_reverse_delay(&s_name_scroll_anim, SESSION_NAME_SCROLL_PAUSE_MS);
        lv_anim_set_repeat_delay(&s_name_scroll_anim, SESSION_NAME_SCROLL_PAUSE_MS);
        s_name_scroll_anim_ready = true;
    }
    return &s_name_scroll_anim;
}

// A routine content refresh must not animate anything (§2.3) — and rewriting
// a label always invalidates it, so compare first.
static void set_label_if_changed(lv_obj_t* lbl, const char* txt) {
    if (strcmp(lv_label_get_text(lbl), txt) != 0) lv_label_set_text(lbl, txt);
}

// ---- The pulse (§2.3) ----
// One module-level animation drives a shared value every waiting indicator
// reads, so two chats waiting at once pulse in unison. Only waiting states
// pulse; the text never does.
static int32_t pulse_val = (int32_t)LV_OPA_COVER;

static void pulse_exec_cb(void* var, int32_t v) {
    (void)var;
    pulse_val = v;
    // Indicator and state text pulse together, in one shared phase.
    for (auto& c : chat_cards)
        if (c.used && c.waiting) {
            lv_obj_set_style_bg_opa(c.dot, (lv_opa_t)v, 0);
            lv_obj_set_style_text_opa(c.lbl_state, (lv_opa_t)v, 0);
        }
    if (s_focus_waiting && focus_card.dot) {
        lv_obj_set_style_bg_opa(focus_card.dot, (lv_opa_t)v, 0);
        lv_obj_set_style_text_opa(focus_card.lbl_state, (lv_opa_t)v, 0);
    }
}

// ---- Card motion callbacks (§2.3) ----
static void card_y_anim_cb(void* obj, int32_t v)   { lv_obj_set_y((lv_obj_t*)obj, v); }
// Plain opa, never opa_layered: the recursive style lookup in the draw path
// fades the whole subtree without allocating a composite buffer (§7).
static void card_opa_anim_cb(void* obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0); }
static void card_fadeout_done_cb(lv_anim_t* a)     { lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN); }

// Idle cards recede wholesale (§3): plain opa on the card fades labels, bar
// and both icons through the recursive style lookup.
static lv_opa_t session_tier_opa(uint8_t state) {
    return session_bucket(state) == SESSION_BUCKET_IDLE ? LV_OPA_60 : LV_OPA_COVER;
}

// ---- Card construction ----

static lv_obj_t* make_badge_icon(lv_obj_t* parent, const lv_image_dsc_t* dsc, lv_color_t col) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    // Runtime recolor of the white-tinted RGB565A8 glyph — the badge color
    // rides on the same draw path as the labels, so the idle card's 60%
    // dimming still applies. Position comes from layout_badge_cluster().
    lv_obj_set_style_image_recolor(img, col, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

static lv_obj_t* make_card_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t col) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, col, 0);
    return lbl;
}

// Build one chat card. Both anatomies are §1.1's three lines — name row,
// context bar, state line — the focus variant is just bigger and swaps the
// top-right ctx% for the model name + a big percentage.
static void build_chat_card(ChatCard* c, lv_obj_t* parent, int x, int y, bool focus) {
    const lv_font_t* f_name = focus ? &SESSION_FONT_NAME_FOCUS : &SESSION_FONT_NAME_LIST;
    const lv_font_t* f_line = &SESSION_FONT_LINE;
    const int h = focus ? FOCUS_CARD_H : CHAT_CARD_H;

    c->card = make_panel(parent, x, y, L.scr_w, h);   // full-bleed (see CHAT_CARD_PAD_X)
    lv_obj_set_style_pad_left(c->card, CHAT_CARD_PAD_X, 0);
    lv_obj_set_style_pad_right(c->card, CHAT_CARD_PAD_X, 0);
    if (!focus) {
        lv_obj_set_style_pad_top(c->card, CHAT_CARD_PAD_Y, 0);
        lv_obj_set_style_pad_bottom(c->card, CHAT_CARD_PAD_Y, 0);
    }

    const int cw = L.scr_w - 2 * CHAT_CARD_PAD_X;

    // Name width starts at the full row; every content update re-budgets it
    // against the measured width of the actual right-side neighbor (the model
    // pill on the focus card, the token label on list cards) so a long name
    // scrolls right up to its neighbor instead of stopping short at a
    // worst-case gap.
    c->name_font = f_name;
    c->name_w = cw;
    c->lbl_name = make_card_label(c->card, f_name, COL_TEXT);
    lv_obj_set_width(c->lbl_name, cw);
    // Long names/prompts scroll through the available width instead of
    // being cut short with an ellipsis — LVGL only animates when the text
    // is actually wider than the box, so short names just sit still. One
    // line, exactly, so it never wraps to a second line first. Ping-pong
    // (pauses at each end, then reverses) rather than a one-way circular
    // loop — trying this mode per user request, easy to flip back. The
    // style-anim template lengthens the pause at each end past LVGL's
    // built-in 300ms default (SESSION_NAME_SCROLL_PAUSE_MS).
    lv_label_set_long_mode(c->lbl_name, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_anim(c->lbl_name, name_scroll_anim_template(), 0);
    lv_obj_set_height(c->lbl_name, lv_font_get_line_height(f_name));
    lv_obj_align(c->lbl_name, LV_ALIGN_TOP_LEFT, 0, 0);

    if (!focus) {
        c->lbl_ctx = make_card_label(c->card, f_name, COL_TEXT);
        lv_obj_align(c->lbl_ctx, LV_ALIGN_TOP_RIGHT, 0, 0);
        // Same pill treatment as the focus card's model chip, scaled down.
        c->lbl_model = make_card_label(c->card, &SESSION_FONT_MODEL, COL_TEXT);
        lv_obj_set_style_bg_color(c->lbl_model, COL_BAR_BG, 0);
        lv_obj_set_style_bg_opa(c->lbl_model, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->lbl_model, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_left(c->lbl_model, 6, 0);
        lv_obj_set_style_pad_right(c->lbl_model, 6, 0);
        lv_obj_set_style_pad_top(c->lbl_model, 2, 0);
        lv_obj_set_style_pad_bottom(c->lbl_model, 2, 0);
        lv_obj_align(c->lbl_model, LV_ALIGN_TOP_RIGHT, 0, 0);
        c->lbl_effort = make_card_label(c->card, &SESSION_FONT_MODEL, COL_TEXT);
        lv_obj_set_style_bg_color(c->lbl_effort, COL_BAR_BG, 0);
        lv_obj_set_style_bg_opa(c->lbl_effort, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->lbl_effort, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_left(c->lbl_effort, 6, 0);
        lv_obj_set_style_pad_right(c->lbl_effort, 6, 0);
        lv_obj_set_style_pad_top(c->lbl_effort, 2, 0);
        lv_obj_set_style_pad_bottom(c->lbl_effort, 2, 0);
        lv_obj_align(c->lbl_effort, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        c->lbl_ctx = nullptr;
        c->lbl_model = nullptr;
        c->lbl_effort = nullptr;
    }

    c->bar = make_bar(c->card, 0, 0, cw, focus ? SESSION_BAR_H_FOCUS : SESSION_BAR_H_LIST);
    lv_obj_set_style_bg_color(c->bar, COL_DIM, LV_PART_INDICATOR);  // context stays neutral (§1.3)
    lv_obj_align(c->bar, LV_ALIGN_BOTTOM_LEFT, 0,
                focus ? SESSION_BAR_OFFSET_FOCUS : SESSION_BAR_OFFSET_LIST);

    // State line — everything shares one visual center line, `line_c` px above
    // the card content's bottom edge. The dot sits flush with the card's left
    // content edge (same x as the name and the bar above it).
    const int dot_sz  = SESSION_DOT_SZ;
    const int line_h  = lv_font_get_line_height(f_line);
    const int line_dy = focus ? -2 : 0;   // base line, from content bottom
    // Dot center = label line-box center (measured: Styrene's lowercase
    // x-height band centers on its line box). On list cards the text rides
    // 1 px lower than the box math — user-tuned against hardware.
    const int dot_dy  = line_dy - (line_h - dot_sz) / 2;
    const int text_dy = focus ? line_dy : line_dy + 1;

    c->dot = lv_obj_create(c->card);
    lv_obj_set_size(c->dot, dot_sz, dot_sz);
    lv_obj_set_style_radius(c->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c->dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(c->dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c->dot, 0, 0);
    lv_obj_set_style_pad_all(c->dot, 0, 0);
    lv_obj_add_flag(c->dot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(c->dot, LV_ALIGN_BOTTOM_LEFT, 0, dot_dy);
    c->lbl_state = make_card_label(c->card, f_line, COL_DIM);
    lv_label_set_long_mode(c->lbl_state, LV_LABEL_LONG_DOT);  // state ellipsizes before a badge drops (§5)
    lv_obj_set_width(c->lbl_state, SESSION_STATE_LABEL_W);
    // One text line, exactly: with a free-growing height an over-long state
    // would wrap to a second line instead of taking the DOT ellipsis.
    lv_obj_set_height(c->lbl_state, line_h);
    lv_obj_align(c->lbl_state, LV_ALIGN_BOTTOM_LEFT, dot_sz + 8, text_dy);

    // Badges + timer form a right-aligned cluster (timer rightmost); their x
    // positions are recomputed per update in layout_badge_cluster().
    // Badge colors: todo = terra-cotta accent, subagents = the palette's
    // muted purple; icon and count share the color so each badge reads as
    // one unit.
    c->img_todo = make_badge_icon(c->card, SESSION_ICON_TODO(focus),
                                  COL_ACCENT);
    c->lbl_todo = make_card_label(c->card, f_line, COL_ACCENT);
    c->img_agents = make_badge_icon(c->card, SESSION_ICON_AGENTS(focus),
                                    COL_PURPLE);
    c->lbl_agents = make_card_label(c->card, f_line, COL_PURPLE);

    c->lbl_elapsed = make_card_label(c->card, f_line, COL_DIM);
    lv_obj_align(c->lbl_elapsed, LV_ALIGN_BOTTOM_RIGHT, 0, text_dy);

    c->sid[0] = 0;
    c->target_y = -1;
    c->used = c->waiting = c->claimed = false;
    c->name_scrolling = true;   // matches the LV_LABEL_LONG_SCROLL set above
}

// Right-align the badge cluster: timer rightmost, subagent badge to its left,
// todo badge left of that, evenly spaced. Chained from the timer so hidden
// badges leave no gap. Runs after the labels' texts are set (layout works on
// hidden subtrees too — layout_update_core doesn't skip LV_OBJ_FLAG_HIDDEN).
static void layout_badge_cluster(ChatCard* c) {
    const int gap = 20;       // between cluster items
    const int icon_gap = 6;   // icon ↔ its count
    lv_obj_update_layout(c->card);
    lv_obj_t* anchor = c->lbl_elapsed;
    if (!lv_obj_has_flag(c->img_agents, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align_to(c->lbl_agents, anchor, LV_ALIGN_OUT_LEFT_MID, -gap, 0);
        lv_obj_align_to(c->img_agents, c->lbl_agents, LV_ALIGN_OUT_LEFT_MID, -icon_gap, 0);
        anchor = c->img_agents;
    }
    if (!lv_obj_has_flag(c->img_todo, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align_to(c->lbl_todo, anchor, LV_ALIGN_OUT_LEFT_MID, -gap, 0);
        lv_obj_align_to(c->img_todo, c->lbl_todo, LV_ALIGN_OUT_LEFT_MID, -icon_gap, 0);
    }
}

// Fill a card from a row. Content only — no motion here (§2.3).
static void chat_card_set_row(ChatCard* c, const SessionRow* r) {
    const int bucket = session_bucket(r->state);
    c->waiting = (bucket == SESSION_BUCKET_WAITING);

    char buf[24];
    // Top-right cluster (list cards): effort tag, model tag, then token
    // count (or ctx% as the older-host fallback) — right-aligned and chained
    // like the bottom badge cluster. Set BEFORE the name so the name's
    // scroll width can track the rendered width of its actual neighbors
    // (hidden labels give the name back their share of the row).
    if (c->lbl_ctx) {
        const int cw = L.scr_w - 2 * CHAT_CARD_PAD_X;
        bool shown = true;
        if (r->tok >= 0) {
            session_tok_text(r->tok, buf, sizeof(buf));
        } else if (r->ctx_pct >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", r->ctx_pct);
        } else {
            shown = false;
        }
        int nw = cw;
        if (shown) {
            set_label_if_changed(c->lbl_ctx, buf);
            lv_obj_clear_flag(c->lbl_ctx, LV_OBJ_FLAG_HIDDEN);
            lv_point_t sz;
            lv_text_get_size(&sz, buf, c->name_font, 0, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            nw = cw - sz.x - 12;   // min gap between name end and neighbor
        } else {
            lv_obj_add_flag(c->lbl_ctx, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_t* anchor = shown ? c->lbl_ctx : nullptr;

        // Effort sits closest to ctx% (rightmost of the two tags), model
        // chains off its left — reads left-to-right as "model, effort, ctx%".
        const char* effort = session_effort_text(r->effort);
        if (effort[0]) {
            set_label_if_changed(c->lbl_effort, effort);
            lv_obj_clear_flag(c->lbl_effort, LV_OBJ_FLAG_HIDDEN);
            if (anchor) lv_obj_align_to(c->lbl_effort, anchor, LV_ALIGN_OUT_LEFT_MID, -6, 0);
            else        lv_obj_align(c->lbl_effort, LV_ALIGN_TOP_RIGHT, 0, 0);
            lv_point_t esz;
            lv_text_get_size(&esz, effort, &SESSION_FONT_MODEL, 0, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            nw -= esz.x + 2 * 6 /*pill pad*/ + 6 /*gap*/;
            anchor = c->lbl_effort;
        } else {
            lv_obj_add_flag(c->lbl_effort, LV_OBJ_FLAG_HIDDEN);
        }

        const char* model = session_model_text(r->model);
        if (model[0]) {
            set_label_if_changed(c->lbl_model, model);
            lv_obj_clear_flag(c->lbl_model, LV_OBJ_FLAG_HIDDEN);
            if (anchor) lv_obj_align_to(c->lbl_model, anchor, LV_ALIGN_OUT_LEFT_MID, -6, 0);
            else        lv_obj_align(c->lbl_model, LV_ALIGN_TOP_RIGHT, 0, 0);
            lv_point_t msz;
            lv_text_get_size(&msz, model, &SESSION_FONT_MODEL, 0, 0,
                             LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            nw -= msz.x + 2 * 6 /*pill pad*/ + 6 /*gap*/;
        } else {
            lv_obj_add_flag(c->lbl_model, LV_OBJ_FLAG_HIDDEN);
        }

        if (nw != c->name_w) {
            c->name_w = nw;
            lv_obj_set_width(c->lbl_name, nw);
        }
    }

    // Scroll only while the chat is doing something; an idle chat's name
    // sits parked at the start instead of drawing the eye with motion that
    // has nothing to report. Plain CLIP, not DOT: this is a "paused", not
    // "shortened", state — it shows as much of the real name as fits with a
    // hard edge, no "..." implying the rest is gone forever, since the full
    // name resumes scrolling the moment the chat wakes back up.
    // lv_label_set_long_mode() always restarts the animation (no
    // same-value guard in LVGL), so only call it on a change.
    const bool want_scroll = (bucket != SESSION_BUCKET_IDLE);
    if (want_scroll != c->name_scrolling) {
        c->name_scrolling = want_scroll;
        lv_label_set_long_mode(c->lbl_name,
            want_scroll ? LV_LABEL_LONG_SCROLL : LV_LABEL_LONG_CLIP);
    }
    set_label_if_changed(c->lbl_name, r->label);

    // Context bar: hidden entirely when the percentage is unknown — an empty
    // bar reads as "0% used" (§5).
    if (r->ctx_pct < 0) {
        lv_obj_add_flag(c->bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(c->bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(c->bar, r->ctx_pct, LV_ANIM_OFF);
    }

    char sbuf[32];
    session_state_text(r, sbuf, sizeof(sbuf));
    set_label_if_changed(c->lbl_state, sbuf);
    lv_obj_set_style_text_color(c->lbl_state, c->waiting ? COL_ACCENT : COL_DIM, 0);
    // Waiting text pulses in phase with the indicator; anything else is solid.
    lv_obj_set_style_text_opa(c->lbl_state, c->waiting ? (lv_opa_t)pulse_val : LV_OPA_COVER, 0);

    // Indicator: dim when idle, neutral when working, accent + pulse when
    // the session needs a human (§1.1).
    lv_obj_set_style_bg_color(c->dot,
        c->waiting ? COL_ACCENT :
        (bucket == SESSION_BUCKET_WORKING) ? COL_TEXT : COL_DIM, 0);
    lv_obj_set_style_bg_opa(c->dot, c->waiting ? (lv_opa_t)pulse_val : LV_OPA_COVER, 0);

    // Badges are hidden when they'd say nothing (§1.1).
    if (r->ttotal > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", r->tdone, r->ttotal);
        set_label_if_changed(c->lbl_todo, buf);
        lv_obj_clear_flag(c->img_todo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(c->lbl_todo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->img_todo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->lbl_todo, LV_OBJ_FLAG_HIDDEN);
    }
    if (r->nagents > 0) {
        snprintf(buf, sizeof(buf), "%d", r->nagents);
        set_label_if_changed(c->lbl_agents, buf);
        lv_obj_clear_flag(c->img_agents, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(c->lbl_agents, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c->img_agents, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->lbl_agents, LV_OBJ_FLAG_HIDDEN);
    }

    session_elapsed_text(r->elapsed_s, buf, sizeof(buf));
    set_label_if_changed(c->lbl_elapsed, buf);

    layout_badge_cluster(c);
}

static void focus_set_content(const SessionRow* r) {
    // Budget the name against the pill(s) actually rendered (text width +
    // padding + a small gap, chained right-to-left) — not a worst case — so a
    // long name runs right up to the nearest pill. An empty pill would
    // render as a bare chip: hide it with its text and give the name back
    // that share of the row. Effort sits at the outer/right edge, model
    // chains off its left — reads left-to-right as "model, effort".
    const int cw = L.scr_w - 2 * CHAT_CARD_PAD_X;
    int nw = cw;
    lv_obj_t* anchor = nullptr;

    const char* effort = session_effort_text(r->effort);
    set_label_if_changed(focus_lbl_effort, effort);
    if (effort[0]) {
        lv_obj_clear_flag(focus_lbl_effort, LV_OBJ_FLAG_HIDDEN);
        lv_point_t esz;
        lv_text_get_size(&esz, effort, &SESSION_FONT_MODEL, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        nw -= esz.x + 2 * 12 /*pill pad*/ + 4 /*gap*/;
        anchor = focus_lbl_effort;
    } else {
        lv_obj_add_flag(focus_lbl_effort, LV_OBJ_FLAG_HIDDEN);
    }

    const char* model = session_model_text(r->model);
    set_label_if_changed(focus_lbl_model, model);
    if (model[0]) {
        lv_obj_clear_flag(focus_lbl_model, LV_OBJ_FLAG_HIDDEN);
        if (anchor) lv_obj_align_to(focus_lbl_model, anchor, LV_ALIGN_OUT_LEFT_MID, -4, 0);
        else        lv_obj_align(focus_lbl_model, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_point_t sz;
        lv_text_get_size(&sz, model, &SESSION_FONT_MODEL, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        nw -= sz.x + 2 * 12 /*pill pad*/ + 4 /*gap*/;
    } else {
        lv_obj_add_flag(focus_lbl_model, LV_OBJ_FLAG_HIDDEN);
    }

    if (nw != focus_card.name_w) {
        focus_card.name_w = nw;
        lv_obj_set_width(focus_card.lbl_name, nw);
    }

    chat_card_set_row(&focus_card, r);
    s_focus_waiting = focus_card.waiting;
    s_focus_active = (session_bucket(r->state) != SESSION_BUCKET_IDLE);

    // Context row: percentage (spelled out — it doubles as onboarding for
    // the terse multi-chat bars) on the left, token counter on the right.
    char buf[32];
    if (r->ctx_pct < 0) {
        lv_obj_add_flag(focus_lbl_ctx, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buf, sizeof(buf), "%d%% of context used", r->ctx_pct);
        set_label_if_changed(focus_lbl_ctx, buf);
        lv_obj_clear_flag(focus_lbl_ctx, LV_OBJ_FLAG_HIDDEN);
    }
    if (r->tok < 0) {
        lv_obj_add_flag(focus_lbl_tok, LV_OBJ_FLAG_HIDDEN);
    } else {
        session_tok_text(r->tok, buf, sizeof(buf));
        set_label_if_changed(focus_lbl_tok, buf);
        lv_obj_clear_flag(focus_lbl_tok, LV_OBJ_FLAG_HIDDEN);
    }

    lv_anim_delete(focus_card.card, card_opa_anim_cb);
    lv_obj_set_style_opa(focus_card.card, session_tier_opa(r->state), 0);
}

// ---- Card pool: identity-stable matching + the reorder slide (§2.3) ----

static ChatCard* chat_card_by_sid(const char* sid) {
    for (auto& c : chat_cards)
        if (c.used && !c.claimed && strcmp(c.sid, sid) == 0) return &c;
    return nullptr;
}

static ChatCard* chat_card_alloc(void) {
    for (auto& c : chat_cards)   // prefer a fully retired card…
        if (!c.used && lv_obj_has_flag(c.card, LV_OBJ_FLAG_HIDDEN)) return &c;
    for (auto& c : chat_cards) { // …else steal one mid-fade-out
        if (!c.used) { lv_anim_delete(c.card, NULL); return &c; }
    }
    return nullptr;              // unreachable: pool size == max rows
}

static void chats_set_content(const SessionList* list) {
    // Motion only while the list is on screen: entering the view (or updating
    // it while another view is up) positions everything instantly.
    const bool animate = (view_state == 4);

    for (auto& c : chat_cards) c.claimed = false;

    for (int i = 0; i < list->count; i++) {
        const SessionRow* r = &list->rows[i];
        const int target_y = i * CHAT_CARD_PITCH;
        ChatCard* c = chat_card_by_sid(r->sid);
        if (c) {
            c->claimed = true;
            chat_card_set_row(c, r);
            lv_anim_delete(c->card, card_opa_anim_cb);   // cancel a stale fade before restyling
            lv_obj_set_style_opa(c->card, session_tier_opa(r->state), 0);
            if (c->target_y != target_y) {
                c->target_y = target_y;
                lv_anim_delete(c->card, card_y_anim_cb);  // re-sort mid-flight: restart from here
                if (animate) {
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, c->card);
                    lv_anim_set_exec_cb(&a, card_y_anim_cb);
                    lv_anim_set_values(&a, lv_obj_get_y(c->card), target_y);
                    lv_anim_set_duration(&a, 260);
                    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                    lv_anim_start(&a);
                } else {
                    lv_obj_set_y(c->card, target_y);
                }
            }
        } else {
            c = chat_card_alloc();
            if (!c) continue;
            snprintf(c->sid, sizeof(c->sid), "%s", r->sid);
            c->used = true;
            c->claimed = true;
            c->target_y = target_y;
            lv_anim_delete(c->card, NULL);
            lv_obj_set_y(c->card, target_y);
            chat_card_set_row(c, r);
            lv_obj_clear_flag(c->card, LV_OBJ_FLAG_HIDDEN);
            const lv_opa_t tier = session_tier_opa(r->state);
            if (animate) {
                // New chat: fade in at its slot (§2.3)
                lv_obj_set_style_opa(c->card, LV_OPA_TRANSP, 0);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, c->card);
                lv_anim_set_exec_cb(&a, card_opa_anim_cb);
                lv_anim_set_values(&a, LV_OPA_TRANSP, tier);
                lv_anim_set_duration(&a, 260);
                lv_anim_start(&a);
            } else {
                lv_obj_set_style_opa(c->card, tier, 0);
            }
        }
    }

    // Chats that closed: fade out; their slot is reclaimed by the slide (§2.3).
    for (auto& c : chat_cards) {
        if (!c.used || c.claimed) continue;
        c.used = false;
        c.waiting = false;
        c.sid[0] = 0;
        c.target_y = -1;
        lv_anim_delete(c.card, NULL);
        if (animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, c.card);
            lv_anim_set_exec_cb(&a, card_opa_anim_cb);
            lv_anim_set_values(&a, lv_obj_get_style_opa(c.card, LV_PART_MAIN), LV_OPA_TRANSP);
            lv_anim_set_duration(&a, 260);
            lv_anim_set_completed_cb(&a, card_fadeout_done_cb);
            lv_anim_start(&a);
        } else {
            lv_obj_add_flag(c.card, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Refresh the chat views' quota widgets from the cached usage payload. Values
// match the RESTING panels; enterprise accounts map spending → slot 1,
// period → slot 2.
static void session_quota_refresh(void) {
    if (!focus_group || !s_usage_cache.valid) return;
    const UsageData* d = &s_usage_cache;

    const int s_pct = (int)(d->session_pct + 0.5f);
    const int w_pct = d->enterprise ? d->time_pct : (int)(d->weekly_pct + 0.5f);
    const lv_color_t col0 = pct_color(d->session_pct);
    const lv_color_t col1 = pct_color((float)w_pct);

    char pct0[8], pct1[8], buf[48];
    snprintf(pct0, sizeof(pct0), "%d%%", s_pct);
    snprintf(pct1, sizeof(pct1), "%d%%", w_pct);

    // ONE-CHAT 5h panel — same treatment as the RESTING "Current" panel.
    set_label_if_changed(f5_pct, pct0);
    set_label_if_changed(f5_pill, d->enterprise ? "Spending" : "Current");
    lv_bar_set_value(f5_bar, s_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(f5_bar, col0, LV_PART_INDICATOR);
    if (d->enterprise) {
        lv_obj_add_flag(f5_reset, LV_OBJ_FLAG_HIDDEN);  // spending has no reset clock
    } else {
        format_reset_time(d->session_reset_mins, buf, sizeof(buf));
        set_label_if_changed(f5_reset, buf);
        lv_obj_clear_flag(f5_reset, LV_OBJ_FLAG_HIDDEN);
    }

    // SEVERAL-CHATS one-line strip.
    set_label_if_changed(cq_tag[0], d->enterprise ? "$"  : "5h");
    set_label_if_changed(cq_tag[1], d->enterprise ? "pd" : "7d");
    set_label_if_changed(cq_pct[0], pct0);
    set_label_if_changed(cq_pct[1], pct1);
    lv_bar_set_value(cq_bar[0], s_pct, LV_ANIM_OFF);
    lv_bar_set_value(cq_bar[1], w_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(cq_bar[0], col0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cq_bar[1], col1, LV_PART_INDICATOR);
}

// Transparent full-screen group, same pattern as usage_group / pair_group.
static lv_obj_t* make_session_group(lv_obj_t* parent) {
    lv_obj_t* g = lv_obj_create(parent);
    lv_obj_set_size(g, L.scr_w, L.scr_h);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);   // update_view_state decides
    return g;
}

static void build_session_views(lv_obj_t* parent) {
    init_icon_dsc_rgb565a8(&icon_todo_dsc, ICON_LIST_TODO_W, ICON_LIST_TODO_H, icon_list_todo_data);
    init_icon_dsc_rgb565a8(&icon_todo_small_dsc, ICON_LIST_TODO_SMALL_W, ICON_LIST_TODO_SMALL_H, icon_list_todo_small_data);
    init_icon_dsc_rgb565a8(&icon_agents_dsc, ICON_USERS_ROUND_W, ICON_USERS_ROUND_H, icon_users_round_data);
    init_icon_dsc_rgb565a8(&icon_agents_small_dsc, ICON_USERS_ROUND_SMALL_W, ICON_USERS_ROUND_SMALL_H, icon_users_round_small_data);

    // ---- ONE-CHAT (§1.3): two boxes ----
    // The 5h quota panel (exact RESTING "Current" panel — it carries the most
    // minute-to-minute value) on top, the chat card below it. One box per
    // concern, so a future multi-account build gets a box per account. The
    // 7d row is dropped here (see the f5_* rationale).
    focus_group = make_session_group(parent);
    lv_obj_t* p5 = make_usage_panel(focus_group, L.content_y, "Current",
                                    &f5_pct, &f5_pill, &f5_bar, &f5_reset);
    // Full-bleed like the chat card below it. make_usage_panel stays shared
    // with the untouched RESTING view, so the width/pad/bar adjustments are
    // applied here instead: text keeps a 20px inset from the glass edge.
    lv_obj_set_pos(p5, 0, L.content_y);
    lv_obj_set_size(p5, L.scr_w, L.usage_panel_h);
    lv_obj_set_style_pad_left(p5, CHAT_CARD_PAD_X, 0);
    lv_obj_set_style_pad_right(p5, CHAT_CARD_PAD_X, 0);
    lv_obj_set_width(f5_bar, L.scr_w - 2 * CHAT_CARD_PAD_X);
    const int focus_card_y = L.content_y + L.usage_panel_h + FOCUS_PANEL_GAP;
    build_chat_card(&focus_card, focus_group, 0, focus_card_y, true);

    // Chat card extras: model pill (quota-pill treatment at the chat card's
    // own text size) + the context line.
    focus_lbl_model = make_card_label(focus_card.card, &SESSION_FONT_MODEL, COL_TEXT);
    lv_obj_set_style_bg_color(focus_lbl_model, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(focus_lbl_model, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(focus_lbl_model, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(focus_lbl_model, 12, 0);
    lv_obj_set_style_pad_right(focus_lbl_model, 12, 0);
    lv_obj_set_style_pad_top(focus_lbl_model, 4, 0);
    lv_obj_set_style_pad_bottom(focus_lbl_model, 4, 0);
    lv_obj_align(focus_lbl_model, LV_ALIGN_TOP_RIGHT, 0, 0);
    // Same pill treatment, chained to the model pill's left in focus_set_content().
    focus_lbl_effort = make_card_label(focus_card.card, &SESSION_FONT_MODEL, COL_TEXT);
    lv_obj_set_style_bg_color(focus_lbl_effort, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(focus_lbl_effort, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(focus_lbl_effort, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(focus_lbl_effort, 12, 0);
    lv_obj_set_style_pad_right(focus_lbl_effort, 12, 0);
    lv_obj_set_style_pad_top(focus_lbl_effort, 4, 0);
    lv_obj_set_style_pad_bottom(focus_lbl_effort, 4, 0);
    lv_obj_align(focus_lbl_effort, LV_ALIGN_TOP_RIGHT, 0, 0);
    focus_lbl_ctx = make_card_label(focus_card.card, &SESSION_FONT_MODEL, COL_TEXT);
    lv_obj_align(focus_lbl_ctx, LV_ALIGN_TOP_LEFT, 0, SESSION_FOCUS_ROW2_Y);
    focus_lbl_tok = make_card_label(focus_card.card, &SESSION_FONT_MODEL, COL_TEXT);
    lv_obj_align(focus_lbl_tok, LV_ALIGN_TOP_RIGHT, 0, SESSION_FOCUS_ROW2_Y);

    // ---- SEVERAL-CHATS (§1.4): one-line quota strip + the card list ----
    chats_group = make_session_group(parent);
    // Combined 5h/7d row, PR #129 geometry: [dim tag | bar | big pct] twice,
    // each element vertically centered in the CHAT_ROW_H band.
    const int half = (L.content_w - CHAT_ROW_HALF_GAP) / 2;
    const int strip_bar_w = half - CHAT_ROW_LBL_W - CHAT_ROW_PCT_W - 2 * CHAT_ROW_COL_GAP;
    const int tag_y = L.content_y +
        (CHAT_ROW_H - lv_font_get_line_height(&SESSION_FONT_STRIP_TAG)) / 2;
    const int pct_y = L.content_y +
        (CHAT_ROW_H - lv_font_get_line_height(&SESSION_FONT_STRIP_PCT)) / 2;
    for (int i = 0; i < 2; i++) {
        const int x0 = L.margin + i * (half + CHAT_ROW_HALF_GAP);
        cq_tag[i] = make_card_label(chats_group, &SESSION_FONT_STRIP_TAG, COL_DIM);
        lv_obj_set_width(cq_tag[i], CHAT_ROW_LBL_W);
        lv_obj_set_pos(cq_tag[i], x0, tag_y);
        cq_bar[i] = make_bar(chats_group,
                             x0 + CHAT_ROW_LBL_W + CHAT_ROW_COL_GAP,
                             L.content_y + (CHAT_ROW_H - CHAT_ROW_BAR_H) / 2,
                             strip_bar_w, CHAT_ROW_BAR_H);
        cq_pct[i] = make_card_label(chats_group, &SESSION_FONT_STRIP_PCT, COL_TEXT);
        lv_obj_set_width(cq_pct[i], CHAT_ROW_PCT_W);
        lv_obj_set_style_text_align(cq_pct[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(cq_pct[i],
                       x0 + CHAT_ROW_LBL_W + 2 * CHAT_ROW_COL_GAP + strip_bar_w,
                       pct_y);
    }

    // Card viewport: runs from the strip to the PHYSICAL bottom edge — this
    // sub-view alone drops the bottom margin, because clipped content tapers
    // out through the fade band below instead of hitting a hard cut (§2.5's
    // mid-card affordance, softened; the 4th card shows 62 of 80 px, the last
    // 60 of them fading). Side margins stay. Vertical touch scroll reaches
    // whatever's below the fold — attention-first ordering (the host
    // pre-sorts §5) keeps the most urgent chats visible without scrolling,
    // but with enough sessions open the rest would otherwise be permanently
    // unreachable, not just momentarily off-screen.
    //
    // Native LVGL scrolling (LV_OBJ_FLAG_SCROLLABLE): a hand-rolled version
    // driven from a card's bubbled LV_EVENT_PRESSING (to flip the direction
    // — LVGL has no per-object flag for that; it's baked into the generic
    // indev press handler in lv_indev_scroll.c) didn't actually move
    // anything on hardware and wasn't worth debugging blind. Native scroll's
    // target resolution walks up from the touched point looking for the
    // nearest LV_OBJ_FLAG_SCROLLABLE ancestor directly — a separate,
    // simpler, battle-tested path that doesn't depend on event bubbling at
    // all, which is likely exactly why it's the reliable option here.
    // Direction follows LVGL's standard convention (drag up reveals lower
    // content) for now.
    const int list_y = L.content_y + CHAT_ROW_H + CHAT_ROW_GAP + 4;  // #129 ch_list_y
    const int list_h = L.scr_h - list_y;
    cards_cont = lv_obj_create(chats_group);
    lv_obj_set_pos(cards_cont, 0, list_y);              // full-bleed card column
    lv_obj_set_size(cards_cont, L.scr_w, list_h);
    lv_obj_set_style_bg_opa(cards_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards_cont, 0, 0);
    lv_obj_set_style_pad_all(cards_cont, 0, 0);
    lv_obj_add_flag(cards_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cards_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cards_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(cards_cont, LV_OBJ_FLAG_EVENT_BUBBLE);

    for (auto& c : chat_cards) {
        build_chat_card(&c, cards_cont, 0, 0, false);
        lv_obj_add_flag(c.card, LV_OBJ_FLAG_HIDDEN);
    }

    // Bottom fade: whatever pokes below the fold tapers into the panel's
    // bottom edge instead of ending in a hard cut + dead black band. A pure
    // style gradient — per-end background opacity, no intermediate buffers —
    // so PSRAM-free ports can enable it as-is. Created after the card pool:
    // it's a later sibling of cards_cont, so reorder slides and pool churn
    // inside the container can never draw above it. Input-transparent (not
    // clickable), so the tap-anywhere splash toggle works through it.
    lv_obj_t* fade = lv_obj_create(chats_group);
    lv_obj_set_pos(fade, 0, L.scr_h - CHAT_FADE_H);
    lv_obj_set_size(fade, L.scr_w, CHAT_FADE_H);
    lv_obj_set_style_radius(fade, 0, 0);
    lv_obj_set_style_border_width(fade, 0, 0);
    lv_obj_set_style_pad_all(fade, 0, 0);
    lv_obj_set_style_bg_color(fade, COL_BG, 0);
    lv_obj_set_style_bg_grad_color(fade, COL_BG, 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);  // top: fully see-through
    lv_obj_set_style_bg_grad_opa(fade, LV_OPA_COVER, 0);   // bottom: panel black
    lv_obj_clear_flag(fade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(fade, LV_OBJ_FLAG_SCROLLABLE);

    // The shared pulse: LV_OPA_COVER ↔ LV_OPA_30, 700 ms each way, forever.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &pulse_val);
    lv_anim_set_exec_cb(&a, pulse_exec_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_playback_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

#endif  // BOARD_HAS_SESSION_VIEWS

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // Clip rather than wrap: a label this is genuinely too wide for its box
    // should lose its tail, not push a second line into the panel below.
    // Cheap insurance for "Sessions" (BOARD_HAS_SESSION_VIEWS), untested here since
    // sim doesn't build that feature.
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(lbl_title, L.scr_w - 2 * L.margin);
    lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);
#if BOARD_HAS_SESSION_VIEWS
    if (board_caps().has_session_views) build_session_views(usage_container);
#endif

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
}

// ---- Settings screen (tap the corner mascot from the usage screen) ----
// First pass: a single flex-column list rather than the hand-tuned per-tier
// pixel layout the rest of the file uses (see Layout struct) — simplest way
// to fit a growing, freely-orderable list of rows across every screen size
// without new per-tier constants for each one. Revisit if this needs to
// match the rest of the app's visual density more closely.

static lv_obj_t* make_settings_row(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    // Generous vertical padding — bigger tap target per row, easier to hit
    // with a finger (especially on CYD's imprecise resistive touch) than
    // the original tightly-packed rows.
    lv_obj_set_style_pad_ver(row, L.small_icons ? 6 : 8, 0);
    lv_obj_set_style_pad_hor(row, L.small_icons ? 1 : 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t* make_settings_label(lv_obj_t* parent, const char* text, lv_color_t col) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

// Small pill button — label + tap target. Value text set separately via the
// returned label so callbacks can update it in place.
static lv_obj_t* make_settings_button(lv_obj_t* parent, lv_event_cb_t cb, lv_obj_t** out_label) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, COL_BAR_BG, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    // Generous padding — bigger tap target, easier to hit with a finger
    // (especially on CYD's imprecise resistive touch).
    lv_obj_set_style_pad_hor(btn, L.small_icons ? 16 : 22, 0);
    lv_obj_set_style_pad_ver(btn, L.small_icons ? 6 : 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    *out_label = lv_label_create(btn);
    lv_obj_set_style_text_font(*out_label, L.bt_device_font, 0);
    lv_obj_set_style_text_color(*out_label, COL_TEXT, 0);
    lv_obj_center(*out_label);
    return btn;
}

static void fps_dec_cb(lv_event_t* e) {
    (void)e;
    uint8_t v = settings_get_fps();
    settings_set_fps(v <= SETTINGS_FPS_MIN ? SETTINGS_FPS_MIN : v - SETTINGS_FPS_STEP);
    lv_label_set_text_fmt(lbl_fps_value, "%u", settings_get_fps());
}

static void fps_inc_cb(lv_event_t* e) {
    (void)e;
    uint8_t v = settings_get_fps();
    settings_set_fps(v >= SETTINGS_FPS_MAX ? SETTINGS_FPS_MAX : v + SETTINGS_FPS_STEP);
    lv_label_set_text_fmt(lbl_fps_value, "%u", settings_get_fps());
}

static void transport_cycle_cb(lv_event_t* e) {
    (void)e;
    transport_pref_t t = (transport_pref_t)((settings_get_transport() + 1) % TRANSPORT_COUNT);
    settings_set_transport(t);
    lv_label_set_text(lbl_transport_value, settings_transport_name(t));
    // USB-only turns the BLE radio off outright (also silences the HID
    // keyboard shortcuts, which ride the same connection — accepted
    // tradeoff). Auto/BLE keep it on.
    ble_set_enabled(t != TRANSPORT_USB);
}

static void clockfmt_cycle_cb(lv_event_t* e) {
    (void)e;
    uint8_t new_fmt = (settings_get_clock_format() == 24) ? 12 : 24;
    settings_set_clock_format(new_fmt);
    clock_fmt = new_fmt;
    clock_last_epoch = -1;   // force the usage-screen title to redraw in the new format
    lv_label_set_text_fmt(lbl_clockfmt_value, "%uh", new_fmt);
}

static void settings_back_cb(lv_event_t* e) {
    (void)e;
    ui_show_screen(SCREEN_USAGE);
}

static void init_settings_screen(lv_obj_t* scr) {
    // "< Back" is a fixed footer, not part of the scrollable list below —
    // always reachable with one tap regardless of scroll position, rather
    // than something that could scroll out of view.
    const int back_h = L.small_icons ? 44 : 60;

    settings_container = lv_obj_create(scr);
    lv_obj_set_size(settings_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_container, 0, 0);
    lv_obj_set_style_bg_color(settings_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(settings_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_container, 0, 0);
    lv_obj_set_style_pad_all(settings_container, 0, 0);
    lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* settings_scroll = lv_obj_create(settings_container);
    lv_obj_set_size(settings_scroll, L.scr_w, L.scr_h - back_h);
    lv_obj_set_pos(settings_scroll, 0, 0);
    lv_obj_set_style_bg_opa(settings_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_scroll, 0, 0);
    lv_obj_set_style_pad_all(settings_scroll, L.margin, 0);
    // Matches lbl_title's own absolute position on the usage screen
    // (L.title_y, not the general-purpose L.margin) so the "Settings"
    // title starts at the exact same Y as "Usage"/"Sessions" — same fix
    // as settings_icon_img's position below, same root cause (a flex
    // child defaults to the container's general padding, not the more
    // specific constant the reference screen actually uses).
    lv_obj_set_style_pad_top(settings_scroll, L.title_y, 0);
    // Small gap — each row's own padding (see make_settings_row) already
    // provides the bigger tap target; stacking a large inter-row gap on
    // top of that made things feel too spread out.
    lv_obj_set_style_pad_row(settings_scroll, L.small_icons ? 0 : 2, 0);
    // Scrollable rather than hand-fit to every tier — the smallest screens
    // (240x240) can't fit the full list without it, and it costs nothing on
    // the larger tiers where everything already fits.
    lv_obj_set_scroll_dir(settings_scroll, LV_DIR_VER);
    lv_obj_set_flex_flow(settings_scroll, LV_FLEX_FLOW_COLUMN);

    // Fixed-pixel-size art rendered unscaled looks proportionally far
    // bigger on a small low-res panel than a large one (confirmed on real
    // CYD hardware — the large-only version overran this row) — same
    // small/large split every other icon in this file already uses.
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&settings_icon_dsc, CLAWD_SETTINGS_SMALL_W, CLAWD_SETTINGS_SMALL_H, clawd_settings_small_data);
    } else {
        init_icon_dsc_rgb565a8(&settings_icon_dsc, CLAWD_SETTINGS_W, CLAWD_SETTINGS_H, clawd_settings_data);
    }

    // Icon lives on `scr` (like logo_img), not inside settings_container's
    // flex flow — so it sits at a fixed corner position exactly like the
    // usage-screen mascot, and the title text below is centered across the
    // FULL width independent of the icon, matching how "Usage"/"Sessions"
    // (lbl_title) centers regardless of the corner mascot's presence. An
    // earlier version put icon+text in one flex row together, which made
    // the text start right after the icon instead of centering — visibly
    // inconsistent with every other screen's title treatment.
    settings_icon_img = lv_image_create(scr);
    lv_image_set_src(settings_icon_img, &settings_icon_dsc);
    if (L.small_icons) {
        // Same slot-centering formula as the corner mascot/logo_img below —
        // CLAWD_SETTINGS_SMALL_H matches CLAWD_STILL_SMALL_H exactly, so
        // this lines up with logo_img's own position pixel-for-pixel.
        const int top = L.logo_y + (LOGO_SMALL_HEIGHT - CLAWD_STILL_SMALL_H) / 2;
        lv_obj_set_pos(settings_icon_img, L.margin, top);
    } else {
        lv_obj_set_pos(settings_icon_img, L.margin, L.logo_y);
    }
    lv_obj_add_flag(settings_icon_img, LV_OBJ_FLAG_HIDDEN);
    // Tapping the icon while already on Settings backs out to Usage, same
    // as "< Back" — mirrors tapping the mascot on Usage to get here.
    // Extended hit area for the same reason as the usage-screen tap zone:
    // real-finger accuracy on CYD's resistive touch is poor against a
    // small target (see settings_tap_zone in ui_init()).
    lv_obj_add_flag(settings_icon_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(settings_icon_img, 20);
    lv_obj_add_event_cb(settings_icon_img, settings_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_title2 = lv_label_create(settings_scroll);
    lv_label_set_text(lbl_title2, "Settings");
    // Same font, width, and centering as "Usage"/"Sessions" (lbl_title in
    // init_usage_screen) — consistent title treatment across every screen.
    lv_obj_set_style_text_font(lbl_title2, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title2, COL_TEXT, 0);
    lv_label_set_long_mode(lbl_title2, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(lbl_title2, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_title2, LV_TEXT_ALIGN_CENTER, 0);

    // Large tier only: the icon (CLAWD_SETTINGS_H, deliberately much taller
    // than the title text) floats independently of the flex flow now (see
    // settings_icon_img above), so unlike the title itself — which is
    // expected to overlap it, same tradeoff as the usage screen's
    // mascot/clock overlap — the rows below need explicit reserved space
    // or the icon covers them.
    if (!L.small_icons) {
        lv_obj_t* icon_spacer = lv_obj_create(settings_scroll);
        lv_obj_set_size(icon_spacer, 1, CLAWD_SETTINGS_H - L.logo_y);
        lv_obj_set_style_bg_opa(icon_spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon_spacer, 0, 0);
        lv_obj_set_style_pad_all(icon_spacer, 0, 0);
        lv_obj_clear_flag(icon_spacer, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Session/Weekly already live on the main Usage screen — showing them
    // again here was pure duplication. Account identity instead: email/org
    // come from the daemon (may arrive a poll cycle after boot, hence the
    // "--" placeholder), device name/MAC are static device-side facts set
    // once below rather than refreshed with usage data.
    lbl_set_email   = make_settings_label(settings_scroll, "Email: --", COL_TEXT);
    lbl_set_org     = make_settings_label(settings_scroll, "Org: --", COL_TEXT);
    lbl_set_account = make_settings_label(settings_scroll, "Account: --", COL_DIM);
    lbl_set_device  = make_settings_label(settings_scroll, "Device: --", COL_DIM);
    lbl_set_mac     = make_settings_label(settings_scroll, "MAC: --", COL_DIM);
    // Long values (email addresses especially) can exceed even the large
    // tier's width — ellipsize rather than overflow past the screen edge.
    lv_obj_t* id_labels[] = {lbl_set_email, lbl_set_org, lbl_set_device, lbl_set_mac};
    for (lv_obj_t* l : id_labels) {
        lv_obj_set_width(l, LV_PCT(100));
        // DOTS mode only truncates (rather than word-wrapping) when the
        // object has a fixed one-line height — LV_SIZE_CONTENT lets it grow
        // to fit the wrapped text instead, which defeats the point.
        lv_obj_set_height(l, lv_font_get_line_height(L.bt_device_font));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text_fmt(lbl_set_device, "Device: %s", ble_get_device_name());
    lv_label_set_text_fmt(lbl_set_mac, "MAC: %s", ble_get_mac_address());

    lv_obj_t* sep = lv_label_create(settings_scroll);
    lv_label_set_text(sep, "");
    lv_obj_set_height(sep, 4);

    lv_obj_t* fps_row = make_settings_row(settings_scroll);
    make_settings_label(fps_row, "FPS", COL_TEXT);
    lv_obj_t* fps_ctrl = lv_obj_create(fps_row);
    lv_obj_set_size(fps_ctrl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(fps_ctrl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fps_ctrl, 0, 0);
    lv_obj_set_style_pad_all(fps_ctrl, 0, 0);
    lv_obj_clear_flag(fps_ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(fps_ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fps_ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* lbl_minus; lv_obj_t* btn_minus = make_settings_button(fps_ctrl, fps_dec_cb, &lbl_minus);
    lv_label_set_text(lbl_minus, "-");
    (void)btn_minus;
    lbl_fps_value = lv_label_create(fps_ctrl);
    lv_label_set_text_fmt(lbl_fps_value, "%u", settings_get_fps());
    lv_obj_set_style_text_font(lbl_fps_value, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_fps_value, COL_TEXT, 0);
    lv_obj_set_style_pad_hor(lbl_fps_value, 10, 0);
    lv_obj_t* lbl_plus; lv_obj_t* btn_plus = make_settings_button(fps_ctrl, fps_inc_cb, &lbl_plus);
    lv_label_set_text(lbl_plus, "+");
    (void)btn_plus;

    lv_obj_t* transport_row = make_settings_row(settings_scroll);
    make_settings_label(transport_row, "Transport", COL_TEXT);
    make_settings_button(transport_row, transport_cycle_cb, &lbl_transport_value);
    lv_label_set_text(lbl_transport_value, settings_transport_name(settings_get_transport()));

    lv_obj_t* clock_row = make_settings_row(settings_scroll);
    make_settings_label(clock_row, "Clock", COL_TEXT);
    make_settings_button(clock_row, clockfmt_cycle_cb, &lbl_clockfmt_value);
    lv_label_set_text_fmt(lbl_clockfmt_value, "%uh", settings_get_clock_format());

    // Fixed footer, sibling of settings_scroll (not inside it) — always
    // on-screen at a constant position regardless of scroll offset, per
    // the user's request that Back never scroll out of reach.
    lv_obj_t* back_row = lv_button_create(settings_container);
    lv_obj_set_size(back_row, L.scr_w, back_h);
    lv_obj_set_pos(back_row, 0, L.scr_h - back_h);
    lv_obj_set_style_radius(back_row, 0, 0);
    lv_obj_set_style_bg_color(back_row, COL_BAR_BG, 0);
    lv_obj_add_event_cb(back_row, settings_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl_back = lv_label_create(back_row);
    lv_label_set_text(lbl_back, "< Back");
    lv_obj_set_style_text_font(lbl_back, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_back, COL_TEXT, 0);
    lv_obj_center(lbl_back);
}

// Keeps the Settings screen's usage summary current even while it isn't the
// visible screen — cheap (a handful of label sets), same tradeoff the rest
// of this file already makes for hidden panels.
static void refresh_settings_usage_labels(void) {
    const UsageData* d = &s_settings_usage;
    if (!lbl_set_email) return;
    // Older daemons (pre account-info support) never send "em"/"og" — leave
    // the "--" placeholder rather than blanking the row.
    if (d->email[0]) lv_label_set_text_fmt(lbl_set_email, "Email: %s", d->email);
    if (d->org[0])   lv_label_set_text_fmt(lbl_set_org, "Org: %s", d->org);
    lv_label_set_text(lbl_set_account, d->enterprise ? "Account: Enterprise" : "Account: Pro");
}

static void open_settings_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_USAGE) ui_show_screen(SCREEN_SETTINGS);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Static corner mascot (see clawd_still.h) — the animated one needs PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#if BOARD_HAS_SESSION_VIEWS
    // Only a small-tier asset exists today (see clawd_sessions.h) — the
    // only board hitting this branch with session views on is CYD.
    init_icon_dsc_rgb565a8(&sessions_icon_dsc, CLAWD_SESSIONS_SMALL_W, CLAWD_SESSIONS_SMALL_H, clawd_sessions_small_data);
#endif
#endif
    init_battery_icons();
    init_conn_icons();

    init_usage_screen(scr);
    init_settings_screen(scr);
    splash_init(scr);

    clock_fmt = settings_get_clock_format();

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    // Tapping it (rather than the screen at large) opens Settings instead of
    // toggling the splash screen — see open_settings_cb().
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    // Invisible tap target, well bigger than the visible mascot art itself
    // (a real tap on a small icon isn't reliable — confirmed on CYD's
    // resistive touch, where per-pixel accuracy is poor right after a fresh
    // flash since its self-widening calibration hasn't stretched yet; see
    // touch.cpp). Covers the top-left corner generously rather than being
    // sized to the artwork, and works identically for both mascot variants
    // since it doesn't depend on either one's own bounds.
    settings_tap_zone = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(settings_tap_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_tap_zone, 0, 0);
    lv_obj_set_style_pad_all(settings_tap_zone, 0, 0);
    lv_obj_clear_flag(settings_tap_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_tap_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(settings_tap_zone, 0, 0);
    lv_obj_set_size(settings_tap_zone, L.scr_w * 2 / 5, L.scr_h * 7 / 20);
    lv_obj_add_event_cb(settings_tap_zone, open_settings_cb, LV_EVENT_CLICKED, NULL);

    // Transport icons sit left of the battery icon, hugging the corner:
    // [serial][bluetooth][battery]. Uses conn_row_margin (tighter than the
    // L.margin the rest of the screen uses) so the cluster stays compact and
    // leaves the title/clock as much centered room as possible. Always the
    // small 24px size (see init_conn_icons()) regardless of L.batt_w, so
    // vertically center them against the battery icon's row rather than
    // sharing its top edge.
    const int16_t battery_x = L.scr_w - L.batt_w - L.conn_row_margin;
    const int16_t bt_icon_x = battery_x - L.conn_icon_gap - ICON_BLUETOOTH_SMALL_W;
    const int16_t serial_icon_x = bt_icon_x - L.conn_icon_gap - ICON_SERIAL_SMALL_W;
    const int16_t conn_icon_y = L.batt_y + (L.batt_w - ICON_BLUETOOTH_SMALL_H) / 2;

    bt_icon_img = lv_image_create(scr);
    lv_image_set_src(bt_icon_img, &bt_icon_dsc);
    lv_obj_set_pos(bt_icon_img, bt_icon_x, conn_icon_y);
    lv_obj_set_style_image_recolor_opa(bt_icon_img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(bt_icon_img, COL_DIM, 0);

    serial_icon_img = lv_image_create(scr);
    lv_image_set_src(serial_icon_img, &serial_icon_dsc);
    lv_obj_set_pos(serial_icon_img, serial_icon_x, conn_icon_y);
    lv_obj_set_style_image_recolor_opa(serial_icon_img, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(serial_icon_img, COL_DIM, 0);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, battery_x, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;
    s_settings_usage = *data;
    refresh_settings_usage_labels();

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        // clock_fmt is device-owned (Settings screen) — data->clock_fmt (the
        // daemon's "tf" field) is deliberately not consulted here.
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_epoch = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }

#if BOARD_HAS_SESSION_VIEWS
    // The chat views carry their own mini quota widgets (§1.3/§1.4) — keep
    // them in step with the panels above.
    if (board_caps().has_session_views) {
        s_usage_cache = *data;
        session_quota_refresh();
    }
#endif
}

// The `✻` status line doubles as the connection-state readout (pairing/
// idle/RESTING screens: "Waiting"/"Connected"/whimsical — see
// ui_tick_anim()) and, on the session views, a "something's happening"
// flourish. Those are two different jobs: on views 0/1/2 it must always be
// visible so BLE state stays legible, regardless of session data — only on
// the session views themselves does it yield when it has nothing to say
// (every chat closed or idle, or a chat's own pulsing indicator already has
// your attention).
//
// Keyed off s_any_active/s_focus_active (any row / the one row not idle)
// rather than only s_any_waiting/s_focus_waiting, because "is anyone doing
// anything" is exactly the question this line answers here — showing it
// over an idle chat (or an all-idle list) read as false liveliness. A
// near-identical bucket-driven version was blamed for a "whole screen
// flickering" report once before and reverted to waiting-only; root cause
// was never conclusively pinned down (several other things changed the same
// session), and the daemon's own state machine holds RUNNING_TOOL steady
// between a turn's tool calls rather than dipping through IDLE, so it
// shouldn't actually flap. Watch for a recurrence before assuming this is
// settled.
static void apply_anim_visibility(void) {
    if (!lbl_anim) return;
    bool hide = false;
#if BOARD_HAS_SESSION_VIEWS
    if (view_state == 4) hide = (s_live_count == 0) || s_any_waiting || !s_any_active;
    else if (view_state == 3) hide = (s_live_count == 0) || s_focus_waiting || !s_focus_active;
#endif
    if (hide) lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
}

// The view resolver (§2.1) — one function, run every tick; nothing else
// chooses a view. Picks the usage-screen sub-view: pairing hint (BLE down),
// the idle "Zzz" screen (connected but data stale), the live quota panels
// (RESTING), or — on boards with session views — ONE-CHAT / SEVERAL-CHATS.
// Only re-lays-out on an actual change.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    const uint32_t now = lv_tick_get();
    // data_ok folded into freshness (not just data_received) so the daemon's
    // {"ok":false} no-data beat still routes to idle instead of showing a
    // stale RESTING/chat view — see the BLE-vs-serial view-state fix on main.
    const bool fresh = data_received && data_ok && (now - last_data_ms) < DATA_FRESH_MS;
    int v;
#if BOARD_HAS_SESSION_VIEWS
    if (board_caps().has_session_views && s_any_waiting) {
        // A waiting chat pins the view (§2.1): it bypasses the freshness
        // check and the linger timer. A session that has sat on a permission
        // prompt for forty minutes is precisely the case this feature exists
        // for, and a plain inactivity timeout would hide it.
        v = (s_live_count <= 1) ? 3 : 4;
    } else
#endif
    if (fresh) {
        // Live data — from BLE or USB serial — takes priority over the
        // !s_ble_connected check below: serial doesn't set s_ble_connected,
        // so gating on it here would strand a serial-only setup on the
        // pairing hint forever even with fresh data in hand.
#if BOARD_HAS_SESSION_VIEWS
        if (board_caps().has_session_views && s_live_count == 1) {
            v = 3;  // ONE CHAT (§1.3)
        } else if (board_caps().has_session_views && s_live_count >= 2) {
            v = 4;  // SEVERAL CHATS (§1.4)
        } else if (board_caps().has_session_views && s_chats_linger &&
                   (now - s_chats_gone_ms) < CHAT_LINGER_MS) {
            v = s_linger_view;  // hold the chat view after the last chat closed (§2.1)
        } else
#endif
        {
#if BOARD_HAS_SESSION_VIEWS
            s_chats_linger = false;  // linger expired (or never armed)
#endif
            v = 2;  // RESTING — live quota panels
        }
    } else if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
#if BOARD_HAS_SESSION_VIEWS
    Serial.printf("view_state: %d -> %d (live=%d waiting=%d fresh=%d)\n",
                  view_state, v, s_live_count, s_any_waiting, fresh);
#else
    Serial.printf("view_state: %d -> %d (fresh=%d)\n", view_state, v, fresh);
#endif
    view_state = v;
    // Instant swap. §2.3's 280 ms RESTING↔chat cross-fade is deliberately
    // deferred — the existing sub-view pattern is instant, and the fade is
    // cosmetic-only.
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
#if BOARD_HAS_SESSION_VIEWS
    if (focus_group) lv_obj_add_flag(focus_group, LV_OBJ_FLAG_HIDDEN);
    if (chats_group) lv_obj_add_flag(chats_group, LV_OBJ_FLAG_HIDDEN);
    if (v == 3 || v == 4) {
        lv_label_set_text(lbl_title, "Sessions");
#ifndef BOARD_HAS_PSRAM
        if (logo_img) lv_image_set_src(logo_img, &sessions_icon_dsc);
#endif
        if (v == 3) lv_obj_clear_flag(focus_group, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_clear_flag(chats_group, LV_OBJ_FLAG_HIDDEN);
            // Freshly entering the list (not just re-laying it out while
            // already on it — that path doesn't reach here, see the early
            // return above): start scrolled to the top, where the
            // attention-first sort put whatever's most urgent. Otherwise a
            // scroll position left over from a previous visit could open
            // straight onto whatever happened to be there before.
            if (cards_cont) lv_obj_scroll_to_y(cards_cont, 0, LV_ANIM_OFF);
        }
    } else {
        // Leaving the session view: restore the title now instead of
        // waiting up to a second for the next clock tick to overwrite it.
        lv_label_set_text(lbl_title, "Usage");
#ifndef BOARD_HAS_PSRAM
        if (logo_img) lv_image_set_src(logo_img, &logo_dsc);
#endif
        clock_last_epoch = -1;
        lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                          LV_OBJ_FLAG_HIDDEN);
    }
#else
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
#endif
#if BOARD_HAS_SESSION_VIEWS
    Serial.printf("  after swap: usage_hidden=%d focus_hidden=%d chats_hidden=%d\n",
                  lv_obj_has_flag(usage_group, LV_OBJ_FLAG_HIDDEN),
                  focus_group ? lv_obj_has_flag(focus_group, LV_OBJ_FLAG_HIDDEN) : -1,
                  chats_group ? lv_obj_has_flag(chats_group, LV_OBJ_FLAG_HIDDEN) : -1);
#else
    Serial.printf("  after swap: usage_hidden=%d\n",
                  lv_obj_has_flag(usage_group, LV_OBJ_FLAG_HIDDEN));
#endif
    apply_anim_visibility();
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    // Only BLE connect/disconnect and a serial payload landing are otherwise
    // event-driven — serial going stale (green -> red) needs a poll.
    update_connection_icons();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every second between payloads.
    // Suppressed while a session view is up (title reads "Sessions" there instead
    // — see the view-swap block in update_view_state()).
#if BOARD_HAS_SESSION_VIEWS
    if (clock_base_epoch > 0 && view_state != 3 && view_state != 4) {
#else
    if (clock_base_epoch > 0) {
#endif
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        if (cur != clock_last_epoch) {   // only rewrite the title when the second changes
            clock_last_epoch = cur;
            struct tm tmv;
            gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
            char tbuf[16];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d:%02d %s", h12, tmv.tm_min, tmv.tm_sec,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (view_state == 2 && !s_ble_connected) {
        text = (now - last_data_ms < 5000)
            ? "Updated"
            : anim_messages[anim_msg_idx];
    } else if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    const bool hide = (current_screen == SCREEN_SPLASH);
    if (battery_img) {
        if (hide) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    }
    // Transport icons share the battery icon's row, so they share its
    // splash-only hidden state too — there's no separate "hasn't got one"
    // case to gate on the way battery_img's existence does.
    if (hide) {
        lv_obj_add_flag(bt_icon_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(serial_icon_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(bt_icon_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(serial_icon_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_icon_img, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:    splash_show(); break;
    case SCREEN_USAGE:     lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SETTINGS:
        lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(settings_icon_img, LV_OBJ_FLAG_HIDDEN);
        break;
    default: break;
    }

    // Also hidden on Settings — its own title sits in the same top-left
    // corner the mascot occupies, and it's the click target that got you
    // here in the first place, not something Settings itself needs shown.
    const bool show_mascot = (screen != SCREEN_SPLASH && screen != SCREEN_SETTINGS);
    splash_mascot_set_visible(show_mascot);
    if (logo_img) {
        if (show_mascot) lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    // Not just cosmetic: leaving this clickable while hidden-by-screen would
    // silently swallow taps meant for splash_root's own click handler (LVGL
    // stops at the first clickable hit, hidden or not doesn't matter unless
    // HIDDEN is set — see lv_indev_search_obj()).
    if (show_mascot) lv_obj_clear_flag(settings_tap_zone, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(settings_tap_zone, LV_OBJ_FLAG_HIDDEN);

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

// Recolors the two transport icons: green while live, gray if that transport
// has never been used, red if it was used before but has since gone quiet.
// Cheap to call every tick — only writes a style when a color actually
// changes (lv_color_eq(), not the raw struct, since LVGL 9's lv_color_t
// isn't guaranteed a single comparable field across color depths).
static void update_connection_icons(void) {
    if (!bt_icon_img || !serial_icon_img) return;

    lv_color_t bt_col = !s_ble_ever_connected ? COL_DIM
                       : s_ble_connected       ? COL_GREEN
                                               : COL_RED;
    bool serial_fresh = s_serial_ever_active &&
                         (lv_tick_get() - s_last_serial_ms < DATA_FRESH_MS);
    lv_color_t serial_col = !s_serial_ever_active ? COL_DIM
                           : serial_fresh          ? COL_GREEN
                                                   : COL_RED;

    if (!s_icon_col_init || !lv_color_eq(bt_col, s_bt_icon_col)) {
        lv_obj_set_style_image_recolor(bt_icon_img, bt_col, 0);
        s_bt_icon_col = bt_col;
    }
    if (!s_icon_col_init || !lv_color_eq(serial_col, s_serial_icon_col)) {
        lv_obj_set_style_image_recolor(serial_icon_img, serial_col, 0);
        s_serial_icon_col = serial_col;
    }
    s_icon_col_init = true;
}

// Called from main.cpp whenever check_serial_cmd() successfully applies a
// usage JSON payload received over USB serial — the only signal this
// connectionless transport has that a host is actually out there.
void ui_note_serial_activity(void) {
    s_serial_ever_active = true;
    s_last_serial_ms = lv_tick_get();
    update_connection_icons();
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);
    if (s_ble_connected) s_ble_ever_connected = true;

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    update_connection_icons();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

#if BOARD_HAS_SESSION_VIEWS
void ui_update_sessions(const SessionList* list) {
    if (!list || !focus_group || !board_caps().has_session_views) return;

    const uint8_t prev_count = s_live_count;
    s_live_count = list->count;
    s_any_waiting = false;
    s_any_active = false;
    for (int i = 0; i < list->count; i++) {
        const int b = session_bucket(list->rows[i].state);
        if (b == SESSION_BUCKET_WAITING) s_any_waiting = true;
        if (b != SESSION_BUCKET_IDLE)    s_any_active = true;
    }

    if (list->count == 0) {
        if (prev_count > 0 && (view_state == 3 || view_state == 4)) {
            // The last live chat disappeared → hold the current view for
            // CHAT_LINGER_MS (§2.1). Cards keep their final content, but the
            // waiting treatment is dropped: a chat that ended can't need you,
            // and the pulse must keep meaning "come here".
            s_chats_linger = true;
            s_chats_gone_ms = lv_tick_get();
            s_linger_view = view_state;
            s_focus_waiting = false;
            s_focus_active = false;
            if (focus_card.dot) {
                lv_obj_set_style_bg_opa(focus_card.dot, LV_OPA_COVER, 0);
                lv_obj_set_style_text_opa(focus_card.lbl_state, LV_OPA_COVER, 0);
            }
            for (auto& c : chat_cards) {
                c.waiting = false;
                if (c.used) {
                    lv_obj_set_style_bg_opa(c.dot, LV_OPA_COVER, 0);
                    lv_obj_set_style_text_opa(c.lbl_state, LV_OPA_COVER, 0);
                }
            }
        }
        update_view_state();
        apply_anim_visibility();
        return;
    }

    s_chats_linger = false;
    focus_set_content(&list->rows[0]);
    chats_set_content(list);
    update_view_state();
    apply_anim_visibility();
}
#else
// Boards without session views compile to today's behavior; the call sites
// in main.cpp are gated too, so this stub only keeps the public API total.
void ui_update_sessions(const SessionList* list) { (void)list; }
#endif

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
