/*
 * The schedule editor, in a browser.
 *
 * The page from the controller this was ported from: one tab per zone,
 * four cards of start and end time, the seven day letters that go out
 * when you tap them, a switch per slot and one SAVE button. It is served
 * by the remoted server rather than one of its own, at /irrig.
 *
 * What the browser sends is applied on the main loop, not on the HTTP
 * task: the engine writes NVS and drives relay pins, and it is written
 * on the understanding that nothing else runs at the same time.
 */
#pragma once

namespace irrig_web {

/** @brief Attach the page to the remoted server. */
void start();

void stop();

/** @brief Apply anything the browser sent. Called from the daemon's tick. */
void tick();

}  // namespace irrig_web
