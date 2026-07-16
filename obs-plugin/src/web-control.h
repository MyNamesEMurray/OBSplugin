/*
 * Tiny embedded HTTP server providing a browser control panel for the
 * connected iOS camera (zoom / exposure / focus / flashlight), DroidCam-style.
 * Serves on 127.0.0.1 only; control commands are queued onto the source
 * and forwarded to the device over the stream connection.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

struct ios_camera_source;
struct web_control;

/* Implemented in ios-camera-source.c */
void ios_camera_enqueue_control(struct ios_camera_source *s, const char *json,
				size_t len);
void ios_camera_copy_status(struct ios_camera_source *s, char *buf,
			    size_t size);
void ios_camera_copy_state(struct ios_camera_source *s, char *buf,
			   size_t size);
bool ios_camera_is_screen(struct ios_camera_source *s);
bool ios_camera_is_standby(struct ios_camera_source *s);
bool ios_camera_is_connected(struct ios_camera_source *s);
bool ios_camera_auto_start(struct ios_camera_source *s);
void ios_camera_set_auto_start(struct ios_camera_source *s, bool on);

struct web_control *web_control_start(struct ios_camera_source *source,
				      uint16_t port);
void web_control_stop(struct web_control *wc);
