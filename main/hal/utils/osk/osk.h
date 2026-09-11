/*
 * The on-screen keyboard.
 *
 * This board has a 800x480 touch panel and no keys. touch_keys divides
 * the screen into nine cells and plays the chord a Cardputer user would
 * press, which is enough to navigate and, as its own header says,
 * cannot type. This is the other half: the whole matrix, drawn, in the
 * band below the application.
 *
 * The band costs nothing to keep. Applications on this port draw their
 * interfaces at the coordinates they were written for -- 240x135, in
 * the top left of a canvas that is 800 wide -- so the room underneath
 * was already empty. The canvas simply stops higher up, and anything
 * that lays itself out from canvas.height(), as the WiFi screen does,
 * reflows into what is left without being touched.
 *
 * What makes it correct rather than merely convenient: the keys are
 * drawn from the same table they inject from. A touch in the cell at
 * column seven of row two sends the matrix position (2,7), and the
 * legend on that cell came from the same entry. The picture and the
 * behaviour cannot disagree, because there is only one of them.
 *
 * Modifiers latch rather than being held, since one finger cannot hold
 * Fn and press a key at once. Latching Fn redraws the legends, which
 * the physical keyboard cannot do -- there, the Fn layer has to be
 * remembered.
 */
#pragma once

#include <cstdint>

namespace osk {

/** @brief Rows the band occupies at the bottom of the panel. */
int height();

/** @brief The first panel row belonging to the band. */
int top();

/** @brief Build the band's sprite. Called when the canvases are made. */
void init();

/** @brief Give the sprite back, with the other canvases. */
void deinit();

/** @brief Poll the panel and inject. Call once per frame. */
void update();

/** @brief Draw the band, if anything about it changed. */
void render();

/** @brief Ask for a redraw, after something else has painted over it. */
void invalidate();

/**
 * @brief Show or hide the band.
 *
 * The desktop hides it: there is nothing to type on a screen of icons,
 * and the two hundred rows it occupies are the difference between icons
 * you tap with a finger and icons you tap with a fingernail. Hiding
 * clears the band to black once and stops drawing it; showing marks it
 * for redrawing.
 */
void set_visible(bool visible);

/** @brief Whether the band is on screen and taking touches. */
bool visible();

}  // namespace osk
