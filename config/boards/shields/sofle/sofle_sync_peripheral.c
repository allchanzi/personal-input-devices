/*
 * Right (peripheral) half of the Sofle status sync: hosts the GATT service
 * and reports its USB power state to the left half.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>

#include "sofle_sync.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct sofle_sync_peripheral_msg peripheral_msg(void) {
    struct sofle_sync_peripheral_msg msg = {0};
    if (zmk_usb_is_powered()) {
        msg.flags |= SOFLE_SYNC_PERIPHERAL_USB_POWERED;
    }
    return msg;
}

static ssize_t read_peripheral_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset) {
    struct sofle_sync_peripheral_msg msg = peripheral_msg();
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &msg, sizeof(msg));
}

static void notify_work_cb(struct k_work *work);
K_WORK_DEFINE(notify_work, notify_work_cb);

static void peripheral_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    if (value & BT_GATT_CCC_NOTIFY) {
        k_work_submit(&notify_work);
    }
}

BT_GATT_SERVICE_DEFINE(
    sofle_sync_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(SOFLE_SYNC_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(SOFLE_SYNC_PERIPHERAL_STATUS_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           read_peripheral_status, NULL, NULL),
    BT_GATT_CCC(peripheral_status_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

/* attrs: 0 service, 1/2 peripheral status decl/value, 3 CCC. */
static void notify_work_cb(struct k_work *work) {
    struct sofle_sync_peripheral_msg msg = peripheral_msg();
    int err = bt_gatt_notify(NULL, &sofle_sync_svc.attrs[1], &msg, sizeof(msg));
    if (err && err != -ENOTCONN) {
        LOG_DBG("Sofle sync notify failed (%d)", err);
    }
}

/* USB power changed: tell the left half. */
static int sofle_sync_listener(const zmk_event_t *eh) {
    k_work_submit(&notify_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sofle_sync, sofle_sync_listener);
ZMK_SUBSCRIPTION(sofle_sync, zmk_usb_conn_state_changed);
