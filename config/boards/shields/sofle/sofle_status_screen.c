/*
 * Custom OLED status screen for the Sofle central (left) half, in portrait.
 *
 * The 128x32 panel is read rotated: its physical left edge is the bottom,
 * its physical right edge is the top. Content is drawn on an off-screen
 * 32x128 portrait canvas and copied rotated onto the visible 128x32 canvas.
 *
 *   +----+
 *   |BT2 |   output: USB / BT profile
 *   | ok |   BT state: ok / -- / new
 *   |----|
 *   |BASE|   active layer
 *   |----|
 *   | L+ |   left battery ("+" while USB powered)
 *   |85% |
 *   | R  |   right (peripheral) battery
 *   |72% |
 *   +----+
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
#include <zmk/battery.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

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

/*
 * Same logical colours as the built-in mono theme (light background); the
 * panel's inversion-on setting makes the text the lit pixels.
 */
#define COLOR_BG lv_color_white()
#define COLOR_FG lv_color_black()

struct sofle_status_state {
    struct zmk_endpoint_instance endpoint;
    bool ble_connected;
    bool ble_bonded;
    zmk_keymap_layer_index_t layer_index;
    const char *layer_name;
    uint8_t battery_left;
    int16_t battery_right; /* -1 = unknown */
    bool usb_powered;
};

static lv_color_t portrait_buf[PORT_W * PORT_H];
static lv_color_t panel_buf[PANEL_W * PANEL_H];
static lv_obj_t *portrait_canvas;
static lv_obj_t *panel_canvas;

/* Only touched from event context (state_func), so no extra locking needed. */
static int16_t peripheral_battery = -1;

static struct sofle_status_state sofle_status_get_state(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    if (eh != NULL) {
        const struct zmk_peripheral_battery_state_changed *pev =
            as_zmk_peripheral_battery_state_changed(eh);
        if (pev != NULL && pev->source == 0) {
            peripheral_battery = pev->state_of_charge;
        }
    }
#endif

    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();

    return (struct sofle_status_state){
        .endpoint = zmk_endpoints_selected(),
#if IS_ENABLED(CONFIG_ZMK_BLE)
        .ble_connected = zmk_ble_active_profile_is_connected(),
        .ble_bonded = !zmk_ble_active_profile_is_open(),
#endif
        .layer_index = index,
        .layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
        .battery_left = zmk_battery_state_of_charge(),
        .battery_right = peripheral_battery,
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_powered = zmk_usb_is_powered(),
#endif
    };
}

static void draw_line_text(lv_draw_label_dsc_t *dsc, lv_coord_t y, const char *text) {
    lv_canvas_draw_text(portrait_canvas, 0, y, PORT_W, dsc, text);
}

static void draw_separator(lv_coord_t y) {
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = COLOR_FG;
    lv_canvas_draw_rect(portrait_canvas, 2, y, PORT_W - 4, 1, &rect);
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

static void sofle_status_update_cb(struct sofle_status_state state) {
    char text[12];
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = COLOR_FG;
    dsc.font = &lv_font_montserrat_10;
    dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_canvas_fill_bg(portrait_canvas, COLOR_BG, LV_OPA_COVER);

    /* Output */
    if (state.endpoint.transport == ZMK_TRANSPORT_USB) {
        draw_line_text(&dsc, 2, "USB");
    } else {
        snprintf(text, sizeof(text), "BT%d", state.endpoint.ble.profile_index + 1);
        draw_line_text(&dsc, 2, text);
        draw_line_text(&dsc, 14,
                       !state.ble_bonded ? "new" : (state.ble_connected ? "ok" : "--"));
    }

    draw_separator(30);

    /* Layer */
    if (state.layer_name != NULL && strlen(state.layer_name) > 0) {
        snprintf(text, sizeof(text), "%s", state.layer_name);
    } else {
        snprintf(text, sizeof(text), "L%d", state.layer_index);
    }
    draw_line_text(&dsc, 36, text);

    draw_separator(54);

    /* Batteries */
    draw_line_text(&dsc, 60, state.usb_powered ? "L+" : "L");
    snprintf(text, sizeof(text), "%u%%", state.battery_left);
    draw_line_text(&dsc, 72, text);

    draw_line_text(&dsc, 92, "R");
    if (state.battery_right < 0) {
        draw_line_text(&dsc, 104, "--");
    } else {
        snprintf(text, sizeof(text), "%d%%", state.battery_right);
        draw_line_text(&dsc, 104, text);
    }

    copy_rotated();
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_status, struct sofle_status_state, sofle_status_update_cb,
                            sofle_status_get_state)

ZMK_SUBSCRIPTION(sofle_status, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(sofle_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(sofle_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(sofle_status, zmk_peripheral_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(sofle_status, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(sofle_status, zmk_ble_active_profile_changed);
#endif

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
