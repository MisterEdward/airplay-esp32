#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

/**
 * One place that knows what the speaker is doing right now, for the web UI
 * (/api/status) and for diagnostics.  Subscribes to RTSP events to keep the
 * "now playing" metadata; everything else is read live from the modules.
 */

esp_err_t device_status_init(void);

/** Build the /api/status document.  Caller owns the returned object. */
cJSON *device_status_build_json(void);

/** Mark the running image valid (OTA rollback) once the device is healthy. */
void device_status_ota_mark_valid_if_pending(const char *reason);
