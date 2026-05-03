/*
 *  Squeezelite for esp32
 *
 *  Phase 2 alert storage service
 *
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
	bool active;
	int16_t *buffer;
	size_t len;
	size_t pos;
	char file[64];
} alert_state_t;

esp_err_t alert_service_init(void);
esp_err_t alert_service_request_play(const char *filename);
const alert_state_t *alert_service_state(void);
