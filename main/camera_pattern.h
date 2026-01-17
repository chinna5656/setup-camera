#ifndef CAMERA_PATTERN_H
#define CAMERA_PATTERN_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief Initialize pattern generator (for use without camera)
	 *
	 * Initializes H.264 encoder for generating color bar test pattern
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t pattern_init(void);

	/**
	 * @brief Start pattern streaming
	 *
	 * Starts generating and encoding color bar test pattern to RTSP
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t pattern_start(void);

	/**
	 * @brief Stop pattern streaming
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t pattern_stop(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_PATTERN_H
