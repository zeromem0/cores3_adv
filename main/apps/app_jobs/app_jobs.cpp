/*
 * SPDX-License-Identifier: MIT
 */
#include "app_jobs.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal.h>
#include <hal/utils/jobs/jobs.h>
#include <mooncake_log.h>

#include <cstdio>

#include "assets/jobs_big.h"
#include "assets/jobs_small.h"

using namespace mooncake;

namespace {

/* The list is short and the state changes on its own, so it is redrawn
 * on a slow beat rather than only when a key is pressed. */
constexpr std::uint32_t kRefreshMs = 1000;

/*
 * The three columns, in pixels, and the name's width in characters.
 *
 * Font0 is six pixels to the character, so the panel is forty columns
 * wide. Ten of them are enough for every job name here and leave the
 * detail its own column instead of a second line -- which is what let
 * the row shrink from twenty-four pixels to fourteen, and the list show
 * seven jobs where it used to show four.
 *
 * Kept as constants shared by the header and the rows, because the two
 * drifting apart is exactly how a table stops looking like one.
 */
constexpr int kColName   = 4;
constexpr int kNameChars = 10;
constexpr int kColState  = kColName + kNameChars * 6;

/* Four characters of state, RUN / STOP / FAIL, and two of gap. Words
 * this short read as a status column rather than as prose, and they buy
 * the detail beside them two more characters. */
constexpr int kColDetail = kColState + 6 * 6;

constexpr int kRowHeight = 14;

/* Everything here is measured down from under the band across the top,
 * which every other application on this board carries too. The three
 * numbers used to be 2, 12 and 22; they are the same three, moved down
 * by the band. */
int column_header_y()
{
    return app_header::height() + 2;
}
int rule_y()
{
    return column_header_y() + 10;
}
int list_top()
{
    return column_header_y() + 20;
}

/* The legend and the rule above it, measured up from the bottom edge, so
 * the list knows where it has to stop. This is what the fourth job ran
 * into: the rows simply carried on and the detail line landed on top of
 * "Enter: start/stop". */
constexpr int kLegendHeight = 14;

/* Same accent the irrigation controller uses for anything that is on. */
constexpr std::uint16_t kColourOn = 0xFC00;

/* Background of the selected row, and of everything else. */
constexpr std::uint16_t kColourRow = 0x2104;
constexpr std::uint16_t kColourBg  = TFT_BLACK;

}  // namespace

AppJobs::AppJobs()
{
    setAppInfo().name     = "Jobs";
    setAppInfo().userData = new AppIcon_t(image_data_jobs_big, image_data_jobs_small);
}

AppJobs::~AppJobs()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

int AppJobs::visible_rows() const
{
    const int room = GetHAL().display.height() - kLegendHeight - list_top();
    const int rows = room / kRowHeight;
    return rows > 0 ? rows : 1;
}

void AppJobs::follow_selection()
{
    const int visible = visible_rows();
    if (_selected < _top) {
        _top = _selected;
    } else if (_selected >= _top + visible) {
        _top = _selected - visible + 1;
    }
}

void AppJobs::draw()
{
    auto& canvas = GetHAL().display;

    if (_needs_clear) {
        canvas.fillScreen(kColourBg);
        _needs_clear = false;
    }

    app_header::draw(canvas, "Jobs");

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    const int total       = (int)jobs::count();
    const int visible     = visible_rows();
    const int list_bottom = canvas.height() - kLegendHeight;

    canvas.setTextColor(TFT_DARKGREY, kColourBg);
    canvas.drawString("name", kColName, column_header_y());
    canvas.drawString("state", kColState, column_header_y());
    canvas.drawString("detail", kColDetail, column_header_y());

    /* How far down the list this is, shown only when there is more of it
     * than fits -- otherwise the counter is noise. */
    if (total > visible) {
        char position[16];
        std::snprintf(position, sizeof(position), "%d/%d", _selected + 1, total);
        canvas.setTextDatum(top_right);
        canvas.drawString(position, app_header::back_left(canvas.width()) - 8, column_header_y());
        canvas.setTextDatum(top_left);
    }
    canvas.drawFastHLine(0, rule_y(), canvas.width(), TFT_DARKGREY);

    int drawn = 0;
    for (int i = _top; i < total && drawn < visible; i++) {
        jobs::Status status = {};
        if (!jobs::status((std::size_t)i, status)) {
            continue;
        }

        const int y        = list_top() + drawn * kRowHeight;
        const bool current = (i == _selected);
        drawn++;

        /* Painted whether or not the row is selected: nothing wipes the
         * screen between redraws, so a row has to cover its own previous
         * state -- the highlight it has just lost, and a detail line that
         * has since become shorter. */
        if (current) {
            canvas.fillRoundRect(0, y - 3, canvas.width(), kRowHeight - 2, 3, kColourRow);
        } else {
            canvas.fillRect(0, y - 3, canvas.width(), kRowHeight - 2, kColourBg);
        }
        const std::uint16_t back = current ? kColourRow : kColourBg;

        const bool running = (status.state == jobs::State::Running);

        /* Cut to the column rather than allowed to run into the next
         * one: a name longer than the column is a layout problem, not a
         * reason to lose the state beside it. */
        char name[kNameChars + 1];
        std::snprintf(name, sizeof(name), "%s", status.name != nullptr ? status.name : "");
        canvas.setTextColor(TFT_WHITE, back);
        canvas.drawString(name, kColName, y);

        canvas.setTextColor(running ? kColourOn : (status.state == jobs::State::Failed ? TFT_RED : TFT_DARKGREY),
                            back);
        canvas.drawString(running ? "RUN" : (status.state == jobs::State::Failed ? "FAIL" : "STOP"), kColState, y);

        /* Whatever is left of the width. A detail longer than that is
         * clipped by the panel edge, which is the honest outcome of
         * giving it a column instead of a line of its own. */
        if (status.detail != nullptr) {
            canvas.setTextColor(TFT_LIGHTGREY, back);
            canvas.drawString(status.detail, kColDetail, y);
        }
    }

    /* Whatever the list no longer reaches, given back to the background:
     * scrolling up leaves the last row of the previous view behind. */
    const int used = list_top() + drawn * kRowHeight - 3;
    if (used < list_bottom) {
        canvas.fillRect(0, used, canvas.width(), list_bottom - used, kColourBg);
    }

    canvas.drawFastHLine(0, list_bottom, canvas.width(), TFT_DARKGREY);
    canvas.setTextColor(TFT_DARKGREY, kColourBg);
    canvas.drawString("Enter: start/stop", 4, canvas.height() - 10);

    _last_draw_ms = GetHAL().millis();
    _dirty        = false;
}

void AppJobs::move(int delta)
{
    const int total = (int)jobs::count();
    if (total == 0) {
        return;
    }
    _selected = (_selected + delta + total) % total;
    follow_selection();
    _dirty    = true;
}

void AppJobs::toggle_selected()
{
    jobs::Status status = {};
    if (!jobs::status((std::size_t)_selected, status)) {
        return;
    }

    if (status.state == jobs::State::Running) {
        jobs::stop(status.name);
    } else {
        jobs::start(status.name);
    }
    _dirty = true;
}

void AppJobs::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    /* The whole panel: this list is the only thing worth looking at while
     * it is open, and the launcher's bars cost it two rows. */
    GetHAL().setFullScreenApp(true);

    _selected    = 0;
    _top         = 0;
    _needs_clear = true;
    _dirty       = true;
    app_header::reset();

    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& key) {
        if (!key.state) {
            return;
        }
        if (key.row == 2 && key.col == 11) {
            move(-1);
        } else if (key.row == 3 && key.col == 11) {
            move(1);
        }
    });

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& key) {
        if (key.isModifier || !key.state) {
            return;
        }
        switch (key.keyCode) {
            case KEY_ENTER:
            case KEY_SPACE:
                toggle_selected();
                break;
            case KEY_ESC:
                _close_requested = true;
                break;
            default:
                break;
        }
    });
}

void AppJobs::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        audio::play_random_tone();
        close();
        return;
    }

    if (app_header::back_pressed(GetHAL().display.width(), 0, 0)) {
        audio::play_random_tone();
        close();
        return;
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    if (_dirty || (GetHAL().millis() - _last_draw_ms) >= kRefreshMs) {
        draw();
    }
}

void AppJobs::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    if (_key_raw_slot >= 0) {
        GetHAL().keyboard.onKeyEventRaw.disconnect(_key_raw_slot);
        _key_raw_slot = -1;
    }

    GetHAL().setFullScreenApp(false);
}
