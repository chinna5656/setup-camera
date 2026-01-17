#ifndef CAMERA_ENCODER_COMMON_H
#define CAMERA_ENCODER_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief Allocate aligned buffer for video data
	 *
	 * @param size Buffer size in bytes
	 * @param name Name for logging
	 * @return Pointer to allocated buffer or NULL on failure
	 */
	void *alloc_aligned_buffer(size_t size, const char *name);

	/**
	 * @brief Find actual H.264 data end in encoder output buffer
	 *
	 * The H.264 encoder returns the buffer size, not the actual encoded size.
	 * This function searches for the end of the last NAL unit.
	 *
	 * @param data H.264 data buffer
	 * @param max_len Maximum buffer size
	 * @return Actual data length
	 */
	size_t find_h264_data_end(const uint8_t *data, size_t max_len);

	/**
	 * @brief Extract SPS and PPS from H.264 data
	 *
	 * @param data H.264 data buffer
	 * @param len Data length
	 * @param cached_sps Buffer to cache SPS
	 * @param cached_sps_len Pointer to SPS length
	 * @param cached_pps Buffer to cache PPS
	 * @param cached_pps_len Pointer to PPS length
	 * @param sps_pps_sent Pointer to flag indicating SPS/PPS were sent
	 */
	void extract_sps_pps(const uint8_t *data, size_t len,
						uint8_t *cached_sps, size_t *cached_sps_len,
						uint8_t *cached_pps, size_t *cached_pps_len,
						bool *sps_pps_sent);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_ENCODER_COMMON_H
