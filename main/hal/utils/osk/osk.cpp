/*
 * SPDX-License-Identifier: MIT
 */
#include "osk.h"
#include "../cardkb/cardkb.h"

#include "../../hal.h"

#include <mooncake_log.h>

namespace osk {

namespace {

const std::string _tag = "osk";

/* Four rows of fourteen, the shape of the matrix this stands in for.
 * Fifty-two rows a key on a 800 wide panel gives cells of 57 by 52,
 * which is a finger rather than a stylus. */
constexpr int kRows    = 4;
constexpr int kCols    = 14;
constexpr int kKeyRows = 22;
constexpr int kBandTop = 2;  // a rule between the application and the keys

/* Where the modifiers sit in the matrix, so they can latch instead of
 * being pressed. Taken from the key table's own layout. */
constexpr int kFnRow = 2, kFnCol = 0;
constexpr int kShiftRow = 2, kShiftCol = 1;

constexpr std::uint16_t rgb(int r, int g, int b)
{
    return (std::uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr std::uint16_t kBg      = rgb(16, 16, 20);
constexpr std::uint16_t kKeyFace = rgb(44, 46, 54);
constexpr std::uint16_t kKeyEdge = rgb(70, 74, 84);
constexpr std::uint16_t kText    = rgb(226, 228, 232);
constexpr std::uint16_t kLatched = rgb(255, 138, 91);
constexpr std::uint16_t kPressed = rgb(96, 104, 120);
constexpr std::uint16_t kRule    = rgb(58, 60, 68);

bool _ready   = false;
bool _visible = true;
bool _dirty   = true;
bool _fn      = false;
bool _shift   = false;

/* The cell under the finger, so it can be drawn pressed and so the
 * release is sent for the same key the press was. */
int _held_row = -1;
int _held_col = -1;

bool _touching = false;

int key_x(int col)
{
    return col * GetHAL().display.width() / kCols;
}

int key_w(int col)
{
    return key_x(col + 1) - key_x(col);
}

void send(int row, int col)
{
    auto& keyboard = GetHAL().keyboard;

    /* Latched modifiers are held around the key exactly as fingers
     * would hold them, so everything downstream -- the raw handlers,
     * the key-code handlers, the modifier mask -- sees a sequence it
     * cannot tell from a real one. */
    if (_fn) {
        keyboard.injectKeyEventRaw({true, (uint8_t)kFnRow, (uint8_t)kFnCol});
    }
    if (_shift) {
        keyboard.injectKeyEventRaw({true, (uint8_t)kShiftRow, (uint8_t)kShiftCol});
    }

    keyboard.injectKeyEventRaw({true, (uint8_t)row, (uint8_t)col});
    keyboard.injectKeyEventRaw({false, (uint8_t)row, (uint8_t)col});

    if (_shift) {
        keyboard.injectKeyEventRaw({false, (uint8_t)kShiftRow, (uint8_t)kShiftCol});
    }
    if (_fn) {
        keyboard.injectKeyEventRaw({false, (uint8_t)kFnRow, (uint8_t)kFnCol});
    }

    /* One key each, as a physical Fn would be released after use. */
    if (_fn || _shift) {
        _fn    = false;
        _shift = false;
        _dirty = true;
    }
}

}  // namespace

int height()
{
    /* With a CardKB plugged in the band would only be taking room from
     * a 240 pixel panel, so it is not shown at all. */
    if (cardkb::isPresent()) {
        return 0;
    }
    return kBandTop + kRows * kKeyRows;
}


int top()
{
    return GetHAL().display.height() - height();
}

void init()
{
    auto& band = GetHAL().canvasOsk;

    /* Nothing to build when the band is not wanted. Asking for a sprite
     * of zero rows fails the same way running out of memory does, and
     * the error that came back said the second when it meant the
     * first. */
    if (height() <= 0) {
        _ready = false;
        return;
    }

    band.setPsram(true);
    band.setColorDepth(GetHAL().display.getColorDepth());
    _ready = band.createSprite(GetHAL().display.width(), height());
    if (!_ready) {
        mclog::tagError(_tag, "no room for the keyboard band");
        return;
    }
    _dirty = true;
}

void deinit()
{
    GetHAL().canvasOsk.deleteSprite();
    _ready = false;
}

void invalidate()
{
    _dirty = true;
}

void set_visible(bool visible)
{
    if (_visible == visible) {
        return;
    }
    _visible = visible;

    if (!_visible && _ready) {
        /* Cleared once on the way out. Whatever takes the screen next
         * draws over the rest of it, but nothing would necessarily draw
         * here. */
        GetHAL().display.fillRect(0, top(), GetHAL().display.width(), height(), kBg);
    }
    _dirty = true;
}

bool visible()
{
    return _visible && _ready;
}

void render()
{
    if (!_ready || !_visible || !_dirty) {
        return;
    }
    _dirty = false;

    auto& band     = GetHAL().canvasOsk;
    auto& keyboard = GetHAL().keyboard;

    band.fillScreen(kBg);
    band.drawFastHLine(0, 0, band.width(), kRule);
    band.setFont(&fonts::Font0);
    band.setTextDatum(middle_center);

    for (int row = 0; row < kRows; row++) {
        for (int col = 0; col < kCols; col++) {
            const int x = key_x(col);
            const int w = key_w(col);
            const int y = kBandTop + row * kKeyRows;

            const bool latched = (row == kFnRow && col == kFnCol && _fn) ||
                                 (row == kShiftRow && col == kShiftCol && _shift);
            const bool pressed = (row == _held_row && col == _held_col);

            const std::uint16_t face = latched ? kLatched : (pressed ? kPressed : kKeyFace);
            band.fillRoundRect(x + 2, y + 2, w - 4, kKeyRows - 4, 5, face);
            band.drawRoundRect(x + 2, y + 2, w - 4, kKeyRows - 4, 5, kKeyEdge);

            const char* legend = keyboard.legendAt((uint8_t)row, (uint8_t)col, _fn, _shift);
            if (legend == nullptr || legend[0] == '\0') {
                continue;
            }

            /*
             * Sized to the key rather than to taste. A cell is 57 wide
             * with four taken by the border, and Font0 is six pixels a
             * character, so at size two anything past four letters ran
             * over the edge -- which is what shift, enter and right did.
             */
            int chars = 0;
            while (legend[chars] != 0) {
                chars++;
            }
            band.setTextSize(chars == 1 ? 3 : (chars <= 4 ? 2 : 1));
            band.setTextColor(latched ? kBg : kText);
            band.drawString(legend, x + w / 2, y + kKeyRows / 2);
        }
    }

    band.setTextDatum(top_left);
    GetHAL().pushCanvasOsk();
}

void update()
{
    if (!_ready || !_visible) {
        return;
    }

    std::int32_t x = 0;
    std::int32_t y = 0;
    const bool touching = GetHAL().display.getTouch(&x, &y) > 0;

    if (touching == _touching) {
        return;
    }
    _touching = touching;

    if (!touching) {
        if (_held_row >= 0) {
            const int row = _held_row;
            const int col = _held_col;
            _held_row = -1;
            _held_col = -1;
            _dirty    = true;

            /* Fn and shift latch on release rather than sending
             * anything: they change what the next key means, and what
             * this band shows. */
            if (row == kFnRow && col == kFnCol) {
                _fn    = !_fn;
                _shift = false;
            } else if (row == kShiftRow && col == kShiftCol) {
                _shift = !_shift;
                _fn    = false;
            } else {
                send(row, col);
            }
        }
        return;
    }

    const int band_top = top();
    if (y < band_top) {
        return;  // above the band belongs to touch_keys
    }

    const int row = (y - band_top - kBandTop) / kKeyRows;
    const int col = (int)x * kCols / GetHAL().display.width();
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) {
        return;
    }

    _held_row = row;
    _held_col = col;
    _dirty    = true;
}

}  // namespace osk
