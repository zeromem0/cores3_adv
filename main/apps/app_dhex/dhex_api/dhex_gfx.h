#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DHEX_GFX_COLOR_WHITE,
    DHEX_GFX_COLOR_LIGHT,
    DHEX_GFX_COLOR_DARK,
    DHEX_GFX_COLOR_BLACK,
} dhex_gfx_color_t;

#define DHEX_GFX_GRAY_MAX 16U
#define DHEX_GFX_COLOR_GRAY_BASE 16U
#define DHEX_GFX_COLOR_GRAY_LAST (DHEX_GFX_COLOR_GRAY_BASE + DHEX_GFX_GRAY_MAX)

typedef enum {
    DHEX_GFX_FONT_SMALL,
    DHEX_GFX_FONT_MONO,
    DHEX_GFX_FONT_BOLD,
    DHEX_GFX_FONT_MONO_12,
    DHEX_GFX_FONT_MONO_14,
    DHEX_GFX_FONT_MONO_16,
    DHEX_GFX_FONT_MONO_18,
    DHEX_GFX_FONT_MONO_20,
    DHEX_GFX_FONT_BOLD_12,
    DHEX_GFX_FONT_BOLD_14,
    DHEX_GFX_FONT_BOLD_16,
    DHEX_GFX_FONT_BOLD_18,
    DHEX_GFX_FONT_BOLD_20,
    DHEX_GFX_FONT_ITALIC_12,
    DHEX_GFX_FONT_ITALIC_14,
    DHEX_GFX_FONT_ITALIC_16,
    DHEX_GFX_FONT_ITALIC_18,
    DHEX_GFX_FONT_ITALIC_20,
    DHEX_GFX_FONT_BOLD_ITALIC_12,
    DHEX_GFX_FONT_BOLD_ITALIC_14,
    DHEX_GFX_FONT_BOLD_ITALIC_16,
    DHEX_GFX_FONT_BOLD_ITALIC_18,
    DHEX_GFX_FONT_BOLD_ITALIC_20,
    DHEX_GFX_FONT_MONO_32,
    DHEX_GFX_FONT_BOLD_32,
    DHEX_GFX_FONT_PROFONT_12,     /* ProFont 6x12, full ASCII */
    DHEX_GFX_FONT_PROFONT_12_NUM, /* ProFont 6x12, digits + time punctuation only */
    DHEX_GFX_FONT_PROFONT_15,
    DHEX_GFX_FONT_PROFONT_17,
    DHEX_GFX_FONT_PROFONT_22,
    DHEX_GFX_FONT_PROFONT_29,
    /* Pixel-doubled ProFont for 2x UI scale on large panels. */
    DHEX_GFX_FONT_PROFONT_24,
    DHEX_GFX_FONT_PROFONT_30,
    DHEX_GFX_FONT_PROFONT_34,
    DHEX_GFX_FONT_PROFONT_44,
    DHEX_GFX_FONT_PROFONT_58,
    DHEX_GFX_FONT_COUNT,
} dhex_gfx_font_t;

typedef enum {
    DHEX_GFX_LINE_SOLID,
    DHEX_GFX_LINE_DOTTED,
    DHEX_GFX_LINE_DASHED,
} dhex_gfx_line_style_t;

typedef struct dhex_gfx dhex_gfx_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
} dhex_gfx_point_t;

size_t dhex_gfx_width(const dhex_gfx_t *gfx);
size_t dhex_gfx_height(const dhex_gfx_t *gfx);
dhex_gfx_color_t dhex_gfx_gray(uint8_t level);
bool dhex_gfx_color_is_valid(dhex_gfx_color_t color);
void dhex_gfx_set_color(dhex_gfx_t *gfx, dhex_gfx_color_t color);
dhex_gfx_color_t dhex_gfx_color(const dhex_gfx_t *gfx);
void dhex_gfx_set_font(dhex_gfx_t *gfx, dhex_gfx_font_t font);
dhex_gfx_font_t dhex_gfx_font(const dhex_gfx_t *gfx);
size_t dhex_gfx_text_width(dhex_gfx_t *gfx, const char *text);
void dhex_gfx_set_line_style(dhex_gfx_t *gfx, dhex_gfx_line_style_t style);
dhex_gfx_line_style_t dhex_gfx_line_style(const dhex_gfx_t *gfx);
void dhex_gfx_clear(dhex_gfx_t *gfx, dhex_gfx_color_t color);
void dhex_gfx_pixel(dhex_gfx_t *gfx, int x, int y);
void dhex_gfx_line(dhex_gfx_t *gfx, int x0, int y0, int x1, int y1);
void dhex_gfx_rect(dhex_gfx_t *gfx, int x, int y, int width, int height);
void dhex_gfx_fill_rect(dhex_gfx_t *gfx, int x, int y, int width, int height);
void dhex_gfx_fill_polygon(dhex_gfx_t *gfx,
                               const dhex_gfx_point_t *points,
                               size_t point_count);
void dhex_gfx_circle(dhex_gfx_t *gfx, int x, int y, int radius);
void dhex_gfx_fill_circle(dhex_gfx_t *gfx, int x, int y, int radius);
void dhex_gfx_text(dhex_gfx_t *gfx, int x, int baseline_y, const char *text);
void dhex_gfx_bitmap(dhex_gfx_t *gfx,
                         int x,
                         int y,
                         int width,
                         int height,
                         const uint8_t *bitmap);
bool dhex_gfx_needs_present(const dhex_gfx_t *gfx);
void dhex_gfx_present(dhex_gfx_t *gfx);

#ifdef __cplusplus
}
#endif
