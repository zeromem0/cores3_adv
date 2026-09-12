/*
 * SPDX-License-Identifier: MIT
 */
#include "app_header.h"

#include <hal.h>

namespace app_header {

namespace {

bool _was_touching = false;

/* aprecio's numbers, which is where this band comes from: the title on a
 * 24 baseline, the rule ten rows under it. */
constexpr int kRuleY  = 34;
constexpr int kMargin = 6;

constexpr int kBackW = 96;
constexpr int kBackH = 30;
constexpr int kBackY = 2;

constexpr std::uint32_t kRule  = 0x555555;
constexpr std::uint32_t kTitle = 0xFFFFFF;

/* The band paints its own ground rather than inheriting each screen's.
 * Three of them are grey and five are black, and a row of screenshots
 * side by side made that look like an oversight rather than a choice. */
constexpr std::uint32_t kBand = 0x000000;

}  // namespace

int height()
{
    return kRuleY;
}

void back_rect(int width, int& x, int& y, int& w, int& h)
{
    x = width - kMargin - kBackW;
    y = kBackY;
    w = kBackW;
    h = kBackH;
}

int back_left(int width)
{
    return width - kMargin - kBackW;
}

void draw_title(lgfx::LovyanGFX& target, const char* title, int offset_y)
{
    /* Every piece of state this touches is set, none of it assumed. A
     * proportional face is still multiplied by the text size, so a
     * screen that left it at three -- and they do -- would have drawn
     * this title three times the size it is meant to be. */
    target.setFont(&fonts::FreeSansBold12pt7b);
    target.setTextSize(1);
    target.setTextColor(kTitle);
    target.setTextDatum(lgfx::textdatum_t::baseline_left);
    target.drawString(title, kMargin, offset_y + 24);

    target.setTextDatum(lgfx::textdatum_t::top_left);
    target.setFont(&fonts::Font0);
}

void draw_back(lgfx::LovyanGFX& target, int offset_y)
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    back_rect(target.width(), x, y, w, h);
    y += offset_y;

    target.drawRoundRect(x, y, w, h, 6, kTitle);
    target.setFont(&fonts::Font0);
    target.setTextSize(2);
    target.setTextColor(kTitle);
    target.setTextDatum(lgfx::textdatum_t::middle_center);
    target.drawString("< BACK", x + w / 2, y + h / 2);

    target.setTextDatum(lgfx::textdatum_t::top_left);
    target.setTextSize(1);
}

void draw(lgfx::LovyanGFX& target, const char* title, int offset_y)
{
    const int width = target.width();

    target.fillRect(0, offset_y, width, kRuleY, kBand);
    draw_title(target, title, offset_y);

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    back_rect(width, x, y, w, h);
    y += offset_y;

    target.drawRoundRect(x, y, w, h, 6, kTitle);
    target.setFont(&fonts::Font0);
    target.setTextSize(2);
    target.setTextDatum(lgfx::textdatum_t::middle_center);
    target.drawString("< BACK", x + w / 2, y + h / 2);

    target.drawLine(0, offset_y + kRuleY, width - 1, offset_y + kRuleY, kRule);

    /* Left as it was found: applications draw straight after this and
     * were written against the library's own starting state. */
    target.setTextDatum(lgfx::textdatum_t::top_left);
    target.setTextSize(1);
}

bool back_tapped(int x, int y, int width, int offset_x, int offset_y)
{
    int bx = 0;
    int by = 0;
    int bw = 0;
    int bh = 0;
    back_rect(width, bx, by, bw, bh);

    bx += offset_x;
    by += offset_y;

    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

bool back_pressed_at(bool touching, int x, int y, int width, int offset_x, int offset_y)
{
    const bool pressed = touching && !_was_touching && back_tapped(x, y, width, offset_x, offset_y);
    _was_touching      = touching;
    return pressed;
}

bool back_pressed(int width, int offset_x, int offset_y)
{
    std::int32_t x      = 0;
    std::int32_t y      = 0;
    const bool touching = GetHAL().display.getTouch(&x, &y) > 0;

    return back_pressed_at(touching, (int)x, (int)y, width, offset_x, offset_y);
}

void reset()
{
    /* Taken as down rather than up: an application opened by a tap is
     * opened while the finger is still on the glass, and treating that
     * as a fresh press would close it again in the same breath. */
    _was_touching = true;
}

}  // namespace app_header
