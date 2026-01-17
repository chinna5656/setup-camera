#ifndef CAMERA_H
#define CAMERA_H

#include "esp_err.h"
#include "linux/videodev2.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIDEO_FMT_RGB565 = V4L2_PIX_FMT_RGB565,
    VIDEO_FMT_YUV420 = V4L2_PIX_FMT_YUV420,
} video_fmt_t;

typedef void (*camera_frame_cb_t)(uint8_t *buf, uint8_t idx,
                                  uint32_t w, uint32_t h, size_t len);

esp_err_t camera_init(void);
int camera_open(video_fmt_t fmt);
esp_err_t camera_setup_buffers(int fd);
esp_err_t camera_start(int fd, int core, camera_frame_cb_t cb);
esp_err_t camera_stop(int fd);
uint32_t camera_get_width(void);
uint32_t camera_get_height(void);

#ifdef __cplusplus
}
#endif

#endif
