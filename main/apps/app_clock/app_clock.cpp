/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_clock.h"
#include "assets/timer_big.h"
#include "assets/timer_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal/utils/jobs/jobs.h>
#include <hal/utils/timed/timed.h>
#include <apps/utils/app_header/app_header.h>
#include <hal/utils/touch_keys/touch_keys.h>
#include <mooncake_log.h>

#include <sys/time.h>

#include <cstdio>

using namespace mooncake;

namespace {

/* The irrigation controller's palette, because that is the other screen
 * on this board meant to be read from across a room. */
constexpr std::uint16_t kBg     = 0x0000;
constexpr std::uint16_t kText   = 0xFFFF;
constexpr std::uint16_t kActive = 0xFC00;
constexpr std::uint16_t kDim    = 0x8410;

struct Rect_t {
    int x;
    int y;
    int w;
    int h;
};

enum Button_t {
    /* Left to right as they are read, and as a watch has them: walk the
     * fields, step the one you are on, commit, and the one that talks to
     * the world outside the chip.
     *
     * Leaving is not among them any more: it lives in the corner of the
     * band across the top, the same one every other screen on this board
     * carries. It had to be a button here while there was no band, since
     * the nine touch cells that stand in for Escape elsewhere are
     * suspended on this screen -- their middle row sits exactly where
     * this one does. */
    kBtnPrev = 0,
    kBtnNext,
    kBtnPlus,
    kBtnMinus,
    kBtnSet,
    kBtnSync,
    kButtonCount,
};

const char* const kLabels[kButtonCount] = {"PREV", "NEXT", "+1", "-1", "SET", "SYNC"};

/* Where each field sits in "YYYY-MM-DD HH:MM:SS", in characters. The
 * stamp is drawn as one string with the selection underlined beneath it,
 * so the two have to agree about the columns. Six, in the order the
 * application walks them. */
constexpr int kFieldCount = 6;

struct Span_t {
    int at;
    int len;
};
const Span_t kSpans[kFieldCount] = {{0, 4}, {5, 2}, {8, 2}, {11, 2}, {14, 2}, {17, 2}};

/* Sized from the panel rather than written down, since it is written
 * down once here and read on boards of two sizes. */
int ui_scale(int width)
{
    return width >= 640 ? 2 : 1;
}

/* One definition of where the buttons are. Drawing and hit testing both
 * call it, so a highlight cannot end up next to the thing it highlights. */
Rect_t button_rect(int index, int width, int height, int scale)
{
    const int margin = 6 * scale;
    const int gap    = margin / 2;
    const int h      = 22 * scale;
    const int w      = (width - 2 * margin - (kButtonCount - 1) * gap) / kButtonCount;

    return {margin + index * (w + gap), height - margin - h, w, h};
}

int days_in_month(int year, int month)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

}  // namespace

AppClock::AppClock()
{
    setAppInfo().name     = "Clock";
    setAppInfo().userData = new AppIcon_t(image_data_timer_big, image_data_timer_small);
}

AppClock::~AppClock()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppClock::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    /*
     * The zone, in case nothing has set it yet. It is normally applied
     * when the network comes up, which means a board that has never
     * joined one would show -- and, worse, set -- the time in UTC.
     * Applying it here costs nothing and is what makes setting the clock
     * by hand, with no network at all, land on the right instant.
     */
    setenv("TZ", timed::timezone_rule(), 1);
    tzset();

    _close_requested = false;
    _editing         = false;
    _dirty           = true;
    _redraw_ms       = 0;
    _notice_until_ms = 0;
    _was_touching    = false;
    app_header::reset();

    _key_slot = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& event) { handle_key(event); });
}

void AppClock::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        audio::play_random_tone();
        close();
        return;
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    /*
     * This screen owns the glass.
     *
     * The panel is otherwise divided into nine cells that stand in for a
     * keyboard, and the middle band of them sends left, enter and right
     * -- which is exactly where this row of buttons sits, and exactly
     * what this application does something with. PREV took its own step
     * and the injected left arrow's as well; NEXT took one forward and
     * that same arrow's one back, and so never moved at all.
     *
     * Held down here rather than asked for once at open: the launcher
     * resumes the cells on the transition out of the desktop, which
     * happens after onOpen, so a request made there would be undone a
     * pass later.
     */
    touch_keys::suspend();

    /* One reading for the pass, shared: the controller is emptied by
     * being read, and a second ask can answer "nobody is touching" with
     * the finger still down. */
    std::int32_t tx     = 0;
    std::int32_t ty     = 0;
    const bool touching = GetHAL().display.getTouch(&tx, &ty) > 0;

    if (app_header::back_pressed_at(touching, (int)tx, (int)ty, GetHAL().canvas.width(),
                                    GetHAL().canvasKeyboardBar.width(), 0)) {
        _close_requested = true;
        return;
    }

    handle_touch(touching, (int)tx, (int)ty);

    const std::uint32_t now = GetHAL().millis();

    /* An edit nobody has touched for half a minute gives the screen back
     * to the clock. */
    if (_editing && now > _edit_until_ms) {
        _editing = false;
        _dirty   = true;
    }

    if (_dirty || now - _redraw_ms >= kRedrawIntervalMs) {
        _redraw_ms = now;
        _dirty     = false;
        render();
    }
}

void AppClock::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }

    /* Given back to whatever comes next, which is written against the
     * cells rather than against buttons of its own. */
    touch_keys::resume();
}

/* -------------------------------------------------------------------------- */
/*                                   drawing                                  */
/* -------------------------------------------------------------------------- */

void AppClock::render()
{
    auto& canvas     = GetHAL().canvas;
    const int width  = canvas.width();
    const int height = canvas.height();
    const int scale  = ui_scale(width);
    const int margin = 6 * scale;
    const int line_h = 10 * scale;

    canvas.fillScreen(kBg);
    app_header::draw(canvas, "Clock");
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);

    const int top = app_header::height() + margin;

    /* The chip's time, or the copy being edited: while a field is being
     * set, the face has to hold still. */
    std::tm shown = {};
    if (_editing) {
        shown = _edit;
    } else {
        std::time_t now = 0;
        std::time(&now);
        localtime_r(&now, &shown);
    }

    char text[80];

    /* The time, which is what this screen is opened for. */
    canvas.setTextSize(scale * 4);
    canvas.setTextColor(_editing ? kActive : kText, kBg);
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d", shown.tm_hour, shown.tm_min, shown.tm_sec);
    canvas.drawString(text, margin, top);
    const int clock_w = canvas.textWidth(text);
    const int clock_h = canvas.fontHeight();

    canvas.setTextSize(scale * 2);
    canvas.setTextColor(kText, kBg);
    static const char* const kWeek[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const int wday = (shown.tm_wday >= 0 && shown.tm_wday < 7) ? shown.tm_wday : 0;
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %s", shown.tm_year + 1900, shown.tm_mon + 1,
                  shown.tm_mday, kWeek[wday]);
    const int date_y = top + clock_h + margin / 2;
    canvas.drawString(text, margin, date_y);
    const int date_h = canvas.fontHeight();

    /*
     * Where the time came from, in the column beside the clock.
     *
     * Beside the clock rather than half the panel: on 320 columns the
     * stamp below is wider than half, and a column placed at the middle
     * is a column the stamp runs underneath.
     */
    const int right_x = margin + clock_w + margin;
    int right_y       = top;

    canvas.setTextSize(scale);

    const bool wifi    = GetHAL().isWifiConnected();
    const bool running = jobs::is_running("timed");

    if (timed::ever_synced()) {
        const std::uint32_t age_s = (GetHAL().millis() - timed::last_sync_ms()) / 1000U;
        if (age_s < 60U) {
            std::snprintf(text, sizeof(text), "SYNCED %us ago", (unsigned)age_s);
        } else if (age_s < 3600U) {
            std::snprintf(text, sizeof(text), "SYNCED %um ago", (unsigned)(age_s / 60U));
        } else {
            std::snprintf(text, sizeof(text), "SYNCED %uh ago", (unsigned)(age_s / 3600U));
        }
        canvas.setTextColor(kActive, kBg);
    } else if (GetHAL().isTimeSynced()) {
        /* A plausible clock that no server set during this run: either
         * somebody set it by hand, or a run before this one did. */
        std::snprintf(text, sizeof(text), "SET, NOT BY NTP");
        canvas.setTextColor(kText, kBg);
    } else {
        std::snprintf(text, sizeof(text), "NEVER SYNCED");
        canvas.setTextColor(kDim, kBg);
    }
    canvas.drawString(text, right_x, right_y);
    right_y += line_h;

    /* Two lines rather than one: what is left of the row beside a clock
     * this size is nineteen characters, and the sentence is longer. */
    canvas.setTextColor(running ? kText : kDim, kBg);
    canvas.drawString(running ? "job running" : "job stopped", right_x, right_y);
    right_y += line_h;
    canvas.setTextColor(wifi ? kText : kDim, kBg);
    canvas.drawString(wifi ? "network up" : "no network", right_x, right_y);

    /*
     * The stamp being set, with the selected field underlined -- an
     * underline rather than a box, because a field is two characters
     * wide and a box around two characters is mostly box.
     */
    const int stamp_size = scale * 2;
    const int stamp_y    = date_y + date_h + margin;

    canvas.setTextSize(stamp_size);
    canvas.setTextColor(_editing ? kText : kDim, kBg);
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", shown.tm_year + 1900,
                  shown.tm_mon + 1, shown.tm_mday, shown.tm_hour, shown.tm_min, shown.tm_sec);
    canvas.drawString(text, margin, stamp_y);
    const int stamp_h = canvas.fontHeight();

    if (_editing) {
        const int char_w = 6 * stamp_size;
        const Span_t& s  = kSpans[_field];
        canvas.fillRect(margin + s.at * char_w, stamp_y + stamp_h + 2, s.len * char_w, 2 * scale,
                        kActive);
    }

    /* Where the time is fetched from, across the full width: the zone
     * rule alone is thirty-one characters and fits nowhere narrower. */
    int block_y = stamp_y + stamp_h + margin * 2;
    canvas.setTextSize(scale);

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("TIME SERVERS", margin, block_y);
    block_y += line_h;

    canvas.setTextColor(kText, kBg);
    for (int i = 0; i < timed::server_count(); i++) {
        canvas.drawString(timed::server_name(i), margin, block_y);
        block_y += line_h;
    }

    canvas.setTextColor(kDim, kBg);
    std::snprintf(text, sizeof(text), "TZ %s", timed::timezone_rule());
    canvas.drawString(text, margin, block_y + line_h / 2);

    /* One line: what just happened, or how to make something happen.
     * Just above the buttons rather than under the stamp, since it says
     * what to press. Taken from the button row itself, so the two cannot
     * drift apart. */
    const int hint_y = button_rect(0, width, height, scale).y - 8 * scale - margin / 2;
    if (GetHAL().millis() < _notice_until_ms) {
        canvas.setTextColor(kActive, kBg);
        canvas.drawString(_notice, margin, hint_y);
    } else {
        canvas.setTextColor(kDim, kBg);
        canvas.drawString(_editing ? "type the field, or step it, then SET"
                                   : "PREV, NEXT or a digit starts setting the clock",
                          margin, hint_y);
    }

    /* The buttons, from the same geometry the taps are tested against. */
    canvas.setTextDatum(middle_center);
    for (int i = 0; i < kButtonCount; i++) {
        const Rect_t r    = button_rect(i, width, height, scale);
        const bool accent = (i == kBtnSet && _editing) || i == kBtnSync;

        canvas.drawRoundRect(r.x, r.y, r.w, r.h, 4 * scale, accent ? kActive : kText);
        canvas.setTextSize(scale);
        canvas.setTextColor(accent ? kActive : kText, kBg);
        canvas.drawString(kLabels[i], r.x + r.w / 2, r.y + r.h / 2);
    }
    canvas.setTextDatum(top_left);

    GetHAL().pushCanvas();
}

/* -------------------------------------------------------------------------- */
/*                                    input                                   */
/* -------------------------------------------------------------------------- */

void AppClock::handle_touch(bool touching, int x, int y)
{
    /* Press edge, with a debounce: the panel reports a held finger
     * continuously, and a button that steps the hour forty times a
     * second is not a button. */
    const std::uint32_t now = GetHAL().millis();
    if (touching && !_was_touching && (now - _last_tap_ms) > 250) {
        _last_tap_ms = now;
        handle_tap(x, y);
    }
    _was_touching = touching;
}

void AppClock::handle_tap(int x, int y)
{
    auto& canvas = GetHAL().canvas;

    /* The panel's coordinates are the panel's; this screen is a sprite
     * pushed past whatever the keyboard bar takes, which on this board
     * is nothing. */
    const int local_x = x - GetHAL().canvasKeyboardBar.width();
    const int local_y = y;

    const int width  = canvas.width();
    const int height = canvas.height();
    if (local_x < 0 || local_x >= width || local_y < 0 || local_y >= height) {
        return;
    }

    const int scale = ui_scale(width);
    for (int i = 0; i < kButtonCount; i++) {
        const Rect_t r = button_rect(i, width, height, scale);
        if (local_x < r.x || local_x >= r.x + r.w || local_y < r.y || local_y >= r.y + r.h) {
            continue;
        }

        switch (i) {
            case kBtnPrev:
                walk_field(-1);
                break;
            case kBtnNext:
                walk_field(1);
                break;
            case kBtnPlus:
                adjust(1);
                break;
            case kBtnMinus:
                adjust(-1);
                break;
            case kBtnSet:
                apply();
                break;
            case kBtnSync:
                request_sync();
                break;
            default:
                break;
        }
        return;
    }
}

void AppClock::handle_key(const Keyboard::KeyEvent_t& event)
{
    if (!event.state || event.isModifier) {
        return;
    }

    /* The corner key leaves as well as Escape does. Escape is what that
     * key carries on the Fn layer, which is two taps on a screen where
     * every other way out is one. */
    if (event.keyCode == KEY_ESC || event.keyCode == KEY_GRAVE) {
        _close_requested = true;
        return;
    }

    /* Digits by their name rather than their code: the keyboard hands
     * over the character it produced, and that is exactly the digit. */
    if (event.keyName != nullptr && event.keyName[0] >= '0' && event.keyName[0] <= '9' &&
        event.keyName[1] == '\0') {
        type_digit(event.keyName[0] - '0');
        return;
    }

    switch (event.keyCode) {
        case KEY_LEFT:
            walk_field(-1);
            break;
        case KEY_RIGHT:
        case KEY_TAB:
            walk_field(1);
            break;
        case KEY_UP:
            adjust(1);
            break;
        case KEY_DOWN:
            adjust(-1);
            break;
        case KEY_ENTER:
            apply();
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------- */
/*                                   editing                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Freeze the clock into the edit copy, if it is not already.
 *
 * @return true when this call is what started it, which the stepping
 *         buttons treat as their whole effect: entering the mode already
 *         puts the selection on the year, and moving off it in the same
 *         press is a press that lands somewhere nobody asked for.
 */
bool AppClock::begin_edit()
{
    if (_editing) {
        return false;
    }

    std::time_t now = 0;
    std::time(&now);
    localtime_r(&now, &_edit);

    _editing = true;
    _field   = FIELD_YEAR;
    _typed   = 0;
    return true;
}

void AppClock::note_input()
{
    _edit_until_ms = GetHAL().millis() + kEditLapseMs;
    _dirty         = true;
}

void AppClock::notice(const char* text)
{
    std::snprintf(_notice, sizeof(_notice), "%s", text);
    _notice_until_ms = GetHAL().millis() + kNoticeMs;
    _dirty           = true;
}

void AppClock::walk_field(int delta)
{
    if (begin_edit()) {
        note_input();
        return;
    }

    _field = (std::uint8_t)((_field + FIELD_COUNT + delta) % FIELD_COUNT);
    _typed = 0;
    note_input();
}

void AppClock::adjust(int delta)
{
    if (begin_edit()) {
        note_input();
        return;
    }
    _typed = 0;

    switch (_field) {
        case FIELD_YEAR: {
            int year = _edit.tm_year + 1900 + delta;
            if (year < 2000) {
                year = 2099;
            }
            if (year > 2099) {
                year = 2000;
            }
            _edit.tm_year = year - 1900;
            break;
        }
        case FIELD_MONTH:
            _edit.tm_mon = (_edit.tm_mon + 12 + delta) % 12;
            break;
        case FIELD_DAY: {
            const int last = days_in_month(_edit.tm_year + 1900, _edit.tm_mon + 1);
            int day        = _edit.tm_mday + delta;
            if (day < 1) {
                day = last;
            }
            if (day > last) {
                day = 1;
            }
            _edit.tm_mday = day;
            break;
        }
        case FIELD_HOUR:
            _edit.tm_hour = (_edit.tm_hour + 24 + delta) % 24;
            break;
        case FIELD_MINUTE:
            _edit.tm_min = (_edit.tm_min + 60 + delta) % 60;
            break;
        case FIELD_SECOND:
            _edit.tm_sec = (_edit.tm_sec + 60 + delta) % 60;
            break;
        default:
            break;
    }

    /* A month or a year that has just moved can leave the day past the
     * end of it -- the 31st of a February that was January a moment
     * ago. */
    const int last = days_in_month(_edit.tm_year + 1900, _edit.tm_mon + 1);
    if (_edit.tm_mday > last) {
        _edit.tm_mday = last;
    }

    note_input();
}

/*
 * A digit shifts in from the right, the way it does on anything with a
 * numeric field: the first one typed replaces what was there, the next
 * pushes it left. A field that is as long as it can be moves the
 * selection on by itself, so a whole date is typed without pressing
 * NEXT between the parts of it.
 *
 * Clamping happens on the last digit rather than on each one. Typing 09
 * has to be allowed to pass through 0, which is not a month.
 */
void AppClock::type_digit(int digit)
{
    /* Typing is its own intent, so it acts on the press that starts the
     * mode rather than being swallowed by it. */
    begin_edit();

    const int len = kSpans[_field].len;
    int value     = 0;

    switch (_field) {
        case FIELD_YEAR:
            value = _edit.tm_year + 1900;
            break;
        case FIELD_MONTH:
            value = _edit.tm_mon + 1;
            break;
        case FIELD_DAY:
            value = _edit.tm_mday;
            break;
        case FIELD_HOUR:
            value = _edit.tm_hour;
            break;
        case FIELD_MINUTE:
            value = _edit.tm_min;
            break;
        default:
            value = _edit.tm_sec;
            break;
    }

    value = (_typed == 0) ? digit : value * 10 + digit;
    _typed++;
    const bool complete = _typed >= len;

    switch (_field) {
        case FIELD_YEAR:
            if (complete) {
                if (value < 2000) {
                    value = 2000;
                }
                if (value > 2099) {
                    value = 2099;
                }
            }
            _edit.tm_year = value - 1900;
            break;
        case FIELD_MONTH:
            if (complete) {
                if (value < 1) {
                    value = 1;
                }
                if (value > 12) {
                    value = 12;
                }
            }
            _edit.tm_mon = value - 1;
            break;
        case FIELD_DAY: {
            if (complete) {
                const int last = days_in_month(_edit.tm_year + 1900, _edit.tm_mon + 1);
                if (value < 1) {
                    value = 1;
                }
                if (value > last) {
                    value = last;
                }
            }
            _edit.tm_mday = value;
            break;
        }
        case FIELD_HOUR:
            if (complete && value > 23) {
                value = 23;
            }
            _edit.tm_hour = value;
            break;
        case FIELD_MINUTE:
            if (complete && value > 59) {
                value = 59;
            }
            _edit.tm_min = value;
            break;
        default:
            if (complete && value > 59) {
                value = 59;
            }
            _edit.tm_sec = value;
            break;
    }

    if (complete) {
        if (_field + 1 < FIELD_COUNT) {
            _field = (std::uint8_t)(_field + 1);
        }
        _typed = 0;
    }

    note_input();
}

void AppClock::apply()
{
    if (!_editing) {
        notice("nothing to set");
        return;
    }

    std::tm when            = _edit;
    when.tm_isdst           = -1;  // let the zone rule decide whether this is summer
    const std::time_t stamp = std::mktime(&when);
    if (stamp == (std::time_t)-1) {
        notice("that is not a date");
        return;
    }

    struct timeval tv = {};
    tv.tv_sec         = stamp;
    tv.tv_usec        = 0;
    settimeofday(&tv, nullptr);

    _editing = false;
    _dirty   = true;

    /* Said plainly, because it is true and surprising: a running time
     * job will overwrite this at its next poll. */
    notice(jobs::is_running("timed") ? "clock set, ntp will correct it" : "clock set");
    mclog::tagInfo(getAppInfo().name, "clock set by hand");
}

void AppClock::request_sync()
{
    notice(timed::sync_now() ? "asked the time servers" : "no network to ask over");
}
