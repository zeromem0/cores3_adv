/*
 * The background services, and a way to stop them.
 *
 * Everything the firmware keeps running behind whatever is on screen is
 * on one list: the network rejoin task, the remote screen server, the
 * irrigation engine. Until now they started themselves and could not be
 * seen at all, which made a device that was misbehaving hard to reason
 * about -- and a valve engine you cannot see the state of is worse than
 * that.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>

class AppJobs : public mooncake::AppAbility {
public:
    AppJobs();
    ~AppJobs();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    int _selected         = 0;

    /* First row on screen. The list outgrew the panel once a fourth job
     * was added, so it scrolls rather than running under the legend. */
    int _top              = 0;

    int _key_slot         = -1;
    int _key_raw_slot     = -1;
    bool _close_requested = false;
    bool _dirty           = true;

    /* Whether the next draw starts from a blank panel. Drawing goes
     * straight to the glass now, so wiping it once a second -- which is
     * the rate this redraws at -- would be a visible flash. Every row
     * paints its own background instead, and the screen is only cleared
     * when the application opens. */
    bool _needs_clear     = true;

    std::uint32_t _last_draw_ms = 0;

    void draw();
    void move(int delta);
    void toggle_selected();

    /** @brief How many rows fit between the header and the legend. */
    int visible_rows() const;

    /** @brief Pull the scroll offset along so the selection stays on screen. */
    void follow_selection();
};
