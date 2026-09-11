/*
 * Graphics surface implemented on an M5GFX sprite.
 *
 * The drawing model targets monochrome panels: a current
 * colour, a current font and a current line style, with immediate-mode
 * primitives drawn in whatever the current colour is. Its four named
 * colours map onto a grey ramp here, which reads correctly on a colour
 * TFT for both light and dark application themes.
 *
 * Fonts are matched by nominal pixel height against the fonts M5GFX
 * ships. The original bitmap fonts are not carried over: applications
 * only ever name a font through the dhex_gfx_font_t enum, so the
 * substitution is invisible to them beyond exact glyph metrics.
 */
#include "dhex_gfx_lgfx.h"

#include <cstring>

struct dhex_gfx {
    LGFX_Sprite* sprite;
    std::function<void()> present;
    dhex_gfx_color_t color;
    dhex_gfx_font_t font;
    dhex_gfx_line_style_t line_style;
};

namespace {

constexpr uint32_t kColorWhite = 0xFFFFFFU;
constexpr uint32_t kColorLight = 0xAAAAAAU;
constexpr uint32_t kColorDark  = 0x555555U;
constexpr uint32_t kColorBlack = 0x000000U;

uint32_t resolve_color(dhex_gfx_color_t color)
{
    if (color >= DHEX_GFX_COLOR_GRAY_BASE && color <= DHEX_GFX_COLOR_GRAY_LAST) {
        // Gray level 0 is black, DHEX_GFX_GRAY_MAX is white.
        const uint32_t level = static_cast<uint32_t>(color) - DHEX_GFX_COLOR_GRAY_BASE;
        const uint32_t value = (level * 255U) / DHEX_GFX_GRAY_MAX;
        return (value << 16) | (value << 8) | value;
    }

    switch (color) {
        case DHEX_GFX_COLOR_WHITE:
            return kColorWhite;
        case DHEX_GFX_COLOR_LIGHT:
            return kColorLight;
        case DHEX_GFX_COLOR_DARK:
            return kColorDark;
        case DHEX_GFX_COLOR_BLACK:
        default:
            return kColorBlack;
    }
}

// The largest font M5GFX ships tops out well below the biggest sizes
// the API names, so those are reached by rendering a smaller font at an
// integer scale.
uint8_t resolve_font_scale(dhex_gfx_font_t font)
{
    return font == DHEX_GFX_FONT_PROFONT_58 ? 2 : 1;
}

const lgfx::IFont* resolve_font(dhex_gfx_font_t font)
{
    switch (font) {
        // Compact grid work. Font0 is 6x8, the narrowest fixed-width font
        // available, which is what keeps hex/ascii columns on a 204 px wide
        // canvas from running off the right edge.
        case DHEX_GFX_FONT_SMALL:
        case DHEX_GFX_FONT_MONO:
        case DHEX_GFX_FONT_MONO_12:
        case DHEX_GFX_FONT_MONO_14:
        case DHEX_GFX_FONT_PROFONT_12:
        case DHEX_GFX_FONT_PROFONT_12_NUM:
        case DHEX_GFX_FONT_PROFONT_15:
            return &fonts::Font0;

        case DHEX_GFX_FONT_MONO_16:
        case DHEX_GFX_FONT_MONO_18:
        case DHEX_GFX_FONT_PROFONT_17:
            return &fonts::FreeMono9pt7b;
        case DHEX_GFX_FONT_MONO_20:
        case DHEX_GFX_FONT_PROFONT_22:
        case DHEX_GFX_FONT_PROFONT_24:
            return &fonts::FreeMono12pt7b;
        case DHEX_GFX_FONT_MONO_32:
        case DHEX_GFX_FONT_PROFONT_29:
        case DHEX_GFX_FONT_PROFONT_30:
        case DHEX_GFX_FONT_PROFONT_34:
            return &fonts::FreeMono18pt7b;
        case DHEX_GFX_FONT_PROFONT_44:
            return &fonts::FreeMono24pt7b;
        // Rendered at 2x by resolve_font_scale(), so this lands taller than
        // FreeMono24pt7b does at 1x and the size ordering still holds.
        case DHEX_GFX_FONT_PROFONT_58:
            return &fonts::FreeMono18pt7b;

        case DHEX_GFX_FONT_BOLD:
        case DHEX_GFX_FONT_BOLD_12:
        case DHEX_GFX_FONT_BOLD_14:
            return &fonts::Font2;
        case DHEX_GFX_FONT_BOLD_16:
        case DHEX_GFX_FONT_BOLD_18:
            return &fonts::FreeSansBold9pt7b;
        case DHEX_GFX_FONT_BOLD_20:
            return &fonts::FreeSansBold12pt7b;
        case DHEX_GFX_FONT_BOLD_32:
            return &fonts::FreeMonoBold18pt7b;

        case DHEX_GFX_FONT_ITALIC_12:
        case DHEX_GFX_FONT_ITALIC_14:
        case DHEX_GFX_FONT_ITALIC_16:
        case DHEX_GFX_FONT_ITALIC_18:
            return &fonts::FreeSans9pt7b;
        case DHEX_GFX_FONT_ITALIC_20:
            return &fonts::FreeSans12pt7b;

        case DHEX_GFX_FONT_BOLD_ITALIC_12:
        case DHEX_GFX_FONT_BOLD_ITALIC_14:
        case DHEX_GFX_FONT_BOLD_ITALIC_16:
        case DHEX_GFX_FONT_BOLD_ITALIC_18:
            return &fonts::FreeSansBold9pt7b;
        case DHEX_GFX_FONT_BOLD_ITALIC_20:
            return &fonts::FreeSansBold12pt7b;

        default:
            return &fonts::Font0;
    }
}

void apply_color(dhex_gfx_t* gfx)
{
    const uint32_t rgb = resolve_color(gfx->color);
    gfx->sprite->setColor(rgb);
    gfx->sprite->setTextColor(gfx->sprite->color888((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
}

// Dotted and dashed lines go through the same entry point as
// solid ones, so the pattern is walked here rather than by the caller.
void draw_patterned_line(dhex_gfx_t* gfx, int x0, int y0, int x1, int y1, int on, int off)
{
    const int dx  = abs(x1 - x0);
    const int dy  = -abs(y1 - y0);
    const int sx  = x0 < x1 ? 1 : -1;
    const int sy  = y0 < y1 ? 1 : -1;
    int err       = dx + dy;
    int step      = 0;
    const int period = on + off;

    while (true) {
        if ((step % period) < on) {
            gfx->sprite->drawPixel(x0, y0);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int err2 = 2 * err;
        if (err2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (err2 <= dx) {
            err += dx;
            y0 += sy;
        }
        step++;
    }
}

}  // namespace

dhex_gfx_t* dhex_host_gfx_create(LGFX_Sprite* sprite, std::function<void()> present)
{
    if (sprite == nullptr) {
        return nullptr;
    }

    auto* gfx       = new dhex_gfx();
    gfx->sprite     = sprite;
    gfx->present    = std::move(present);
    gfx->color      = DHEX_GFX_COLOR_BLACK;
    gfx->font       = DHEX_GFX_FONT_SMALL;
    gfx->line_style = DHEX_GFX_LINE_SOLID;

    // Text is positioned by its baseline, not its top-left corner.
    sprite->setTextDatum(lgfx::textdatum_t::baseline_left);
    sprite->setTextSize(resolve_font_scale(gfx->font));
    sprite->setFont(resolve_font(gfx->font));
    apply_color(gfx);

    return gfx;
}

void dhex_host_gfx_destroy(dhex_gfx_t* gfx)
{
    delete gfx;
}

extern "C" {

size_t dhex_gfx_width(const dhex_gfx_t* gfx)
{
    return gfx != nullptr ? static_cast<size_t>(gfx->sprite->width()) : 0U;
}

size_t dhex_gfx_height(const dhex_gfx_t* gfx)
{
    return gfx != nullptr ? static_cast<size_t>(gfx->sprite->height()) : 0U;
}

dhex_gfx_color_t dhex_gfx_gray(uint8_t level)
{
    if (level > DHEX_GFX_GRAY_MAX) {
        level = DHEX_GFX_GRAY_MAX;
    }
    return static_cast<dhex_gfx_color_t>(DHEX_GFX_COLOR_GRAY_BASE + level);
}

bool dhex_gfx_color_is_valid(dhex_gfx_color_t color)
{
    return color <= DHEX_GFX_COLOR_BLACK ||
           (color >= DHEX_GFX_COLOR_GRAY_BASE && color <= DHEX_GFX_COLOR_GRAY_LAST);
}

void dhex_gfx_set_color(dhex_gfx_t* gfx, dhex_gfx_color_t color)
{
    if (gfx == nullptr || !dhex_gfx_color_is_valid(color)) {
        return;
    }
    gfx->color = color;
    apply_color(gfx);
}

dhex_gfx_color_t dhex_gfx_color(const dhex_gfx_t* gfx)
{
    return gfx != nullptr ? gfx->color : DHEX_GFX_COLOR_BLACK;
}

void dhex_gfx_set_font(dhex_gfx_t* gfx, dhex_gfx_font_t font)
{
    if (gfx == nullptr || font >= DHEX_GFX_FONT_COUNT) {
        return;
    }
    gfx->font = font;
    gfx->sprite->setFont(resolve_font(font));
    gfx->sprite->setTextDatum(lgfx::textdatum_t::baseline_left);
    gfx->sprite->setTextSize(resolve_font_scale(font));
}

dhex_gfx_font_t dhex_gfx_font(const dhex_gfx_t* gfx)
{
    return gfx != nullptr ? gfx->font : DHEX_GFX_FONT_SMALL;
}

size_t dhex_gfx_text_width(dhex_gfx_t* gfx, const char* text)
{
    if (gfx == nullptr || text == nullptr) {
        return 0U;
    }
    return static_cast<size_t>(gfx->sprite->textWidth(text));
}

void dhex_gfx_set_line_style(dhex_gfx_t* gfx, dhex_gfx_line_style_t style)
{
    if (gfx != nullptr) {
        gfx->line_style = style;
    }
}

dhex_gfx_line_style_t dhex_gfx_line_style(const dhex_gfx_t* gfx)
{
    return gfx != nullptr ? gfx->line_style : DHEX_GFX_LINE_SOLID;
}

void dhex_gfx_clear(dhex_gfx_t* gfx, dhex_gfx_color_t color)
{
    if (gfx == nullptr) {
        return;
    }
    gfx->sprite->fillSprite(resolve_color(color));
    apply_color(gfx);
}

void dhex_gfx_pixel(dhex_gfx_t* gfx, int x, int y)
{
    if (gfx != nullptr) {
        gfx->sprite->drawPixel(x, y);
    }
}

void dhex_gfx_line(dhex_gfx_t* gfx, int x0, int y0, int x1, int y1)
{
    if (gfx == nullptr) {
        return;
    }
    switch (gfx->line_style) {
        case DHEX_GFX_LINE_DOTTED:
            draw_patterned_line(gfx, x0, y0, x1, y1, 1, 2);
            break;
        case DHEX_GFX_LINE_DASHED:
            draw_patterned_line(gfx, x0, y0, x1, y1, 4, 3);
            break;
        case DHEX_GFX_LINE_SOLID:
        default:
            gfx->sprite->drawLine(x0, y0, x1, y1);
            break;
    }
}

void dhex_gfx_rect(dhex_gfx_t* gfx, int x, int y, int width, int height)
{
    if (gfx != nullptr) {
        gfx->sprite->drawRect(x, y, width, height);
    }
}

void dhex_gfx_fill_rect(dhex_gfx_t* gfx, int x, int y, int width, int height)
{
    if (gfx != nullptr) {
        gfx->sprite->fillRect(x, y, width, height);
    }
}

void dhex_gfx_fill_polygon(dhex_gfx_t* gfx,
                               const dhex_gfx_point_t* points,
                               size_t point_count)
{
    if (gfx == nullptr || points == nullptr || point_count < 3U) {
        return;
    }
    // M5GFX only offers triangle fills, so the polygon is drawn as a fan
    // from its first vertex. Correct for the convex shapes applications use.
    for (size_t i = 1; i + 1 < point_count; ++i) {
        gfx->sprite->fillTriangle(points[0].x, points[0].y,
                                  points[i].x, points[i].y,
                                  points[i + 1].x, points[i + 1].y);
    }
}

void dhex_gfx_circle(dhex_gfx_t* gfx, int x, int y, int radius)
{
    if (gfx != nullptr) {
        gfx->sprite->drawCircle(x, y, radius);
    }
}

void dhex_gfx_fill_circle(dhex_gfx_t* gfx, int x, int y, int radius)
{
    if (gfx != nullptr) {
        gfx->sprite->fillCircle(x, y, radius);
    }
}

void dhex_gfx_text(dhex_gfx_t* gfx, int x, int baseline_y, const char* text)
{
    if (gfx == nullptr || text == nullptr) {
        return;
    }
    gfx->sprite->drawString(text, x, baseline_y);
}

void dhex_gfx_bitmap(dhex_gfx_t* gfx,
                         int x,
                         int y,
                         int width,
                         int height,
                         const uint8_t* bitmap)
{
    if (gfx == nullptr || bitmap == nullptr) {
        return;
    }
    // Bitmaps are 1 bit per pixel, MSB first, rows padded to whole
    // bytes. Set bits are drawn in the current colour, clear bits are left
    // untouched.
    const int stride = (width + 7) / 8;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const uint8_t byte = bitmap[row * stride + (col / 8)];
            if ((byte >> (7 - (col % 8))) & 0x01U) {
                gfx->sprite->drawPixel(x + col, y + row);
            }
        }
    }
}

bool dhex_gfx_needs_present(const dhex_gfx_t* gfx)
{
    return gfx != nullptr;
}

void dhex_gfx_present(dhex_gfx_t* gfx)
{
    if (gfx != nullptr && gfx->present) {
        gfx->present();
    }
}

}  // extern "C"
