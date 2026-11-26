/*
 * Custom 128x32 OLED status screen for the Sofle central (left) half.
 *
 *   BT2 ok            FN
 *   L +85%        R 72%
 *
 * Line 1: active output (USB / BT profile + state), active layer name.
 * Line 2: left battery ("+" while USB powered), right (peripheral) battery.
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

static lv_obj_t *output_label;
static lv_obj_t *layer_label;
static lv_obj_t *battery_left_label;
static lv_obj_t *battery_right_label;

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

static void sofle_status_update_cb(struct sofle_status_state state) {
    char text[20];

    if (state.endpoint.transport == ZMK_TRANSPORT_USB) {
        snprintf(text, sizeof(text), "USB");
    } else {
        const char *ble_state = !state.ble_bonded ? "new" : (state.ble_connected ? "ok" : "--");
        snprintf(text, sizeof(text), "BT%d %s", state.endpoint.ble.profile_index + 1, ble_state);
    }
    lv_label_set_text(output_label, text);

    if (state.layer_name != NULL && strlen(state.layer_name) > 0) {
        snprintf(text, sizeof(text), "%s", state.layer_name);
    } else {
        snprintf(text, sizeof(text), "L%d", state.layer_index);
    }
    lv_label_set_text(layer_label, text);

    snprintf(text, sizeof(text), "L %s%u%%", state.usb_powered ? "+" : "", state.battery_left);
    lv_label_set_text(battery_left_label, text);

    if (state.battery_right < 0) {
        snprintf(text, sizeof(text), "R --");
    } else {
        snprintf(text, sizeof(text), "R %d%%", state.battery_right);
    }
    lv_label_set_text(battery_right_label, text);
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

    output_label = lv_label_create(screen);
    lv_obj_align(output_label, LV_ALIGN_TOP_LEFT, 0, 0);

    layer_label = lv_label_create(screen);
    lv_obj_align(layer_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    battery_left_label = lv_label_create(screen);
    lv_obj_align(battery_left_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    battery_right_label = lv_label_create(screen);
    lv_obj_align(battery_right_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    sofle_status_init();
    return screen;
}
