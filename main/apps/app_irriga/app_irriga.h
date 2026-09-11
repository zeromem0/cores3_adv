/*
 * Irrigation controller.
 *
 * The front end for the irrigd engine, which keeps watering whether this
 * is open or not.
 *
 * Laid out after the controller that actually waters the garden, a Core2
 * with a panel of exactly this size, rather than after the Cardputer
 * port that came between them: clock and date top left, address and four
 * zone letters top right, and the four zones as cards two by two under
 * it, each showing its own four slots with the one that is watering
 * inverted. Everything is reached by finger -- tapping the home screen
 * opens the menu, and every screen past it carries a BACK button --
 * because that is what the original did with the same 320x240 and the
 * same touch panel.
 *
 * What the original also carried and this does not: the clock is set in
 * Clock, the board is described in about, and the network is joined in
 * SetWiFi. Those were one firmware there and are four applications here.
 */
#pragma once
#include <hal/hal.h>
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
        SCREEN_MENU,
        SCREEN_MANUAL,
        SCREEN_ZONE_SELECT,
        SCREEN_EDIT,
        SCREEN_PINS,
    };

    std::uint8_t _screen = SCREEN_HOME;
    int _zone            = 0;
    int _slot            = 0;

    /* Which of the four buttons the arrow keys are pointing at. The
     * original had only a finger and needed none of this; a CardKB has
     * to be pointing at something visible. */
    int _focus = 0;

    /* Which part of a slot the editor is on: start hour, start minute,
     * end hour, end minute, the seven weekdays, then the active flag --
     * the same twelve steps the original walked, four slots over. */
    int _field = 0;

    int _key_slot         = -1;
    int _key_raw_slot     = -1;
    bool _close_requested = false;
    bool _dirty           = true;

    /* Drawing goes straight to the panel rather than into the launcher's
     * canvas, which this screen has claimed. The header and a zone card
     * are built in sprites first, as the original built them: they are
     * redrawn every second and painting them a field at a time on the
     * glass is what flicker is. */
    bool _needs_clear = true;
    LGFX_Sprite _header = LGFX_Sprite(&M5.Display);
    LGFX_Sprite _card   = LGFX_Sprite(&M5.Display);

    std::uint32_t _last_draw_ms = 0;

    /* Taps are taken on the press edge and no faster than a finger can
     * mean them; the panel reports a held finger continuously. */
    bool _was_touching         = false;
    std::uint32_t _last_tap_ms = 0;

    void draw();
    void draw_header(bool back, const char* title, bool with_zones);
    void draw_zone_card(int zone, int x, int y, bool editing);
    void draw_button(const char* label, int x, int y, int w, bool filled, std::uint16_t colour,
                     bool focused = false);
    void draw_home();
    void draw_menu();
    void draw_manual();
    void draw_zone_select();
    void draw_edit();
    void draw_pins();

    void handle_touch();
    void handle_tap(int x, int y);

    /** @brief Press one of the four buttons, whether by finger or by key. */
    void activate(int quadrant);

    /** @brief Whether this screen is four buttons the arrows can walk. */
    bool has_buttons() const;

    void cycle_pin(int delta);

    /** @brief Switch view, clearing what the last one left behind. */
    void show(std::uint8_t screen);

    /** @brief Walk the editor on by one of its forty-eight steps. */
    void next_field(int delta);

    void adjust(int delta);
    void handle_char(char ch);
};
