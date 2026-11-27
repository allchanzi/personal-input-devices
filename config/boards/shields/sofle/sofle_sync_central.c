/*
 * Left (central) half of the Sofle status sync: finds the sync service on the
 * right half and subscribes to its USB power state.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include "sofle_sync.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DISCOVER_DELAY K_SECONDS(2)
#define DISCOVER_RETRY_DELAY K_SECONDS(1)
#define DISCOVER_MAX_TRIES 5

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SOFLE_SYNC_SERVICE_UUID);
static struct bt_uuid_128 peripheral_status_uuid =
    BT_UUID_INIT_128(SOFLE_SYNC_PERIPHERAL_STATUS_UUID);

/* Connection to the right half, guarded by lock. */
static struct k_spinlock lock;
static struct bt_conn *peer;

static atomic_t peripheral_flags;

/* GATT client state, only used from BT callbacks and the system work queue. */
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_discover_params sub_discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static struct bt_gatt_read_params read_params;
static uint16_t service_end_handle;
static int discover_tries;
/* Discover once per connection: re-subscribing live params would corrupt them. */
static bool discovery_started;

static struct bt_conn *get_peer(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    struct bt_conn *conn = peer != NULL ? bt_conn_ref(peer) : NULL;
    k_spin_unlock(&lock, key);
    return conn;
}

bool sofle_sync_peripheral_usb_powered(void) {
    return atomic_get(&peripheral_flags) & SOFLE_SYNC_PERIPHERAL_USB_POWERED;
}

static void set_peripheral_flags(uint8_t flags) {
    if (atomic_set(&peripheral_flags, flags) != flags) {
        sofle_status_remote_changed();
    }
}

static void apply_peripheral_msg(const void *data, uint16_t length) {
    if (data != NULL && length >= sizeof(struct sofle_sync_peripheral_msg)) {
        set_peripheral_flags(((const struct sofle_sync_peripheral_msg *)data)->flags);
    }
}

static uint8_t peripheral_status_notify(struct bt_conn *conn,
                                        struct bt_gatt_subscribe_params *params, const void *data,
                                        uint16_t length) {
    if (data == NULL) {
        /* Unsubscribed. */
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    apply_peripheral_msg(data, length);
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t peripheral_status_read(struct bt_conn *conn, uint8_t err,
                                      struct bt_gatt_read_params *params, const void *data,
                                      uint16_t length) {
    if (err) {
        LOG_DBG("Sofle sync read failed (%d)", err);
    } else {
        apply_peripheral_msg(data, length);
    }
    return BT_GATT_ITER_STOP;
}

/* ---- discovery ---- */

static void subscribe_peripheral_status(struct bt_conn *conn, uint16_t value_handle) {
    memset(&subscribe_params, 0, sizeof(subscribe_params));
    subscribe_params.notify = peripheral_status_notify;
    subscribe_params.value = BT_GATT_CCC_NOTIFY;
    subscribe_params.value_handle = value_handle;
    subscribe_params.ccc_handle = 0; /* discovered automatically */
    subscribe_params.end_handle = service_end_handle;
    subscribe_params.disc_params = &sub_discover_params;
    /* Re-subscribe on every connection instead of keeping it for the bond. */
    atomic_set_bit(subscribe_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

    int err = bt_gatt_subscribe(conn, &subscribe_params);
    if (err && err != -EALREADY) {
        LOG_ERR("Sofle sync subscribe failed (%d)", err);
    }

    /* Notifications only report changes: read the current value once. */
    memset(&read_params, 0, sizeof(read_params));
    read_params.func = peripheral_status_read;
    read_params.handle_count = 1;
    read_params.single.handle = value_handle;
    read_params.single.offset = 0;
    err = bt_gatt_read(conn, &read_params);
    if (err) {
        LOG_DBG("Sofle sync read request failed (%d)", err);
    }
}

static uint8_t characteristic_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         struct bt_gatt_discover_params *params) {
    if (attr == NULL) {
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_chrc *chrc = attr->user_data;
    if (bt_uuid_cmp(chrc->uuid, &peripheral_status_uuid.uuid) == 0) {
        subscribe_peripheral_status(conn, chrc->value_handle);
        return BT_GATT_ITER_STOP;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t service_discovered(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  struct bt_gatt_discover_params *params) {
    if (attr == NULL) {
        LOG_WRN("Sofle sync service not found on the right half (old firmware?)");
        return BT_GATT_ITER_STOP;
    }

    const struct bt_gatt_service_val *service = attr->user_data;
    service_end_handle = service->end_handle;

    discover_params.uuid = NULL;
    discover_params.func = characteristic_discovered;
    discover_params.start_handle = attr->handle + 1;
    discover_params.end_handle = service_end_handle;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        LOG_ERR("Sofle sync characteristic discovery failed (%d)", err);
    }
    return BT_GATT_ITER_STOP;
}

static void discover_work_cb(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(discover_work, discover_work_cb);

static void discover_work_cb(struct k_work *work) {
    struct bt_conn *conn = get_peer();
    if (conn == NULL) {
        return;
    }

    memset(&discover_params, 0, sizeof(discover_params));
    discover_params.uuid = &service_uuid.uuid;
    discover_params.func = service_discovered;
    discover_params.start_handle = 0x0001;
    discover_params.end_handle = 0xffff;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    /* ZMK runs its own discovery on the same link; retry if the ATT queue is busy. */
    int err = bt_gatt_discover(conn, &discover_params);
    if (err && ++discover_tries < DISCOVER_MAX_TRIES) {
        LOG_DBG("Sofle sync discovery failed (%d), retrying", err);
        k_work_reschedule(&discover_work, DISCOVER_RETRY_DELAY);
    }
    bt_conn_unref(conn);
}

/* ---- connection tracking (role central = the link to the right half) ---- */

static bool is_split_link(struct bt_conn *conn) {
    struct bt_conn_info info;
    return bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL;
}

static void sync_connected(struct bt_conn *conn, uint8_t err) {
    if (err || !is_split_link(conn)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&lock);
    if (peer != NULL) {
        bt_conn_unref(peer);
    }
    peer = bt_conn_ref(conn);
    k_spin_unlock(&lock, key);

    discovery_started = false;
}

static void sync_disconnected(struct bt_conn *conn, uint8_t reason) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool was_peer = peer == conn;
    if (was_peer) {
        bt_conn_unref(peer);
        peer = NULL;
    }
    k_spin_unlock(&lock, key);

    if (was_peer) {
        k_work_cancel_delayable(&discover_work);
        set_peripheral_flags(0);
    }
}

static void sync_security_changed(struct bt_conn *conn, bt_security_t level,
                                  enum bt_security_err err) {
    if (err || level < BT_SECURITY_L2 || !is_split_link(conn) || discovery_started) {
        return;
    }
    discovery_started = true;
    /* The service needs encryption; give ZMK's own discovery a head start. */
    discover_tries = 0;
    k_work_reschedule(&discover_work, DISCOVER_DELAY);
}

BT_CONN_CB_DEFINE(sofle_sync_conn_callbacks) = {
    .connected = sync_connected,
    .disconnected = sync_disconnected,
    .security_changed = sync_security_changed,
};
