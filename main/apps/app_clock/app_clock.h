/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <cstdint>
#include <hal/hal.h>
#include <ctime>

/**
 * @brief The clock, and the two ways it can be set.
 *
 * The face and the palette come from the irrigation controller, which is
 * the other screen on this board read from across a room: a large time
 * with everything about where it came from beside it, the stamp being
 * edited under it, and one row of buttons along the bottom.
 *
 * Setting it is a digital watch's idea of setting it -- walk to a field,
 * step it up or down, press SET -- with the difference that a CardKB on
 * the Grove port can simply type a field. Both matter: the buttons work
 * with nothing but a finger, and typing a date is four presses rather
 * than forty.
 */
class AppClock : public mooncake::AppAbility {
public:
    AppClock();
    ~AppClock();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    /* The order they are walked in, which is the order they are written
     * in: year first, seconds last. */
    enum Field_t : std::uint8_t {
        FIELD_YEAR = 0,
        FIELD_MONTH,
        FIELD_DAY,
        FIELD_HOUR,
        FIELD_MINUTE,
        FIELD_SECOND,
        FIELD_COUNT,
    };

    static constexpr std::uint32_t kRedrawIntervalMs = 1000;

    /* Editing lapses rather than having to be dismissed. A frozen clock
     * left frozen because somebody walked away is a clock that lies, and
     * the only state worth keeping is the one on the chip. */
    static constexpr std::uint32_t kEditLapseMs = 30000;
    static constexpr std::uint32_t kNoticeMs    = 4000;

    std::uint32_t _redraw_ms = 0;

    int _key_slot         = -1;
    bool _close_requested = false;
    bool _dirty           = true;

    /*
     * The copy being edited. While this is live the screen shows it
     * instead of the clock, which is what makes setting a time possible
     * at all: a field cannot be typed into while the seconds under it
     * keep moving.
     */
    std::tm _edit                = {};
    std::uint8_t _field          = FIELD_YEAR;
    bool _editing                = false;
    std::uint32_t _edit_until_ms = 0;

    /* Digits typed into the current field so far, so that a second one
     * shifts in beside the first instead of replacing it. */
    int _typed = 0;

    bool _was_touching         = false;
    std::uint32_t _last_tap_ms = 0;

    /* What the last button press did, said for a few seconds. */
    char _notice[48]               = {0};
    std::uint32_t _notice_until_ms = 0;

    void render();

    void handle_touch(bool touching, int x, int y);
    void handle_tap(int x, int y);
    void handle_key(const Keyboard::KeyEvent_t& event);

    bool begin_edit();
    void walk_field(int delta);
    void adjust(int delta);
    void type_digit(int digit);
    void apply();
    void request_sync();
    void note_input();
    void notice(const char* text);
};
