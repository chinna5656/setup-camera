#include "camera.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

static const char *TAG = "camera";

#define CAM_DEV_PATH ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAM_BUF_COUNT 2
#define CAM_WIDTH 1920
#define CAM_HEIGHT 1080

typedef struct
{
	int fd;
	uint8_t *buffers[CAM_BUF_COUNT];
	size_t buf_size;
	camera_frame_cb_t callback;
	TaskHandle_t task_handle;
	bool running;
} camera_t;

static camera_t s_cam = {0};

esp_err_t camera_init(void)
{
	ESP_LOGI(TAG, "Initializing video subsystem with CSI camera...");

#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
	// Configure CSI camera sensor interface
	esp_video_init_csi_config_t csi_config = {
		.sccb_config = {
			.init_sccb = true,
			.i2c_config = {
				.port = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
				.scl_pin = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
				.sda_pin = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
			},
			.freq = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ,
		},
		.reset_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN,
		.pwdn_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
	};

	esp_video_init_config_t cfg = {
		.csi = &csi_config,
	};
#else
	esp_video_init_config_t cfg = {0};
#endif

	esp_err_t ret = esp_video_init(&cfg);
	if (ret != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_video_init failed: %s (0x%x)", esp_err_to_name(ret), ret);
		return ret;
	}
	ESP_LOGI(TAG, "Video subsystem initialized successfully");
	return ESP_OK;
}

int camera_open(video_fmt_t fmt)
{
	struct v4l2_capability capability;
	struct v4l2_format current_format;

	ESP_LOGI(TAG, "Opening camera device: %s", CAM_DEV_PATH);
	s_cam.fd = open(CAM_DEV_PATH, O_RDONLY | O_NONBLOCK);
	if (s_cam.fd < 0)
	{
		ESP_LOGE(TAG, "Failed to open camera %s: %s (errno=%d)", CAM_DEV_PATH, strerror(errno), errno);
		return -1;
	}

	// Query camera capabilities
	if (ioctl(s_cam.fd, VIDIOC_QUERYCAP, &capability) == 0)
	{
		ESP_LOGI(TAG, "Camera detected:");
		ESP_LOGI(TAG, "  Driver:  %s", capability.driver);
		ESP_LOGI(TAG, "  Card:    %s", capability.card);
		ESP_LOGI(TAG, "  Bus:     %s", capability.bus_info);
		ESP_LOGI(TAG, "  Version: %d.%d.%d",
				 (uint16_t)(capability.version >> 16),
				 (uint8_t)(capability.version >> 8),
				 (uint8_t)capability.version);
	}
	else
	{
		ESP_LOGE(TAG, "Failed to query camera capabilities");
		close(s_cam.fd);
		return -1;
	}

	// Get current format
	memset(&current_format, 0, sizeof(current_format));
	current_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(s_cam.fd, VIDIOC_G_FMT, &current_format) == 0)
	{
		ESP_LOGI(TAG, "Camera native resolution: %" PRIu32 "x%" PRIu32,
				 current_format.fmt.pix.width, current_format.fmt.pix.height);

		// Store actual dimensions
		s_cam.buf_size = current_format.fmt.pix.sizeimage;
	}
	else
	{
		ESP_LOGE(TAG, "Failed to get current format");
		close(s_cam.fd);
		return -1;
	}

	// Set desired format
	struct v4l2_format format = {0};
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = CAM_WIDTH;
	format.fmt.pix.height = CAM_HEIGHT;
	format.fmt.pix.pixelformat = fmt;

	if (ioctl(s_cam.fd, VIDIOC_S_FMT, &format) != 0)
	{
		ESP_LOGE(TAG, "Failed to set format");
		close(s_cam.fd);
		return -1;
	}

	ESP_LOGI(TAG, "Camera configured: %dx%d fmt=%c%c%c%c",
			 CAM_WIDTH, CAM_HEIGHT,
			 fmt & 0xFF, (fmt >> 8) & 0xFF, (fmt >> 16) & 0xFF, (fmt >> 24) & 0xFF);

	return s_cam.fd;
}

esp_err_t camera_setup_buffers(int fd)
{
	struct v4l2_requestbuffers req = {0};
	req.count = CAM_BUF_COUNT;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0)
	{
		ESP_LOGE(TAG, "REQBUFS failed");
		return ESP_FAIL;
	}

	for (int i = 0; i < CAM_BUF_COUNT; i++)
	{
		struct v4l2_buffer buf = {0};
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0)
		{
			ESP_LOGE(TAG, "QUERYBUF failed");
			return ESP_FAIL;
		}

		s_cam.buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
								MAP_SHARED, fd, buf.m.offset);
		if (s_cam.buffers[i] == MAP_FAILED)
		{
			ESP_LOGE(TAG, "mmap failed");
			return ESP_FAIL;
		}

		s_cam.buf_size = buf.length;

		if (ioctl(fd, VIDIOC_QBUF, &buf) != 0)
		{
			ESP_LOGE(TAG, "QBUF failed");
			return ESP_FAIL;
		}
	}

	return ESP_OK;
}

static void camera_task(void *arg)
{
	struct v4l2_buffer buf = {0};

	ESP_LOGI(TAG, "Camera task running");

	while (s_cam.running)
	{
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		int ret = ioctl(s_cam.fd, VIDIOC_DQBUF, &buf);
		if (ret != 0)
		{
			if (errno == EAGAIN)
			{
				vTaskDelay(pdMS_TO_TICKS(1));
				continue;
			}
			ESP_LOGE(TAG, "DQBUF failed: errno %d", errno);
			break;
		}

		if (s_cam.callback)
		{
			s_cam.callback(s_cam.buffers[buf.index], buf.index,
						   CAM_WIDTH, CAM_HEIGHT, s_cam.buf_size);
		}

		ioctl(s_cam.fd, VIDIOC_QBUF, &buf);
	}

	ESP_LOGI(TAG, "Camera task exiting");
	s_cam.task_handle = NULL;
	vTaskDelete(NULL);
}

esp_err_t camera_start(int fd, int core, camera_frame_cb_t cb)
{
	if (s_cam.running)
	{
		return ESP_OK;
	}

	s_cam.callback = cb;
	s_cam.running = true;

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) != 0)
	{
		ESP_LOGE(TAG, "STREAMON failed");
		s_cam.running = false;
		return ESP_FAIL;
	}

	BaseType_t ret = xTaskCreatePinnedToCore(camera_task, "camera", 4096,
											 NULL, 5, &s_cam.task_handle, core);
	if (ret != pdPASS)
	{
		ESP_LOGE(TAG, "Failed to create camera task");
		s_cam.running = false;
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Camera started");
	return ESP_OK;
}

esp_err_t camera_stop(int fd)
{
	s_cam.running = false;

	if (s_cam.task_handle)
	{
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ioctl(fd, VIDIOC_STREAMOFF, &type);

	ESP_LOGI(TAG, "Camera stopped");
	return ESP_OK;
}
