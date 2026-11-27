/*
 * Status sync between the Sofle halves over a small custom GATT service
 * hosted by the right (peripheral) half, next to ZMK's own split service.
 *
 * The right half has no display, so the only thing it reports is whether it
 * is on USB power: read once after connecting, then notified on change. The
 * left half shows it as a bolt next to the right battery.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#define SOFLE_SYNC_SERVICE_UUID                                                                    \
    BT_UUID_128_ENCODE(0x5f1e0000, 0x8c3a, 0x4b7e, 0x9d21, 0x6a0c3e5b7f10)
#define SOFLE_SYNC_PERIPHERAL_STATUS_UUID                                                          \
    BT_UUID_128_ENCODE(0x5f1e0002, 0x8c3a, 0x4b7e, 0x9d21, 0x6a0c3e5b7f10)

/* Peripheral status flags. */
#define SOFLE_SYNC_PERIPHERAL_USB_POWERED BIT(0)

struct sofle_sync_peripheral_msg {
    uint8_t flags;
} __packed;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Implemented by the status screen: re-read the state and redraw if it changed. */
void sofle_status_remote_changed(void);

/* Whether the right half reported USB power (false while disconnected). */
bool sofle_sync_peripheral_usb_powered(void);
#endif
