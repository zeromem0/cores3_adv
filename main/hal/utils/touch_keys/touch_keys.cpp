/*
 * SPDX-License-Identifier: MIT
 */
#include "touch_keys.h"
#include "../osk/osk.h"
#include <hal/hal.h>
#include <mooncake_log.h>

namespace touch_keys {

namespace {

const std::string _tag = "touch_keys";

/* Positions on the Cardputer's matrix, which is what the applications
 * were written against. Fn is at (2, 0) and turns ` into esc and the
 * ;,./ cluster into the arrows. */
constexpr uint8_t kFnRow = 2;
constexpr uint8_t kFnCol = 0;

struct Chord {
    uint8_t row;
    uint8_t col;
    bool with_fn;
};

constexpr Chord kNone  = {0xff, 0xff, false};
constexpr Chord kEsc   = {0, 0, true};
constexpr Chord kUp    = {2, 11, true};
constexpr Chord kDown  = {3, 11, true};
constexpr Chord kLeft  = {3, 10, true};
constexpr Chord kRight = {3, 12, true};
constexpr Chord kEnter = {2, 13, false};
constexpr Chord kSpace = {3, 13, false};

/*
 * Row-major, three columns wide. Space sits bottom right, under the
 * hand that is already there, because Doom is unplayable without a fire
 * key and the cross leaves that corner free. It is a plain space
 * elsewhere, which is harmless.
 *
 * The remaining two corners stay dead on purpose: a stray touch should
 * do nothing rather than something surprising.
 */
constexpr Chord kCells[3][3] = {
    {kEsc, kUp, kNone},
    {kLeft, kEnter, kRight},
    {kNone, kDown, kSpace},
};

bool _is_touching     = false;
Chord _held           = kNone;
bool _suspended       = false;
std::uint32_t _next_poll_ms = 0;

/* The main loop spins as fast as it can; polling the controller on
 * every pass would put continuous traffic on the I2C bus for no gain,
 * since no finger moves meaningfully in under 20ms. */
constexpr std::uint32_t kPollIntervalMs = 20;

bool is_none(const Chord& chord)
{
    return chord.row == kNone.row && chord.col == kNone.col;
}

void send(const Chord& chord, bool state)
{
    if (is_none(chord)) {
        return;
    }

    /* Press order is Fn first then the key, release order is the
     * reverse -- the same order a hand produces, and the order the
     * modifier mask expects if it is to be right at the moment the key
     * event is converted. */
    if (chord.with_fn && state) {
        GetHAL().keyboard.injectKeyEventRaw({true, kFnRow, kFnCol});
    }

    GetHAL().keyboard.injectKeyEventRaw({state, chord.row, chord.col});

    if (chord.with_fn && !state) {
        GetHAL().keyboard.injectKeyEventRaw({false, kFnRow, kFnCol});
    }
}

}  // namespace

void suspend()
{
    /* Any key still down belongs to a finger that the application will
     * never see lift, so it is released before handing over. */
    if (_is_touching) {
        send(_held, false);
        _held        = kNone;
        _is_touching = false;
    }
    _suspended = true;
}

void resume()
{
    _suspended = false;
}

void update()
{
    if (_suspended) {
        return;
    }

    const auto now = GetHAL().millis();
    if (now < _next_poll_ms) {
        return;
    }
    _next_poll_ms = now + kPollIntervalMs;

    std::int32_t x = 0;
    std::int32_t y = 0;
    const bool touching = GetHAL().display.getTouch(&x, &y) > 0;

    if (touching == _is_touching) {
        return;
    }
    _is_touching = touching;

    if (!touching) {
        send(_held, false);
        _held = kNone;
        return;
    }

    /* The keyboard band owns the bottom of the panel. Without this a
     * tap on a key would also land in the bottom row of cells and send
     * a chord alongside the keystroke. */
    if (y >= osk::top()) {
        _is_touching = false;
        return;
    }

    const auto width  = GetHAL().display.width();
    const auto height = GetHAL().display.height();
    if (width <= 0 || height <= 0) {
        return;
    }

    /* Clamped because a touch reported exactly at the far edge would
     * otherwise index one cell past the end. */
    auto cx = static_cast<int>(x) * 3 / width;
    auto cy = static_cast<int>(y) * 3 / height;
    cx      = cx < 0 ? 0 : (cx > 2 ? 2 : cx);
    cy      = cy < 0 ? 0 : (cy > 2 ? 2 : cy);

    _held = kCells[cy][cx];
    mclog::tagDebug(_tag, "touch {},{} -> cell {},{}", (int)x, (int)y, cx, cy);
    send(_held, true);
}

}  // namespace touch_keys
