/*
 * SPDX-License-Identifier: MIT
 *
 * Touch as a keyboard.
 *
 * This board has a 800x480 touch panel and no keys, while every
 * application here was written for a key matrix -- and not merely for
 * key codes: the launcher's menu switches on raw (row, column) pairs,
 * so an input layer that only produced key codes would leave it dead.
 *
 * So the screen is divided into nine cells and each one plays the chord
 * a Cardputer user would actually press. The arrows exist only on that
 * keyboard's Fn layer, so those cells hold Fn down for the duration of
 * the touch exactly as a finger would, and everything downstream --
 * raw handlers, key-code handlers, the modifier mask -- sees a sequence
 * it cannot distinguish from a real one.
 *
 *      +---------+---------+---------+
 *      |   esc   |   up    |         |
 *      +---------+---------+---------+
 *      |  left   |  enter  |  right  |
 *      +---------+---------+---------+
 *      |         |  down   |  space  |
 *      +---------+---------+---------+
 *
 * This is a bring-up layer, not a final input design: it navigates, and
 * it cannot type.
 */
#pragma once

namespace touch_keys {

/** @brief Poll the panel and inject key events. Call once per frame. */
void update();

/**
 * @brief Hand the screen to an application that reads touch itself.
 *
 * The nine cells are a fallback for software written against a
 * keyboard. An application with real touch targets -- the irrigation
 * panels, say -- wants the coordinates, not arrow keys, and would
 * otherwise get both for every tap. Suspending stops the injection
 * without tearing anything down; the application resumes it on the way
 * out.
 */
void suspend();
void resume();

}  // namespace touch_keys
