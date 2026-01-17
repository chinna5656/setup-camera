#ifndef CAMERA_DRAWER_H
#define CAMERA_DRAWER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief Draw text on YUV422 O_UYY_E_VYY format buffer using 16x16 alpha font
	 *
	 * @param yuv YUV buffer in YUV422 O_UYY_E_VYY format
	 * @param width Buffer width in pixels
	 * @param height Buffer height in pixels
	 * @param text Text string to draw
	 * @param x X position to start drawing
	 * @param y Y position to start drawing
	 * @param y_val Y component (luma) value
	 * @param u_val U component (chroma) value
	 * @param v_val V component (chroma) value
	 */
	void draw_text(uint8_t *yuv, uint32_t width, uint32_t height, const char *text,
				   int x, int y, uint8_t y_val, uint8_t u_val, uint8_t v_val);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_DRAWER_H
