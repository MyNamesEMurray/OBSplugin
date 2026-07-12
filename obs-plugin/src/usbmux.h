/*
 * Minimal usbmuxd client: opens a TCP-like channel to an app listening on
 * a USB-connected iOS device, via Apple's device multiplexing service
 * (Apple Mobile Device Service on Windows, usbmuxd on macOS/Linux).
 */

#pragma once

#include <stdint.h>
#include "net-compat.h"

/*
 * Lists the usbmuxd device IDs of currently attached USB devices (Wi-Fi
 * sync entries are excluded). Fills up to `max` into `ids` and returns the
 * count. IDs are stable while a device stays plugged in.
 */
int usbmux_list_devices(long *ids, int max);

/*
 * Connects to a specific device (by usbmuxd device ID) on `device_port`
 * (the port the companion app listens on). On success returns a
 * non-blocking socket that is a raw byte pipe to the app;
 * OBSC_INVALID_SOCKET if the mux service is down or the app isn't listening.
 */
socket_t usbmux_connect_device(long device_id, uint16_t device_port);
