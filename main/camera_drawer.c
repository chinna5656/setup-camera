#include "camera_drawer.h"
#include <stdio.h>
#include <string.h>

// Include the 16x16 alpha font
extern const uint8_t font_16x12_alpha[64][16][16];

/**
 * @brief Map ASCII character to font index
 * Character set: ' 0-9 A-Z a-z #' (64 chars total)
 */
static int char_to_font_index(char c)
{
	if (c == ' ')
		return 0;
	if (c >= '0' && c <= '9')
		return c - '0' + 1;
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 11;
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 37;
	if (c == '#')
		return 63;
	return 0;
}

void draw_text(uint8_t *yuv, uint32_t width, uint32_t height, const char *text,
			   int x, int y, uint8_t y_val, uint8_t u_val, uint8_t v_val)
{
	uint32_t row_stride = (width / 2) * 3;

	for (int i = 0; text[i] != '\0'; i++)
	{
		int font_idx = char_to_font_index(text[i]);
		const uint8_t (*char_bitmap)[16] = font_16x12_alpha[font_idx];

		// Draw 16x16 character with alpha blending
		for (int row = 0; row < 16; row++)
		{
			for (int col = 0; col < 16; col++)
			{
				uint8_t alpha = char_bitmap[row][col];
				if (alpha > 0)
				{
					int px = x + i * 10 + col;
					int py = y + row;

					if (px >= 0 && px < (int)width && py >= 0 && py < (int)height)
					{
						uint32_t macropixel_idx = (px / 2) * 3;
						uint32_t y_offset = py * row_stride + macropixel_idx + 1 + (px % 2);

						if (y_offset < height * row_stride)
						{
							// Alpha blend Y value
							yuv[y_offset] = (y_val * alpha + yuv[y_offset] * (255 - alpha)) / 255;
						}
					}
				}
			}
		}
	}
}