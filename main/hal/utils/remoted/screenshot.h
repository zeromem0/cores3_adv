/*
 * A screenshot of a moving application, without the tearing.
 *
 * The panel mirror on /frame cannot give one. When an application owns
 * the whole screen the mirror falls back to quarters and its buffer
 * holds exactly one of them, so four requests are four different
 * moments -- fine for watching a menu, useless for a clock with
 * molecules drifting across it.
 *
 * This takes the other route. Rather than finding 64 KB to hold a whole
 * frame, which is the one thing this board does not have spare while a
 * heavy application is open, it stops the application: a flag tells the
 * main loop to skip the update that draws, and only once the loop has
 * confirmed it is parked does the panel get read, a row at a time,
 * straight out to the network. Nothing can draw in between, so there is
 * nothing to tear.
 *
 * The cost is a picture frozen on the glass for as long as the read
 * takes, and a deadline in case the client goes away mid-capture -- a
 * board left frozen by a dropped connection would be worse than a torn
 * screenshot.
 */
#pragma once

namespace screenshot {

/** @brief Attach /shot to the remote server. */
void init();

/** @brief Main loop: acknowledge a freeze, and end one that overran. */
void tick();

/** @brief Main loop: true while the application must not be updated. */
bool is_frozen();

}  // namespace screenshot
