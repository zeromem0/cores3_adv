/*
 * The Brownian clock, brought over from its own sketch.
 *
 * https://github.com/zeromem0/brownianClock -- air molecules drifting
 * and bouncing off the edges, with the time over the top of them. It is
 * built on the MovingIcons example from LovyanGFX, whose trick is that
 * the frame is never held whole: the panel is drawn a band at a time
 * into a sprite far smaller than the screen, and the bands are pushed as
 * they are finished.
 *
 * That trick is why it ports here at all. A full 240x135 frame buffer is
 * 64 KB in one piece, which is the same block the emulator wants and
 * more than this board reliably has once WiFi has been up for a while;
 * two bands of 68 rows are 32 KB each, and 32 KB is a request that gets
 * answered.
 *
 * Two things differ from the sketch, both because the hardware differs.
 * It drove ten SK6812 LEDs on a Core2, which this board does not have.
 * And it read an external RTC, which is not fitted here yet -- the time
 * comes from the system clock instead, the one the timed job keeps in
 * step over SNTP, which is also where the Clock application reads it.
 */
#pragma once
#include <mooncake.h>

#include <M5GFX.h>

#include <cstdint>
#include <ctime>

class AppBClock : public mooncake::AppAbility {
public:
    AppBClock();
    ~AppBClock();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    /* One drifting molecule. Straight from the sketch, including the
     * bounce that flips the sign at the edge rather than reflecting the
     * position properly -- it is what gives the motion its character. */
    struct Object_t {
        std::int_fast16_t x;
        std::int_fast16_t y;
        std::int_fast16_t dx;
        std::int_fast16_t dy;
        std::int_fast8_t img;
        float r;
        float z;
        float dr;
        float dz;

        void move(int width, int height);
    };

    static constexpr int kObjectCount = 50;

    /* Height of one band, chosen at open: the sketch starts at half the
     * panel and halves again until the pair of sprites fits, which is
     * the right instinct on a board where the answer changes with what
     * else is running. */
    int _band_height = 0;

    Object_t* _objects = nullptr;
    LGFX_Sprite _bands[2];
    LGFX_Sprite _icons[3];

    /*
     * There is no sprite for the clock face, though the sketch had one.
     * Composing HH:MM once and pushing it as a unit costs 24 KB held for
     * as long as the application is open, and asking for it after the
     * bands have taken the large blocks is asking at the worst possible
     * moment -- it failed exactly there, with 21 KB left. The five
     * glyphs go straight onto each band instead, clipped by it, which
     * needs no memory at all.
     */
    bool _ready           = false;
    int _key_slot         = -1;
    bool _close_requested = false;

    /* Frames counted over the last whole second. The point of showing
     * it is comparison: the same scene, the same fifty molecules, on two
     * boards side by side. */
    std::uint32_t _frames    = 0;
    std::uint32_t _fps       = 0;
    std::uint32_t _fps_ms    = 0;

    /*
     * Which set is drifting. The sketches this comes from are one
     * program with three sets of artwork between them -- the air
     * molecules, the snowflakes, and the icons the LovyanGFX example
     * shipped with before either replaced them. Switching is only ever
     * about what gets pushed into the three sprites: nothing about the
     * motion changes, and neither does the memory, since the two sets
     * not on screen stay in flash.
     */
    enum Drifting_t : std::uint8_t {
        DRIFT_MOLECULES,
        DRIFT_SNOW,
        DRIFT_ICONS,
        DRIFT_COUNT,
    };
    std::uint8_t _drifting = DRIFT_MOLECULES;

    bool build_sprites();
    void load_icons();
    void release_sprites();

    /** @brief One NN:NN group -- the time, or the date -- onto a band. */
    void draw_group(LGFX_Sprite& band, int x, int y, int first, int second, int separator);

    void draw_frame();
};
