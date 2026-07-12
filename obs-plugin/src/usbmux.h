/*
 * Minimal usbmuxd client: opens a TCP-like channel to an app listening on
 * a USB-connected iOS device, via Apple's device multiplexing service
 * (Apple Mobile Device Service on Windows, usbmuxd on macOS/Linux).
 */

#pragma once

#include <stdint.h>
#include "net-compat.h"

/*
 * Connects to the first attached iOS device on `device_port` (the port the
 * companion app listens on). On success returns a non-blocking socket that
 * is a raw byte pipe to the app. Returns OBSC_INVALID_SOCKET if no device
 * is attached, the mux service isn't running, or the app isn't listening.
 */
socket_t usbmux_connect_first(uint16_t device_port);
