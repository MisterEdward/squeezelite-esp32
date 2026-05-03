/*
 *  Squeezelite for esp32
 *
 *  Phase 2 alert storage service
 *
 */

#include "alert_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tools.h"

#define ALERT_FILE_NAME_MAX 64
#define ALERT_MOUNT_POINT "/spiffs"
#define ALERT_PARTITION_LABEL "alerts"
#define ALERT_TASK_STACK_SIZE (4 * 1024)

static const char TAG[] = "alert_service";

typedef struct {
	char file[ALERT_FILE_NAME_MAX];
} alert_request_t;

typedef struct __attribute__((packed)) {
	char riff[4];
	uint32_t size;
	char wave[4];
} wav_riff_header_t;

typedef struct __attribute__((packed)) {
	char id[4];
	uint32_t size;
} wav_chunk_header_t;

typedef struct __attribute__((packed)) {
	uint16_t audio_format;
	uint16_t num_channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
} wav_fmt_chunk_t;

static EXT_RAM_ATTR QueueHandle_t alert_queue;
static EXT_RAM_ATTR SemaphoreHandle_t alert_state_mutex;
static EXT_RAM_ATTR alert_state_t alert_state;
static bool spiffs_mounted;

static bool alert_filename_is_valid(const char *filename) {
	if (filename == NULL || *filename == '\0') return false;
	if (strlen(filename) >= ALERT_FILE_NAME_MAX) return false;
	if (strstr(filename, "..") != NULL) return false;
	if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) return false;
	return true;
}

static void alert_state_replace_buffer(int16_t *buffer, size_t len, const char *filename) {
	xSemaphoreTake(alert_state_mutex, portMAX_DELAY);
	if (alert_state.buffer) {
		free(alert_state.buffer);
	}
	memset(&alert_state, 0, sizeof(alert_state));
	alert_state.buffer = buffer;
	alert_state.len = len;
	alert_state.active = (buffer != NULL && len > 0);
	if (filename) {
		strlcpy(alert_state.file, filename, sizeof(alert_state.file));
	}
	xSemaphoreGive(alert_state_mutex);
}

static esp_err_t alert_mount_spiffs(void) {
	if (spiffs_mounted) return ESP_OK;

	esp_vfs_spiffs_conf_t conf = {
		.base_path = ALERT_MOUNT_POINT,
		.partition_label = ALERT_PARTITION_LABEL,
		.max_files = 4,
		.format_if_mount_failed = false,
	};

	esp_err_t err = esp_vfs_spiffs_register(&conf);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to mount SPIFFS partition '%s': %s", ALERT_PARTITION_LABEL, esp_err_to_name(err));
		return err;
	}

	size_t total = 0, used = 0;
	err = esp_spiffs_info(ALERT_PARTITION_LABEL, &total, &used);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "SPIFFS mounted but info query failed: %s", esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "SPIFFS mounted at %s (%u/%u bytes used)", ALERT_MOUNT_POINT, (unsigned) used, (unsigned) total);
	}

	spiffs_mounted = true;
	return ESP_OK;
}

static esp_err_t alert_load_wav_file(const char *filename, int16_t **buffer, size_t *len) {
	char path[128];
	FILE *file = NULL;
	wav_riff_header_t riff = { 0 };
	wav_fmt_chunk_t fmt = { 0 };
	bool fmt_found = false;
	bool data_found = false;
	long data_offset = 0;
	uint32_t data_size = 0;
	esp_err_t err = ESP_FAIL;

	*buffer = NULL;
	*len = 0;

	snprintf(path, sizeof(path), ALERT_MOUNT_POINT "/%s", filename);
	file = fopen(path, "rb");
	if (!file) {
		ESP_LOGE(TAG, "Cannot open alert file %s", path);
		return ESP_ERR_NOT_FOUND;
	}

	if (fread(&riff, sizeof(riff), 1, file) != 1) {
		ESP_LOGE(TAG, "Failed to read WAV header from %s", path);
		goto done;
	}

	if (memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4)) {
		ESP_LOGE(TAG, "File %s is not a RIFF/WAVE file", path);
		err = ESP_ERR_INVALID_ARG;
		goto done;
	}

	while (!data_found) {
		wav_chunk_header_t chunk = { 0 };
		long chunk_data_pos;

		if (fread(&chunk, sizeof(chunk), 1, file) != 1) {
			break;
		}

		chunk_data_pos = ftell(file);
		if (!memcmp(chunk.id, "fmt ", 4)) {
			if (chunk.size < sizeof(fmt)) {
				ESP_LOGE(TAG, "fmt chunk too small in %s", path);
				err = ESP_ERR_INVALID_ARG;
				goto done;
			}
			if (fread(&fmt, sizeof(fmt), 1, file) != 1) {
				ESP_LOGE(TAG, "Failed to read fmt chunk from %s", path);
				goto done;
			}
			fmt_found = true;
		} else if (!memcmp(chunk.id, "data", 4)) {
			data_offset = chunk_data_pos;
			data_size = chunk.size;
			data_found = true;
		}

		if (fseek(file, chunk_data_pos + chunk.size + (chunk.size & 1), SEEK_SET) != 0) {
			ESP_LOGE(TAG, "Failed to seek inside %s", path);
			goto done;
		}
	}

	if (!fmt_found || !data_found) {
		ESP_LOGE(TAG, "Missing fmt/data chunk in %s", path);
		err = ESP_ERR_INVALID_ARG;
		goto done;
	}

	if (fmt.audio_format != 1 || fmt.num_channels != 1 || fmt.sample_rate != 44100 ||
		fmt.bits_per_sample != 16 || fmt.block_align != 2) {
		ESP_LOGE(TAG,
				 "Unsupported WAV format in %s: fmt=%u ch=%u rate=%u bits=%u align=%u",
				 path, fmt.audio_format, fmt.num_channels, (unsigned) fmt.sample_rate,
				 fmt.bits_per_sample, fmt.block_align);
		err = ESP_ERR_INVALID_ARG;
		goto done;
	}

	if (data_size == 0 || (data_size % sizeof(int16_t)) != 0) {
		ESP_LOGE(TAG, "Invalid data chunk size %u in %s", (unsigned) data_size, path);
		err = ESP_ERR_INVALID_SIZE;
		goto done;
	}

	*buffer = malloc_init_external(data_size);
	if (*buffer == NULL) {
		err = ESP_ERR_NO_MEM;
		goto done;
	}

	if (fseek(file, data_offset, SEEK_SET) != 0) {
		ESP_LOGE(TAG, "Failed to seek to PCM data in %s", path);
		err = ESP_FAIL;
		goto done;
	}

	if (fread(*buffer, 1, data_size, file) != data_size) {
		ESP_LOGE(TAG, "Failed to read PCM payload from %s", path);
		err = ESP_FAIL;
		goto done;
	}

	*len = data_size / sizeof(int16_t);
	err = ESP_OK;

done:
	if (err != ESP_OK && *buffer) {
		free(*buffer);
		*buffer = NULL;
		*len = 0;
	}
	if (file) fclose(file);
	return err;
}

static void alert_loader_task(void *arg) {
	alert_request_t request;

	while (xQueueReceive(alert_queue, &request, portMAX_DELAY) == pdTRUE) {
		int16_t *buffer = NULL;
		size_t len = 0;
		esp_err_t err = alert_load_wav_file(request.file, &buffer, &len);

		if (err == ESP_OK) {
			alert_state_replace_buffer(buffer, len, request.file);
			ESP_LOGI(TAG, "Loaded alert %s into PSRAM (%u samples)", request.file, (unsigned) len);
		} else {
			ESP_LOGE(TAG, "Failed to load alert %s: %s", request.file, esp_err_to_name(err));
		}
	}

	vTaskDelete(NULL);
}

esp_err_t alert_service_init(void) {
	if (alert_state_mutex && alert_queue) return alert_mount_spiffs();

	alert_state_mutex = xSemaphoreCreateMutex();
	if (alert_state_mutex == NULL) {
		ESP_LOGE(TAG, "Unable to create alert state mutex");
		return ESP_ERR_NO_MEM;
	}

	alert_queue = xQueueCreate(1, sizeof(alert_request_t));
	if (alert_queue == NULL) {
		ESP_LOGE(TAG, "Unable to create alert queue");
		return ESP_ERR_NO_MEM;
	}

	if (xTaskCreate(alert_loader_task, "alert_loader", ALERT_TASK_STACK_SIZE, NULL,
					ESP_TASK_PRIO_MIN + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Unable to create alert loader task");
		return ESP_ERR_NO_MEM;
	}

	return alert_mount_spiffs();
}

esp_err_t alert_service_request_play(const char *filename) {
	alert_request_t request = { 0 };

	if (!alert_filename_is_valid(filename)) {
		return ESP_ERR_INVALID_ARG;
	}

	if (alert_queue == NULL || alert_state_mutex == NULL) {
		return ESP_ERR_INVALID_STATE;
	}

	strlcpy(request.file, filename, sizeof(request.file));
	if (xQueueOverwrite(alert_queue, &request) != pdPASS) {
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Queued alert load for %s", request.file);
	return ESP_OK;
}

const alert_state_t *alert_service_state(void) {
	return &alert_state;
}
