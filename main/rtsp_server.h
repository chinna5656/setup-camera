#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    RTSP_STATE_INIT,
    RTSP_STATE_READY,
    RTSP_STATE_PLAYING,
    RTSP_STATE_TEARDOWN
} rtsp_state_t;

esp_err_t rtsp_server_init(void);
esp_err_t rtsp_server_start(void);
void rtsp_server_stop(void);
esp_err_t rtsp_send_h264_frame(const uint8_t *data, size_t len, uint32_t timestamp);
esp_err_t rtsp_set_sps_pps(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len);

#endif
