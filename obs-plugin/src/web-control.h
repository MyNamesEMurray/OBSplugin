/*
 * Tiny embedded HTTP server providing a browser control panel for the
 * connected camera devices (zoom / exposure / focus / flashlight),
 * DroidCam-style. Serves on 127.0.0.1 only; control commands are queued
 * onto the addressed source and forwarded to the device over the stream
 * connection.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct ios_camera_source;

/* Implemented in ios-camera-source.c */
void ios_camera_enqueue_control(struct ios_camera_source *s, const char *json,
				size_t len);
void ios_camera_copy_status(struct ios_camera_source *s, char *buf,
			    size_t size);
void ios_camera_copy_state(struct ios_camera_source *s, char *buf,
			   size_t size);
void ios_camera_copy_name(struct ios_camera_source *s, char *buf, size_t size);
bool ios_camera_is_screen(struct ios_camera_source *s);
bool ios_camera_is_standby(struct ios_camera_source *s);
bool ios_camera_is_connected(struct ios_camera_source *s);
bool ios_camera_auto_start(struct ios_camera_source *s);
void ios_camera_set_auto_start(struct ios_camera_source *s, bool on);

/* One server, many sources. Camera sources register at create and
 * unregister at destroy (before freeing anything the upcalls above
 * touch); the server runs while at least one source is registered and
 * the plugin-wide setting is on. Requests address a source with the
 * ?src= id listed by /api/sources; no id means the first source, so
 * single-source setups and older scripts work unchanged.
 * apply_settings reconciles the server with the plugin-wide enable/port
 * settings. All three calls: main/UI thread only. */
void web_control_register(struct ios_camera_source *source);
void web_control_unregister(struct ios_camera_source *source);
void web_control_apply_settings(void);
