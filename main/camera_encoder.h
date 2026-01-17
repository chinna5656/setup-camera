#ifndef CAMERA_ENCODER_H
#define CAMERA_ENCODER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief VBR mode enumeration
	 */
	typedef enum
	{
		VBR_MODE_CONSTANT = 0,		  // Fixed bitrate (manual or preset)
		VBR_MODE_SCENE_BASED = 1,	  // Auto-adjust based on scene complexity
		VBR_MODE_NETWORK_ADAPTIVE = 2 // Auto-adjust based on network feedback
	} vbr_mode_t;

	/**
	 * @brief Network feedback callback type
	 */
	typedef void (*network_feedback_cb_t)(uint32_t bandwidth_bps, uint8_t packet_loss_percent);

	/**
	 * @brief VBR statistics structure
	 */
	typedef struct
	{
		uint32_t current_bitrate;
		uint32_t min_bitrate;
		uint32_t max_bitrate;
		uint32_t avg_frame_size;
		uint8_t motion_level;
		vbr_mode_t mode;
	} vbr_stats_t;

	/**
	 * @brief Initialize camera and H.264 encoder
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_init(void);

	/**
	 * @brief Start camera capture and H.264 encoding
	 *
	 * Starts the camera stream, encodes frames to H.264, and sends via RTSP
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_start(void);

	/**
	 * @brief Stop camera capture and encoding
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_stop(void);

	/**
	 * @brief Get camera frame width
	 *
	 * @return Width in pixels
	 */
	uint32_t camera_get_width(void);

	/**
	 * @brief Get camera frame height
	 *
	 * @return Height in pixels
	 */
	uint32_t camera_get_height(void);

	/**
	 * @brief Initialize test pattern generator (for use without camera)
	 *
	 * Initializes H.264 encoder for generating RGB test pattern bars
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t test_pattern_init(void);

	/**
	 * @brief Start test pattern streaming
	 *
	 * Starts generating and encoding RGB test pattern bars to RTSP
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t test_pattern_start(void);

	/**
	 * @brief Stop test pattern streaming
	 *
	 * @return ESP_OK on success
	 */
	esp_err_t test_pattern_stop(void);

	// VBR API functions

	/**
	 * @brief Set encoder bitrate dynamically
	 *
	 * @param bitrate Target bitrate in bits per second (100K-20M range)
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_set_bitrate(uint32_t bitrate);

	/**
	 * @brief Get current encoder bitrate
	 *
	 * @return Current bitrate in bits per second
	 */
	uint32_t camera_encoder_get_bitrate(void);

	/**
	 * @brief Set VBR mode
	 *
	 * @param mode VBR mode (CONSTANT, SCENE_BASED, NETWORK_ADAPTIVE)
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_set_vbr_mode(vbr_mode_t mode);

	/**
	 * @brief Get current VBR mode
	 *
	 * @return Current VBR mode
	 */
	vbr_mode_t camera_encoder_get_vbr_mode(void);

	/**
	 * @brief Set bitrate range for VBR
	 *
	 * @param min Minimum bitrate in bits per second
	 * @param max Maximum bitrate in bits per second
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_set_bitrate_range(uint32_t min, uint32_t max);

	/**
	 * @brief Get VBR statistics
	 *
	 * @param stats Pointer to vbr_stats_t structure to fill
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_get_vbr_stats(vbr_stats_t *stats);

	/**
	 * @brief Register network feedback callback
	 *
	 * @param cb Callback function pointer
	 * @return ESP_OK on success
	 */
	esp_err_t camera_encoder_set_network_callback(network_feedback_cb_t cb);

	/**
	 * @brief Update network bandwidth target (for network-adaptive VBR)
	 *
	 * @param bandwidth_bps Target bandwidth in bits per second
	 */
	void camera_encoder_update_network_bandwidth(uint32_t bandwidth_bps);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_ENCODER_H
