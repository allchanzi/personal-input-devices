/*
 * Custom OLED status screen for the Sofle central (left) half, in portrait.
 *
 * The 128x32 panel is read rotated: its physical left edge is the bottom,
 * its physical right edge is the top. Content is drawn on an off-screen
 * 32x128 portrait canvas and copied rotated onto the visible 128x32 canvas.
 *
 *   +-----+
 *   |@o.oo|  BT profiles (or "USB"): filled = connected, ring = paired,
 *   |-    |  dot = empty; bar under the active profile
 *   |-----|
 *   | BASE|  active layer
 *   |S C  |  held modifiers: Shift Ctrl Alt Gui
 *   |-----|
 *   |[=][=]|  left and right battery side by side, upright: 3 bars
 *   |[=][ ]|  (75/50/25 %), a bolt while that half is USB powered,
 *   |[=][=]|  "x" when the right half is disconnected
 *   |-----|
 *   |CAPS |  host lock lights, inverted while on: CAPS, NUM, SCRL
 *   +-----+
 *
 * The right half has no display; it reports its USB power over sofle_sync.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/battery.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <dt-bindings/zmk/modifiers.h>

#include "sofle_sync.h"

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

/* Physical panel size (landscape). */
#define PANEL_W 128
#define PANEL_H 32
/* Portrait drawing size. */
#define PORT_W PANEL_H
#define PORT_H PANEL_W

/* Up to 5 profile dots fit in 32 px. */
#if defined(ZMK_BLE_PROFILE_COUNT) && ZMK_BLE_PROFILE_COUNT < 5
#define PROFILE_COUNT ZMK_BLE_PROFILE_COUNT
#else
#define PROFILE_COUNT 5
#endif

/*
 * Same logical colours as the built-in mono theme (light background); the
 * panel's inversion-on setting makes the foreground the lit pixels.
 */
#define COLOR_BG lv_color_white()
#define COLOR_FG lv_color_black()

enum profile_state { PROFILE_EMPTY, PROFILE_PAIRED, PROFILE_CONNECTED };

struct sofle_status_state {
    bool usb_output;
    uint8_t active_profile;
    enum profile_state profiles[PROFILE_COUNT];
    zmk_keymap_layer_index_t layer_index;
    const char *layer_name;
    zmk_mod_flags_t mods;
    uint8_t battery_left;
    int16_t battery_right; /* -1 = unknown, 0 = disconnected */
    bool usb_powered;
    bool right_usb_powered;
    zmk_hid_indicators_t indicators;
};

static lv_color_t portrait_buf[PORT_W * PORT_H];
static lv_color_t panel_buf[PANEL_W * PANEL_H];
static lv_obj_t *portrait_canvas;
static lv_obj_t *panel_canvas;

/* Only touched from event context (state_func), so no extra locking needed. */
static int16_t peripheral_battery = -1;

static struct sofle_status_state sofle_status_get_state(const zmk_event_t *eh) {
    if (eh != NULL) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
        const struct zmk_peripheral_battery_state_changed *pev =
            as_zmk_peripheral_battery_state_changed(eh);
        if (pev != NULL && pev->source == 0) {
            /* ZMK reports level 0 when the peripheral disconnects. */
            peripheral_battery = pev->state_of_charge;
        }
#endif
    }

    struct sofle_status_state state = {0};
    state.usb_output = zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
    state.mods = zmk_hid_get_explicit_mods();
    state.battery_left = zmk_battery_state_of_charge();
    state.battery_right = peripheral_battery;
    state.right_usb_powered = peripheral_battery > 0 && sofle_sync_peripheral_usb_powered();
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    state.indicators = zmk_hid_indicators_get_current_profile();
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    state.usb_powered = zmk_usb_is_powered();
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    state.active_profile = zmk_ble_active_profile_index();
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (zmk_ble_profile_is_connected(i)) {
            state.profiles[i] = PROFILE_CONNECTED;
        } else if (!zmk_ble_profile_is_open(i)) {
            state.profiles[i] = PROFILE_PAIRED;
        } else {
            state.profiles[i] = PROFILE_EMPTY;
        }
    }
#endif

    state.layer_index = zmk_keymap_highest_layer_active();
    state.layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(state.layer_index));

    return state;
}

static void draw_text(lv_draw_label_dsc_t *dsc, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                      lv_text_align_t align, const char *text) {
    dsc->align = align;
    lv_canvas_draw_text(portrait_canvas, x, y, w, dsc, text);
}

static void draw_box(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, bool filled,
                     lv_coord_t radius) {
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.radius = radius;
    rect.bg_color = filled ? COLOR_FG : COLOR_BG;
    rect.border_color = COLOR_FG;
    rect.border_width = filled ? 0 : 1;
    lv_canvas_draw_rect(portrait_canvas, x, y, w, h, &rect);
}

static void draw_separator(lv_coord_t y) { draw_box(2, y, PORT_W - 4, 1, true, 0); }

static void draw_profiles(lv_draw_label_dsc_t *dsc, const struct sofle_status_state *state) {
    if (state->usb_output) {
        draw_text(dsc, 0, 0, PORT_W, LV_TEXT_ALIGN_CENTER, "USB");
        return;
    }

    /* 5 dots of 5 px with 1 px gaps = 29 px, centred in 32 px. */
    for (int i = 0; i < PROFILE_COUNT; i++) {
        lv_coord_t x = 1 + i * 6;
        switch (state->profiles[i]) {
        case PROFILE_CONNECTED:
            draw_box(x, 1, 5, 5, true, LV_RADIUS_CIRCLE);
            break;
        case PROFILE_PAIRED:
            draw_box(x, 1, 5, 5, false, LV_RADIUS_CIRCLE);
            break;
        default:
            draw_box(x + 2, 3, 1, 1, true, 0);
            break;
        }
        if (i == state->active_profile) {
            draw_box(x, 8, 5, 1, true, 0);
        }
    }
}

static void draw_mods(lv_draw_label_dsc_t *dsc, lv_coord_t y, zmk_mod_flags_t mods) {
    static const struct {
        zmk_mod_flags_t mask;
        const char *label;
    } items[] = {
        {MOD_LSFT | MOD_RSFT, "S"},
        {MOD_LCTL | MOD_RCTL, "C"},
        {MOD_LALT | MOD_RALT, "A"},
        {MOD_LGUI | MOD_RGUI, "G"},
    };

    for (int i = 0; i < ARRAY_SIZE(items); i++) {
        if (mods & items[i].mask) {
            draw_text(dsc, i * 8, y, 8, LV_TEXT_ALIGN_CENTER, items[i].label);
        }
    }
}

/* Battery icon size: upright 12x22 body with a 4x2 nub on top. */
#define BATT_W 12
#define BATT_H 24

/* 3 bars: 75 %+ = 3, 50 %+ = 2, 25 %+ = 1, below 25 % = empty. */
static int battery_bars(int level) {
    if (level >= 75) {
        return 3;
    }
    if (level >= 50) {
        return 2;
    }
    if (level >= 25) {
        return 1;
    }
    return 0;
}

/*
 * Upright battery at (x, y): bars for the level, a bolt while USB powered,
 * "x" when disconnected, "-" while not known yet.
 */
static void draw_battery(lv_draw_label_dsc_t *dsc, lv_coord_t x, lv_coord_t y, int level,
                         bool charging) {
    draw_box(x + 4, y, 4, 2, true, 0);
    draw_box(x, y + 2, BATT_W, BATT_H - 2, false, 0);

    if (level <= 0) {
        draw_text(dsc, x, y + 7, BATT_W, LV_TEXT_ALIGN_CENTER, level == 0 ? "x" : "-");
        return;
    }
    if (charging) {
        draw_text(dsc, x, y + 7, BATT_W, LV_TEXT_ALIGN_CENTER, LV_SYMBOL_CHARGE);
        return;
    }

    /* Bars 8x4 with 2 px gaps, filled from the bottom. */
    int bars = battery_bars(level);
    for (int i = 0; i < bars; i++) {
        draw_box(x + 2, y + 17 - i * 6, BATT_W - 4, 4, true, 0);
    }
}

/* Bits of the HID keyboard LED report sent by the host. */
#define HID_LED_NUM_LOCK BIT(0)
#define HID_LED_CAPS_LOCK BIT(1)
#define HID_LED_SCROLL_LOCK BIT(2)

/* One row per lit lock, drawn inverted (light box, dark text) so it stands out. */
static void draw_indicators(lv_draw_label_dsc_t *dsc, lv_coord_t y,
                            zmk_hid_indicators_t indicators) {
    static const struct {
        zmk_hid_indicators_t mask;
        const char *label;
    } items[] = {
        {HID_LED_CAPS_LOCK, "CAPS"},
        {HID_LED_NUM_LOCK, "NUM"},
        {HID_LED_SCROLL_LOCK, "SCRL"},
    };

    lv_color_t fg = dsc->color;
    for (int i = 0; i < ARRAY_SIZE(items); i++) {
        if (!(indicators & items[i].mask)) {
            continue;
        }
        draw_box(2, y, PORT_W - 4, 12, true, 2);
        dsc->color = COLOR_BG;
        draw_text(dsc, 2, y, PORT_W - 4, LV_TEXT_ALIGN_CENTER, items[i].label);
        dsc->color = fg;
        y += 14;
    }
}

/*
 * Portrait pixel (px, py) -> panel pixel (x, y):
 * portrait top (py = 0) is the panel's right edge, portrait left (px = 0)
 * is the panel's top edge.
 */
static void copy_rotated(void) {
    for (int py = 0; py < PORT_H; py++) {
        for (int px = 0; px < PORT_W; px++) {
            int x = PANEL_W - 1 - py;
            int y = px;
            panel_buf[y * PANEL_W + x] = portrait_buf[py * PORT_W + px];
        }
    }
    lv_obj_invalidate(panel_canvas);
}

static bool state_equal(const struct sofle_status_state *a, const struct sofle_status_state *b) {
    if (a->usb_output != b->usb_output || a->active_profile != b->active_profile ||
        a->layer_index != b->layer_index || a->layer_name != b->layer_name ||
        a->mods != b->mods || a->battery_left != b->battery_left ||
        a->battery_right != b->battery_right || a->usb_powered != b->usb_powered ||
        a->right_usb_powered != b->right_usb_powered || a->indicators != b->indicators) {
        return false;
    }
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (a->profiles[i] != b->profiles[i]) {
            return false;
        }
    }
    return true;
}

static void sofle_status_update_cb(struct sofle_status_state state) {
    /* Most key presses change nothing on screen: skip the redraw entirely. */
    static struct sofle_status_state last;
    static bool drawn;
    if (drawn && state_equal(&state, &last)) {
        return;
    }
    last = state;
    drawn = true;

    char text[16];
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = COLOR_FG;
    dsc.font = &lv_font_montserrat_10;

    lv_canvas_fill_bg(portrait_canvas, COLOR_BG, LV_OPA_COVER);

    draw_profiles(&dsc, &state);
    draw_separator(12);

    if (state.layer_name != NULL && strlen(state.layer_name) > 0) {
        snprintf(text, sizeof(text), "%s", state.layer_name);
    } else {
        snprintf(text, sizeof(text), "L%d", state.layer_index);
    }
    draw_text(&dsc, 0, 15, PORT_W, LV_TEXT_ALIGN_CENTER, text);
    draw_mods(&dsc, 27, state.mods);
    draw_separator(41);

    /* Left and right battery side by side: 2 + 12 + 4 + 12 + 2 = 32 px. */
    draw_battery(&dsc, 2, 46, state.battery_left, state.usb_powered);
    draw_battery(&dsc, 18, 46, state.battery_right, state.right_usb_powered);
    draw_separator(74);

    draw_indicators(&dsc, 77, state.indicators);

    copy_rotated();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_status, struct sofle_status_state, sofle_status_update_cb,
                            sofle_status_get_state)

ZMK_SUBSCRIPTION(sofle_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(sofle_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(sofle_status, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(sofle_status, zmk_keycode_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(sofle_status, zmk_peripheral_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(sofle_status, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(sofle_status, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(sofle_status, zmk_hid_indicators_changed);
#endif

/* The right half's USB power changed (called from BT context): refresh like an event would. */
static void remote_changed_work_cb(struct k_work *work) {
    if (zmk_display_is_initialized()) {
        sofle_status_refresh_state(NULL);
        k_work_submit_to_queue(zmk_display_work_q(), &sofle_status_work);
    }
}

K_WORK_DEFINE(remote_changed_work, remote_changed_work_cb);

void sofle_status_remote_changed(void) { k_work_submit(&remote_changed_work); }

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Off-screen portrait canvas: only used as a drawing target. */
    portrait_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(portrait_canvas, portrait_buf, PORT_W, PORT_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(portrait_canvas, LV_OBJ_FLAG_HIDDEN);

    /* Visible landscape canvas covering the whole panel. */
    panel_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(panel_canvas, panel_buf, PANEL_W, PANEL_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(panel_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_fill_bg(panel_canvas, COLOR_BG, LV_OPA_COVER);

    sofle_status_init();
    return screen;
}
