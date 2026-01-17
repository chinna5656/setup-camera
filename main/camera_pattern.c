#include "camera_pattern.h"
#include "camera_drawer.h"
#include "camera_encoder_common.h"
#include "rtsp_server.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pattern";

#define CAM_WIDTH 1920
#define CAM_HEIGHT 1080
#define CAM_FPS 30
#define BITRATE 460000
#define GOP_SIZE 30

static esp_h264_enc_handle_t s_encoder = NULL;
static uint8_t *s_h264_buf = NULL;
static size_t s_h264_buf_size;
static TaskHandle_t s_pattern_task = NULL;
static bool s_pattern_running = false;
static bool s_sps_pps_sent;
static uint8_t s_cached_sps[256], s_cached_pps[256];
static size_t s_cached_sps_len = 0, s_cached_pps_len = 0;

/**
 * @brief Fill a region with white background (for text area)
 */
static void fill_white_background(uint8_t *yuv, uint32_t width, uint32_t height,
								  int x, int y, int w, int h)
{
	uint32_t row_stride = (width / 2) * 3;

	for (int py = y; py < y + h && py < (int)height; py += 2)
	{
		for (int px = x; px < x + w && px < (int)width; px += 2)
		{
			uint32_t block_idx = (px / 2) * 3;

			uint8_t y_val = 235; // White Y value
			uint8_t u_val = 128; // Neutral U
			uint8_t v_val = 128; // Neutral V

			// Even line: U Y00 Y01
			yuv[py * row_stride + block_idx + 0] = u_val;
			yuv[py * row_stride + block_idx + 1] = y_val;
			yuv[py * row_stride + block_idx + 2] = y_val;

			// Odd line: V Y10 Y11
			if (py + 1 < (int)height)
			{
				yuv[(py + 1) * row_stride + block_idx + 0] = v_val;
				yuv[(py + 1) * row_stride + block_idx + 1] = y_val;
				yuv[(py + 1) * row_stride + block_idx + 2] = y_val;
			}
		}
	}
}

static void pattern_task(void *arg)
{
	// Allocate O_UYY_E_VYY format buffer with layout even rows (U Y00 Y01), odd rows (V Y10 Y11)
	size_t yuv_size = CAM_WIDTH * CAM_HEIGHT * 3 / 2;
	yuv_size = (yuv_size + 63) & ~0x3F;
	uint8_t *yuv = alloc_aligned_buffer(yuv_size, "pattern O_UYY_E_VYY");
	if (!yuv)
	{
		s_pattern_running = false;
		vTaskDelete(NULL);
		return;
	}

	// Standard SMPTE color bars in YUV
	const uint8_t y_values[8] = {235, 210, 170, 145, 106, 81, 41, 16};
	const uint8_t u_values[8] = {128, 16, 166, 54, 202, 90, 240, 128};
	const uint8_t v_values[8] = {128, 146, 16, 34, 222, 240, 110, 128};

	uint32_t bar_w = CAM_WIDTH / 8;
	uint32_t row_stride = (CAM_WIDTH / 2) * 3;

	// Generate color bars in O_UYY_E_VYY format directly
	for (uint32_t y = 0; y < CAM_HEIGHT; y += 2)
	{
		for (uint32_t x = 0; x < CAM_WIDTH; x += 2)
		{
			uint32_t bar_idx = (x / bar_w);
			if (bar_idx >= 8)
				bar_idx = 7;

			uint8_t y0 = y_values[bar_idx];
			uint8_t y1 = y_values[(x + 2) / bar_w < 8 ? (x + 2) / bar_w : bar_idx];
			uint8_t u = u_values[bar_idx];
			uint8_t v = v_values[bar_idx];

			uint32_t block_idx = (x / 2) * 3;

			// Even line: U Y00 Y01
			yuv[y * row_stride + block_idx + 0] = u;
			yuv[y * row_stride + block_idx + 1] = y0;
			yuv[y * row_stride + block_idx + 2] = y1;

			// Odd line: V Y10 Y11
			yuv[(y + 1) * row_stride + block_idx + 0] = v;
			yuv[(y + 1) * row_stride + block_idx + 1] = y0;
			yuv[(y + 1) * row_stride + block_idx + 2] = y1;
		}
	}

	TickType_t last_time = xTaskGetTickCount();
	uint32_t frame = 0;

	while (s_pattern_running)
	{
		vTaskDelayUntil(&last_time, pdMS_TO_TICKS(1000 / CAM_FPS));

		// Fill white background for text area
		fill_white_background(yuv, CAM_WIDTH, CAM_HEIGHT, 32, 32, 360, 52);

		// Draw text overlays
		draw_text(yuv, CAM_WIDTH, CAM_HEIGHT, "Connected Experimental Camera", 32, 40, 16, 128, 128);

		// Draw frame counter
		char frame_text[64];
		snprintf(frame_text, sizeof(frame_text), "1920x1080 30 FPS #%lu", (unsigned long)frame);
		draw_text(yuv, CAM_WIDTH, CAM_HEIGHT, frame_text, 32, 60, 16, 128, 128);

		// Pass O_UYY_E_VYY format to encoder
		esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = yuv, .len = yuv_size}};
		esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = s_h264_buf, .len = s_h264_buf_size}};

		if (esp_h264_enc_process(s_encoder, &in, &out) == ESP_H264_ERR_OK && out.raw_data.len > 0)
		{
			size_t len = find_h264_data_end(out.raw_data.buffer, out.raw_data.len);

			if (frame == 0)
			{
				extract_sps_pps(out.raw_data.buffer, len, s_cached_sps, &s_cached_sps_len,
								s_cached_pps, &s_cached_pps_len, &s_sps_pps_sent);
			}

			uint32_t ts = frame * (90000 / CAM_FPS);
			rtsp_send_h264_frame(out.raw_data.buffer, len, ts);
			frame++;
		}
	}

	heap_caps_free(yuv);
	s_pattern_running = false;
	vTaskDelete(NULL);
}

esp_err_t pattern_init(void)
{
	ESP_LOGI(TAG, "Initializing pattern");

	s_h264_buf_size = 3072 * 1024;
	s_h264_buf = alloc_aligned_buffer(s_h264_buf_size, "H264 buffer");
	if (!s_h264_buf)
		return ESP_ERR_NO_MEM;

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

	return ESP_OK;
}

esp_err_t pattern_start(void)
{
	if (s_pattern_running)
		return ESP_OK;
	s_pattern_running = true;
	BaseType_t ret = xTaskCreatePinnedToCore(pattern_task, "pattern", 8192, NULL, 5, &s_pattern_task, 1);
	return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t pattern_stop(void)
{
	s_pattern_running = false;
	vTaskDelay(pdMS_TO_TICKS(100));
	return ESP_OK;
}
