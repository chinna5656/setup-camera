#include "camera_encoder.h"
#include "camera.h"
#include "camera_encoder_common.h"
#include "camera_drawer.h"
#include "rtsp_server.h"
#include "esp_log.h"
#include "esp_h264_enc_single_hw.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "encoder";

#define CAM_WIDTH 1920
#define CAM_HEIGHT 1080
#define CAM_FPS 30
#define BITRATE 4000000
#define GOP_SIZE 30

static int s_video_fd = -1;
static esp_h264_enc_handle_t s_encoder = NULL;
static uint8_t *s_h264_buf = NULL;
static size_t s_h264_buf_size;
static uint32_t s_frame_count;
static bool s_running;
static bool s_sps_pps_sent;
static uint8_t s_cached_sps[256], s_cached_pps[256];
static size_t s_cached_sps_len = 0, s_cached_pps_len = 0;

static void frame_callback(uint8_t *buf, uint8_t idx, uint32_t w, uint32_t h, size_t len)
{
	if (!s_running)
		return;

	// Draw text overlays on camera frame (YUV422 O_UYY_E_VYY format)
	draw_text(buf, w, h, "Connected Experimental Camera", 32, 32, 16, 128, 128);

	// Draw frame counter
	char frame_text[64];
	snprintf(frame_text, sizeof(frame_text), "1920x1080 30 FPS #%lu", (unsigned long)s_frame_count);
	draw_text(buf, w, h, frame_text, 32, 52, 16, 128, 128);

	// O_UYY_E_VYY format passed to encoder
	esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = buf, .len = len}};
	esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = s_h264_buf, .len = s_h264_buf_size}};

	if (esp_h264_enc_process(s_encoder, &in, &out) == ESP_H264_ERR_OK && out.raw_data.len > 0)
	{
		// Find actual H.264 data size
		size_t actual_len = find_h264_data_end(out.raw_data.buffer, out.raw_data.len);

		// Log first frame details
		if (s_frame_count == 0)
		{
			ESP_LOGI(TAG, "First H.264 frame: %d bytes (searched %d bytes)",
					 actual_len, out.raw_data.len);
			extract_sps_pps(out.raw_data.buffer, actual_len, s_cached_sps, &s_cached_sps_len,
							s_cached_pps, &s_cached_pps_len, &s_sps_pps_sent);
		}
		else if (s_frame_count % 300 == 0)
		{
			ESP_LOGI(TAG, "Frame %u: %d bytes", s_frame_count, actual_len);
		}

		// For I-frames, prepend cached SPS/PPS if available
		bool is_iframe = false;
		const uint8_t *data = out.raw_data.buffer;
		size_t data_len = actual_len;

		// Scan ALL NAL units to check if this is an I-frame
		for (size_t i = 0; i < data_len - 4; i++)
		{
			size_t skip = 0;
			if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
			{
				skip = 4;
			}
			else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			{
				skip = 3;
			}
			else
			{
				continue;
			}

			uint8_t nal_header = data[i + skip];
			uint8_t nal_type = nal_header & 0x1F;

			// Check if this NAL is an IDR frame (type 5)
			if (nal_type == 5)
			{
				is_iframe = true;
				if (s_frame_count < 5 || (s_frame_count % 30 == 0))
				{
					ESP_LOGI(TAG, "Frame %u: Found IDR NAL type %d at offset %u", s_frame_count, nal_type, i);
				}
				break;
			}
		}

		if (is_iframe && s_cached_sps_len > 0 && s_cached_pps_len > 0)
		{
			// Send SPS first
			rtsp_send_h264_frame(s_cached_sps, s_cached_sps_len, s_frame_count * (90000 / CAM_FPS));
			// Send PPS
			rtsp_send_h264_frame(s_cached_pps, s_cached_pps_len, s_frame_count * (90000 / CAM_FPS));
			ESP_LOGI(TAG, "Prepended cached SPS/PPS to I-frame %u", s_frame_count);
		}

		uint32_t ts = s_frame_count * (90000 / CAM_FPS);
		rtsp_send_h264_frame(out.raw_data.buffer, actual_len, ts);
		s_frame_count++;
	}
}

esp_err_t camera_encoder_init(void)
{
	ESP_LOGI(TAG, "Init encoder");

	if (camera_init() != ESP_OK)
		return ESP_FAIL;
	s_video_fd = camera_open(VIDEO_FMT_YUV420);
	if (s_video_fd < 0)
		return ESP_FAIL;
	if (camera_setup_buffers(s_video_fd) != ESP_OK)
		return ESP_FAIL;

	s_h264_buf_size = 3072 * 1024;
	s_h264_buf = alloc_aligned_buffer(s_h264_buf_size, "H264 buffer");
	if (!s_h264_buf)
		return ESP_ERR_NO_MEM;

	// Use hardware encoder with YUV422 O_UYY_E_VYY format directly
	esp_h264_enc_cfg_t cfg = {
		.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
		.gop = GOP_SIZE,
		.fps = CAM_FPS,
		.res = {.width = CAM_WIDTH, .height = CAM_HEIGHT},
		.rc = {.bitrate = BITRATE, .qp_min = 10, .qp_max = 40}};

	if (esp_h264_enc_hw_new(&cfg, &s_encoder) != ESP_H264_ERR_OK)
		return ESP_FAIL;
	if (esp_h264_enc_open(s_encoder) != ESP_H264_ERR_OK)
		return ESP_FAIL;

	ESP_LOGI(TAG, "Encoder ready (HW, YUV422 O_UYY_E_VYY): %dx%d@%d", CAM_WIDTH, CAM_HEIGHT, CAM_FPS);
	return ESP_OK;
}

esp_err_t camera_encoder_start(void)
{
	if (s_running)
		return ESP_OK;
	s_running = true;
	s_frame_count = 0;
	return camera_start(s_video_fd, 1, frame_callback);
}

esp_err_t camera_encoder_stop(void)
{
	s_running = false;
	return camera_stop(s_video_fd);
}

uint32_t camera_get_width(void) { return CAM_WIDTH; }
uint32_t camera_get_height(void) { return CAM_HEIGHT; }
