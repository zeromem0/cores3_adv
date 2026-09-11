/*
 * What the board is actually doing with itself.
 *
 * Two questions this firmware has had to answer by guesswork until now:
 * where the processor goes, and where the memory goes. The second was
 * measured once, by hand, with probes compiled in for an afternoon; this
 * puts both on the panel, continuously, next to the list of tasks
 * responsible.
 *
 * It reads FreeRTOS directly through uxTaskGetSystemState(), which needs
 * two options that are off by default and are turned on in
 * sdkconfig.defaults for this: the trace facility, and run time stats.
 * Without the second, the idle task's share of the processor is not
 * recorded anywhere, and processor load is not a thing that can be
 * computed after the fact.
 *
 * The load shown includes this application drawing itself, which is
 * honest rather than convenient: a task manager that hid its own cost
 * would be lying about the number it exists to report.
 */
#pragma once
#include <mooncake.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

class AppTaskman : public mooncake::AppAbility {
public:
    AppTaskman();
    ~AppTaskman();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    /* More than this board runs, with room for what it might. Tasks past
     * the limit are dropped from the list rather than the count. */
    static constexpr int kMaxTasks = 32;

    /* One sample per column of the panel, one second apart: four
     * minutes of history, which is long enough to see WiFi reconnect or
     * an application open and give the memory back. */
    static constexpr int kHistory    = 240;
    static constexpr std::uint32_t kSampleMs = 1000;

    /* Everything held only while the application is open. The launcher
     * builds every application object at boot, so anything left as a
     * member is memory this one costs even when nobody is looking -- the
     * mistake the phrase recogniser had to be rescued from. */
    struct Store_t {
        TaskStatus_t status[kMaxTasks];

        /* Run time counters from the previous sample, matched by handle:
         * the counter is cumulative, and only the difference between two
         * samples says anything about now. */
        TaskHandle_t handles[kMaxTasks];
        std::uint32_t runtime[kMaxTasks];
        int previous_count = 0;

        /* Per task, worked out at each sample and kept for drawing. */
        std::uint8_t load[kMaxTasks];
        int order[kMaxTasks];

        std::uint8_t cpu_history[kHistory];
        std::uint16_t ram_history[kHistory];  // KiB free
    };
    Store_t* _store = nullptr;

    int _task_count = 0;
    int _samples    = 0;  // how much of the history is filled
    int _head       = 0;  // where the next sample goes

    /* Every sample ever taken, which is what the time raster counts
     * against. Tying the marks to screen position instead would leave
     * them standing still while the graph flowed underneath. */
    std::uint32_t _ticks = 0;

    std::uint8_t _cpu_now  = 0;
    std::uint32_t _free_now = 0;
    std::uint32_t _free_min = 0;
    std::uint32_t _ram_total = 0;

    std::uint32_t _last_sample_ms = 0;
    std::uint32_t _previous_total = 0;

    /* Whether a reading has been taken to compare the next one against. */
    bool _primed = false;

    int _top              = 0;
    int _key_slot         = -1;
    int _key_raw_slot     = -1;
    bool _close_requested = false;
    bool _dirty           = true;

    void sample();
    void draw();
    void draw_graph(int top, int height);
    void draw_list(int top, int bottom);

    /** @brief How many task rows fit under the graph. */
    int visible_rows(int top, int bottom) const;

    void scroll(int delta);
};
