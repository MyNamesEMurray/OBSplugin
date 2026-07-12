/*
 * Minimal usbmuxd client: opens a TCP-like channel to an app listening on
 * a USB-connected iOS device, via Apple's device multiplexing service
 * (Apple Mobile Device Service on Windows, usbmuxd on macOS/Linux).
 */

#pragma once

#include <stdint.h>
#include "net-compat.h"

struct usbmux_device {
	long id;        /* usbmuxd device id — ephemeral, changes on replug */
	char udid[64];  /* serial number — stable across replug and reboot */
};

/*
 * Lists currently attached USB devices (Wi-Fi sync entries excluded).
 * Fills up to `max` into `out` and returns the count. Use `udid` for a
 * stable identity; `id` is only valid for the current attachment.
 */
int usbmux_list_devices(struct usbmux_device *out, int max);

/*
 * Connects to a specific device (by usbmuxd device id) on `device_port`
 * (the port the companion app listens on). On success returns a
 * non-blocking socket that is a raw byte pipe to the app;
 * OBSC_INVALID_SOCKET if the mux service is down or the app isn't listening.
 */
socket_t usbmux_connect_device(long device_id, uint16_t device_port);
