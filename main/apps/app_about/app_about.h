/*
 * What this thing is, and what it is made of.
 *
 * Modelled on the About screen in esp32-si4732/ats-mini: the numbers
 * that tell you whether the board is healthy -- chip, flash, memory,
 * address -- on one page you can photograph and send to someone.
 *
 * The colour bar at the bottom is doing a second job. It is also the
 * quickest test of the panel: eight primaries and a grey ramp show at a
 * glance whether the RGB565 path, the byte order and the bit depth are
 * all still what they should be. The controller this firmware's
 * irrigation came from carried the same strip for the same reason.
 */
#pragma once
#include <mooncake.h>
#include <cstdint>

class AppAbout : public mooncake::AppAbility {
public:
    AppAbout();
    ~AppAbout();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    int _key_slot         = -1;
    bool _close_requested = false;
    std::uint32_t _last_draw_ms = 0;

    void draw();
    void draw_colour_bar(int y, int height);
};
