#include "camera_encoder_common.h"
#include "esp_log.h"
#include "rtsp_server.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "encoder_common";

void *alloc_aligned_buffer(size_t size, const char *name)
{
	uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!buf)
	{
		buf = (uint8_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
	}
	if (!buf)
	{
		buf = (uint8_t *)heap_caps_aligned_alloc(64, size, MALLOC_CAP_8BIT);
	}
	if (buf)
	{
		ESP_LOGI(TAG, "Allocated %s: %d bytes", name, size);
	}
	else
	{
		ESP_LOGE(TAG, "Failed to allocate %s: %d bytes", name, size);
	}
	return buf;
}

size_t find_h264_data_end(const uint8_t *data, size_t max_len)
{
	// Search backwards for NAL start codes (00 00 00 01 or 00 00 01)
	// Start from the end but leave room for a potential small NAL
	for (size_t i = max_len - 10; i > 0; i--)
	{
		// Check for 00 00 00 01
		if (i < max_len - 4 &&
			data[i] == 0 && data[i + 1] == 0 &&
			data[i + 2] == 0 && data[i + 3] == 1)
		{
			// Found a start code - data continues after this NAL
			// Continue searching backwards
		}

		// Check for 00 00 01
		if (i < max_len - 3 &&
			data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
		{
			// Found a start code - see if there's more data after it
			for (size_t j = i + 3; j < max_len; j++)
			{
				// If we find non-zero data after this start code, this might be valid data
				if (data[j] != 0)
				{
					// Found potential data, continue from here
					// Find the end by looking for the next zero region
					for (size_t k = j; k < max_len; k++)
					{
						// Check if we hit a large zero region (padding)
						if (data[k] == 0)
						{
							size_t zero_count = 0;
							for (size_t z = k; z < max_len && data[z] == 0; z++)
							{
								zero_count++;
							}
							// If we have 8+ zeros, this is likely padding
							if (zero_count >= 8)
							{
								return k;
							}
							k += zero_count - 1;
						}
					}
					return max_len;
				}
			}
		}
	}

	// Fallback: search for the last non-zero byte
	for (size_t i = max_len - 1; i > 0; i--)
	{
		if (data[i] != 0)
		{
			// Round up to next 4-byte boundary for safety
			return ((i + 4) & ~3);
		}
	}

	return max_len;
}

void extract_sps_pps(const uint8_t *data, size_t len,
					uint8_t *cached_sps, size_t *cached_sps_len,
					uint8_t *cached_pps, size_t *cached_pps_len,
					bool *sps_pps_sent)
{
	if (*sps_pps_sent)
		return;

	const uint8_t *sps = NULL, *pps = NULL;
	size_t sps_len = 0, pps_len = 0;
	const uint8_t *nal = data;

	ESP_LOGI(TAG, "Searching for SPS/PPS in %d bytes (first 16: %02x %02x %02x %02x %02x %02x %02x %02x...)",
			 len, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);

	while (nal < data + len - 4)
	{
		// Track start code position
		const uint8_t *start_code = nal;
		size_t start_code_len = 0;

		if (nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1)
		{
			start_code_len = 4;
			nal += 4;
		}
		else if (nal[0] == 0 && nal[1] == 0 && nal[2] == 1)
		{
			start_code_len = 3;
			nal += 3;
		}
		else
		{
			nal++;
			continue;
		}

		uint8_t nal_type = nal[0] & 0x1F;
		ESP_LOGI(TAG, "Found NAL type %d at offset %td", nal_type, nal - data);

		if (nal_type == 7 && !sps)
		{ // SPS
			const uint8_t *end = nal;
			while (end < data + len - 3)
			{
				if ((end[0] == 0 && end[1] == 0 && end[2] == 1) ||
					(end[0] == 0 && end[1] == 0 && end[2] == 0 && end[3] == 1))
					break;
				end++;
			}
			sps = start_code; // Include start code
			sps_len = end - start_code;
			ESP_LOGI(TAG, "Found SPS: %d bytes at offset %td (start_code_len=%d)", sps_len, sps - data, start_code_len);
		}
		else if (nal_type == 8 && !pps)
		{ // PPS
			const uint8_t *end = nal;
			while (end < data + len - 3)
			{
				if ((end[0] == 0 && end[1] == 0 && end[2] == 1) ||
					(end[0] == 0 && end[1] == 0 && end[2] == 0 && end[3] == 1))
					break;
				end++;
			}
			pps = start_code; // Include start code
			pps_len = end - start_code;
			ESP_LOGI(TAG, "Found PPS: %d bytes at offset %td (start_code_len=%d)", pps_len, pps - data, start_code_len);
		}

		if (sps && pps)
		{
			// Cache locally for prepending to I-frames
			*cached_sps_len = (sps_len <= 256) ? sps_len : 256;
			*cached_pps_len = (pps_len <= 256) ? pps_len : 256;
			memcpy(cached_sps, sps, *cached_sps_len);
			memcpy(cached_pps, pps, *cached_pps_len);

			// Also send to RTSP server for SDP
			rtsp_set_sps_pps(sps, sps_len, pps, pps_len);
			*sps_pps_sent = true;
			ESP_LOGI(TAG, "Cached SPS/PPS: SPS=%d bytes, PPS=%d bytes", *cached_sps_len, *cached_pps_len);
			break;
		}
	}

	if (!sps || !pps)
	{
		ESP_LOGW(TAG, "SPS/PPS not complete (SPS=%p, PPS=%p)", sps, pps);
	}
}
