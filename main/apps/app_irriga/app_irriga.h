/*
 * Irrigation controller.
 *
 * The front end for the irrigd engine, which keeps watering whether this
 * is open or not. Laid out after the controller this was ported from --
 * clock and date top left, address and zone letters top right, the
 * zone's four schedule slots below, the slot whose window contains the
 * current time drawn inverted -- on a quarter of the screen it was drawn
 * for, with a keyboard instead of a touch panel.
 *
 * Screens: the home view, a manual switch view, and a slot editor that
 * walks field by field the way the original's NEXT / +1 / -1 buttons
 * did, because that is what fits four times over on this panel.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>

class AppIrriga : public mooncake::AppAbility {
public:
    AppIrriga();
    ~AppIrriga();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum Screen_t : std::uint8_t {
        SCREEN_HOME,
        SCREEN_MANUAL,
        SCREEN_EDIT,
        SCREEN_PINS,
    };

    std::uint8_t _screen = SCREEN_HOME;
    int _zone            = 0;
    int _slot            = 0;

    /* Which part of a slot the editor is on: start hour, start minute,
     * end hour, end minute, the seven weekdays, then the active flag --
     * the same twelve steps the original walked. */
    int _field = 0;

    int _key_slot         = -1;
    int _key_raw_slot     = -1;
    bool _close_requested = false;
    bool _dirty           = true;

    /* Drawing goes straight to the panel: the application canvas is a
     * quarter smaller than the glass and a full-screen sprite would cost
     * 65 KB of a heap that has none to spare. Nothing here changes width
     * between draws, so opaque text paints over itself without flicker
     * and the screen is only cleared when the view changes. */
    bool _needs_clear = true;

    std::uint32_t _last_draw_ms = 0;

    void draw();
    void draw_header();
    void draw_home();
    void draw_manual();
    void draw_edit();
    void draw_pins();

    void cycle_pin(int delta);

    /** @brief Switch view, clearing what the last one left behind. */
    void show(std::uint8_t screen);

    void adjust(int delta);
    void handle_char(char ch);
};
