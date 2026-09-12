/*
 * SPDX-License-Identifier: MIT
 */
#include "app_taskman.h"

#include "assets/taskman_big.h"
#include "assets/taskman_small.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <hal.h>
#include <mooncake_log.h>

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <new>

using namespace mooncake;

namespace {

/*
 * Colours as raw RGB565, because that is the only form this panel takes
 * without argument: a plain integer handed to LGFX is read as 24-bit
 * and comes out as something else entirely -- which is why the launcher
 * menu's selected row, written as TFT_DARKGREEN, is blue.
 */
constexpr std::uint16_t rgb(int r, int g, int b)
{
    return (std::uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr std::uint16_t kBg      = rgb(10, 10, 12);
constexpr std::uint16_t kRule    = rgb(58, 58, 66);
constexpr std::uint16_t kFaint   = rgb(120, 118, 112);
constexpr std::uint16_t kText    = rgb(228, 226, 220);
constexpr std::uint16_t kCpu     = rgb(255, 138, 91);   // load, filled
constexpr std::uint16_t kCpuDim  = rgb(96, 46, 28);
constexpr std::uint16_t kRam     = rgb(122, 200, 140);  // free memory, a line
constexpr std::uint16_t kRow     = rgb(32, 34, 40);
constexpr std::uint16_t kGrid    = rgb(78, 78, 86);

/* Samples between time marks. One a second, so this is half a minute. */
constexpr std::uint32_t kGridEvery = 30;

constexpr int kRowHeight = 9;
constexpr int kGraphRows = 44;

/* The numbers and the graph move down under the band across the top.
 * The line keeps a row of its own rather than being squeezed into the
 * band beside the title: on 320 columns what is left between "TaskMan"
 * and the corner button is nineteen characters and the line is thirty. */
int header_y()
{
    return app_header::height() + 2;
}
int graph_top()
{
    return header_y() + 10;
}

/* Column positions in the task list, in pixels. Font0 is six wide. */
constexpr int kColName  = 2;
constexpr int kColState = kColName + 12 * 6;
constexpr int kColLoad  = kColState + 2 * 6;
constexpr int kColStack = kColLoad + 5 * 6;
constexpr int kColCore  = kColStack + 7 * 6;

/*
 * What a task is doing when it is not running, which the percentage
 * cannot say: a task blocked on a queue and one spinning ready to run
 * both read as zero. Borrowed from the top command in SolarOS
 * (github.com/nilseuropa/solar_os), letters and all.
 */
char state_char(eTaskState state)
{
    switch (state) {
        case eRunning:   return 'R';
        case eReady:     return 'r';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

}  // namespace

AppTaskman::AppTaskman()
{
    setAppInfo().name     = "TaskMan";
    setAppInfo().userData = new AppIcon_t(image_data_taskman_big, image_data_taskman_small);
}

AppTaskman::~AppTaskman()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

/*
 * One reading of the whole system.
 *
 * The run time counter each task carries is cumulative since boot, so a
 * single reading says nothing about now; what matters is how much each
 * one moved since the last reading, against how much the total moved.
 * Tasks come and go between samples, so the previous counters are
 * matched by handle rather than by position.
 */
void AppTaskman::sample()
{
    Store_t& s = *_store;

    std::uint32_t total = 0;
    const int count = (int)uxTaskGetSystemState(s.status, kMaxTasks, &total);
    _task_count = count;

    /*
     * The first reading only writes down where everything stands.
     *
     * Counted like any other it would compare against nothing: the
     * interval becomes the whole time since boot, every task is unseen
     * and so credited with using none of it, and no idle time across the
     * entire uptime works out, correctly and uselessly, as a processor
     * that has been fully busy since power-on. That was the solid bar at
     * the left edge of the graph.
     */
    if (!_primed) {
        for (int i = 0; i < count; i++) {
            s.handles[i] = s.status[i].xHandle;
            s.runtime[i] = s.status[i].ulRunTimeCounter;
            s.load[i]    = 0;
            s.order[i]   = i;
        }
        s.previous_count = count;
        _previous_total  = total;
        _primed          = true;
        _dirty           = true;
        return;
    }

    const std::uint32_t elapsed = total - _previous_total;
    _previous_total = total;

    std::uint32_t idle = 0;
    int cores          = 0;
    for (int i = 0; i < count; i++) {
        /* What this task used since the last look. A task first seen now
         * has no previous counter, and counts as having used nothing --
         * better than crediting it with everything it did before it was
         * noticed. */
        std::uint32_t previous = s.status[i].ulRunTimeCounter;
        for (int p = 0; p < s.previous_count; p++) {
            if (s.handles[p] == s.status[i].xHandle) {
                previous = s.runtime[p];
                break;
            }
        }

        const std::uint32_t used = s.status[i].ulRunTimeCounter - previous;
        s.load[i] = (elapsed > 0) ? (std::uint8_t)((used * 100 + elapsed / 2) / elapsed) : 0;

        if (std::strncmp(s.status[i].pcTaskName, "IDLE", 4) == 0) {
            idle += used;
            cores++;
        }
    }

    /*
     * Load across the whole chip.
     *
     * Each task's share above is measured against one core's worth of
     * time, which is the convention worth keeping -- IDLE1 at 100% says
     * plainly that core one did nothing. But the chip's capacity over
     * the same interval is that time once per core, and there is no
     * better count of cores available here than the number of idle
     * tasks: FreeRTOS runs exactly one on each.
     */
    if (cores < 1) {
        cores = 1;
    }
    const std::uint32_t capacity = elapsed * (std::uint32_t)cores;
    _cpu_now = (capacity > 0 && idle < capacity)
                   ? (std::uint8_t)(((capacity - idle) * 100 + capacity / 2) / capacity)
                   : 0;

    for (int i = 0; i < count; i++) {
        s.handles[i] = s.status[i].xHandle;
        s.runtime[i] = s.status[i].ulRunTimeCounter;
    }
    s.previous_count = count;

    /* Busiest first, which is the only order a list this short is worth
     * showing in. Insertion sort over indices: thirty items, once a
     * second, on a processor that has just been asked how busy it is. */
    for (int i = 0; i < count; i++) {
        s.order[i] = i;
    }
    for (int i = 1; i < count; i++) {
        const int key = s.order[i];
        int j = i - 1;
        while (j >= 0 && s.load[s.order[j]] < s.load[key]) {
            s.order[j + 1] = s.order[j];
            j--;
        }
        s.order[j + 1] = key;
    }

    _free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    _free_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    s.cpu_history[_head] = _cpu_now;
    s.ram_history[_head] = (std::uint16_t)(_free_now / 1024U);
    _head = (_head + 1) % kHistory;
    _ticks++;
    if (_samples < kHistory) {
        _samples++;
    }

    _dirty = true;
}

/*
 * Load as a filled column and free memory as a line, sharing the same
 * box. Two scales in one plot would be a poor chart on paper; here it is
 * the right trade, because what is being read off it is not a value but
 * a shape -- when the load rose, whether the memory came back.
 */
void AppTaskman::draw_graph(int top, int height)
{
    auto& canvas = GetHAL().display;
    const int width = canvas.width();
    const int bottom = top + height - 1;

    canvas.fillRect(0, top, width, height, kBg);

    /* Halfway, as something for the eye to measure against. */
    for (int x = 0; x < width; x += 4) {
        canvas.drawPixel(x, top + height / 2, kRule);
    }

    const int shown = (_samples < width) ? _samples : width;

    /*
     * A mark every half minute, dotted rather than drawn, and counted
     * against the sample number rather than the column: that is what
     * makes it travel left with the data it marks instead of sitting
     * still while the graph slides underneath it. Eight columns of nine
     * dots is nothing to draw, which is the point -- a solid grid
     * redrawn every second would cost more than the traces do.
     */
    for (int i = 0; i < shown; i++) {
        const int age = shown - 1 - i;
        if ((_ticks - 1 - (std::uint32_t)age) % kGridEvery != 0) {
            continue;
        }
        const int x = width - 1 - age;
        for (int y = top + 2; y < top + height; y += 5) {
            canvas.drawPixel(x, y, kGrid);
        }
    }

    for (int i = 0; i < shown; i++) {
        /* Oldest on the left: walk back from the newest sample. */
        const int age  = shown - 1 - i;
        const int idx  = (_head - 1 - age + kHistory * 2) % kHistory;
        const int x    = width - 1 - age;

        const int load = _store->cpu_history[idx];
        const int bar  = load * height / 100;
        if (bar > 0) {
            canvas.drawFastVLine(x, bottom - bar + 1, bar, kCpuDim);
            canvas.drawPixel(x, bottom - bar + 1, kCpu);
        }

        if (_ram_total > 0) {
            const std::uint32_t free_bytes = (std::uint32_t)_store->ram_history[idx] * 1024U;
            int y = bottom - (int)((std::uint64_t)free_bytes * height / _ram_total);
            if (y < top) y = top;
            if (y > bottom) y = bottom;
            canvas.drawPixel(x, y, kRam);
        }
    }

    canvas.drawFastHLine(0, bottom + 1, width, kRule);
}

int AppTaskman::visible_rows(int top, int bottom) const
{
    const int rows = (bottom - top) / kRowHeight;
    return rows > 0 ? rows : 1;
}

void AppTaskman::draw_list(int top, int bottom)
{
    auto& canvas = GetHAL().display;
    const int width   = canvas.width();
    const int visible = visible_rows(top, bottom);

    int drawn = 0;
    for (int i = _top; i < _task_count && drawn < visible; i++) {
        const int t = _store->order[i];
        const int y = top + drawn * kRowHeight;
        drawn++;

        /* Every row paints its own background: nothing wipes the panel
         * between draws, so a row has to cover whatever it said before. */
        canvas.fillRect(0, y, width, kRowHeight, (drawn & 1) ? kBg : kRow);
        const std::uint16_t back = (drawn & 1) ? kBg : kRow;

        char name[13];
        std::snprintf(name, sizeof(name), "%s", _store->status[t].pcTaskName);
        canvas.setTextColor(kText, back);
        canvas.drawString(name, kColName, y + 1);

        /* Running or ready stands out; blocked and suspended are the
         * quiet majority and are drawn as such. */
        const eTaskState state = _store->status[t].eCurrentState;
        const char letter[2]   = {state_char(state), '\0'};
        canvas.setTextColor((state == eRunning || state == eReady) ? kText : kFaint, back);
        canvas.drawString(letter, kColState, y + 1);

        char load[8];
        std::snprintf(load, sizeof(load), "%3u%%", (unsigned)_store->load[t]);
        canvas.setTextColor(_store->load[t] > 0 ? kCpu : kFaint, back);
        canvas.drawString(load, kColLoad, y + 1);

        /*
         * Stack never used, in bytes.
         *
         * FreeRTOS counts the high water mark in words, and showing that
         * number under a column headed "stack" was quietly wrong by a
         * factor of four -- 732 read as bytes when it meant 2928. Small
         * numbers here are what precedes a stack overflow, so being out
         * by four in the reassuring direction is the wrong way to be
         * wrong.
         */
        const unsigned free_bytes =
            (unsigned)_store->status[t].usStackHighWaterMark * (unsigned)sizeof(StackType_t);
        char stack[10];
        std::snprintf(stack, sizeof(stack), "%5u", free_bytes);
        canvas.setTextColor(free_bytes < 1024 ? kCpu : kFaint, back);
        canvas.drawString(stack, kColStack, y + 1);

        char core[4];
        const int on = (int)_store->status[t].xCoreID;
        std::snprintf(core, sizeof(core), "%s", (on == 0 || on == 1) ? (on == 0 ? "0" : "1") : "-");
        canvas.setTextColor(kFaint, back);
        canvas.drawString(core, kColCore, y + 1);
    }

    /* Whatever the list no longer reaches. */
    const int used = top + drawn * kRowHeight;
    if (used < bottom) {
        canvas.fillRect(0, used, width, bottom - used, kBg);
    }
}

void AppTaskman::draw()
{
    auto& canvas = GetHAL().display;
    const int width  = canvas.width();
    const int height = canvas.height();

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);

    /* Header: the two numbers the graph is a history of, plus the low
     * water mark, which is the one that says whether anything nearly ran
     * out while nobody was watching. */
    char line[48];
    std::snprintf(line, sizeof(line), "cpu %3u%%   free %4uK   low %4uK", (unsigned)_cpu_now,
                  (unsigned)(_free_now / 1024U), (unsigned)(_free_min / 1024U));
    app_header::draw(canvas, "TaskMan");
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.fillRect(0, app_header::height() + 1, width, graph_top() - app_header::height() - 2, kBg);
    canvas.setTextColor(kText, kBg);
    canvas.drawString(line, kColName, header_y());

    draw_graph(graph_top(), kGraphRows);

    const int list_top = graph_top() + kGraphRows + 2;
    draw_list(list_top, height - 10);

    /* Footer, and the count of everything running whether or not it fits
     * on the list above. */
    std::snprintf(line, sizeof(line), "%d tasks   ;/. scroll   esc back", _task_count);
    canvas.fillRect(0, height - 10, width, 10, kBg);
    canvas.setTextColor(kFaint, kBg);
    canvas.drawString(line, kColName, height - 9);

    _dirty = false;
}

void AppTaskman::scroll(int delta)
{
    if (_task_count == 0) {
        return;
    }
    const int visible = visible_rows(graph_top() + kGraphRows + 2, GetHAL().display.height() - 10);

    _top += delta;
    if (_top > _task_count - visible) {
        _top = _task_count - visible;
    }
    if (_top < 0) {
        _top = 0;
    }
    _dirty = true;
}

void AppTaskman::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().setFullScreenApp(true);
    GetHAL().display.fillScreen(kBg);
    app_header::reset();

    _store = new (std::nothrow) Store_t();
    if (_store == nullptr) {
        mclog::tagError(getAppInfo().name, "no room for the task table ({} bytes)", sizeof(Store_t));
        return;
    }

    _task_count     = 0;
    _samples        = 0;
    _head           = 0;
    _top            = 0;
    _previous_total = 0;
    _primed         = false;
    _ticks          = 0;
    _ram_total      = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    _last_sample_ms = 0;

    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& key) {
        if (!key.state) {
            return;
        }
        if (key.row == 2 && key.col == 11) {
            scroll(-1);
        } else if (key.row == 3 && key.col == 11) {
            scroll(1);
        }
    });

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& key) {
        if (key.isModifier || !key.state) {
            return;
        }
        if (key.keyCode == KEY_ESC) {
            _close_requested = true;
        }
    });
}

void AppTaskman::onRunning()
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

    if (_store == nullptr) {
        return;
    }

    const std::uint32_t now = GetHAL().millis();
    if (now - _last_sample_ms >= kSampleMs) {
        _last_sample_ms = now;
        sample();
    }

    if (_dirty) {
        draw();
    }
}

void AppTaskman::onClose()
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

    delete _store;
    _store = nullptr;

    GetHAL().setFullScreenApp(false);
}
