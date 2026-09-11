/*
 * Host bindings for the ZX Spectrum emulator.
 *
 * The emulator core under zx/ is carried in from the standalone sketch
 * unmodified; everything that touches this firmware's hardware lives
 * here: drawing the 256x192 screen onto the panel, turning key events
 * into the Spectrum's key matrix, and feeding the beeper to the speaker.
 */
#pragma once

#include <cstdint>

#include "zx/spectrum/keyboard_defs.h"

namespace zx_host {

/* The Spectrum's own screen, which on this board is drawn pixel for
 * pixel: 256x192 inside a 320x240 panel leaves 32 columns and 24 rows
 * over, and those are exactly what the machine calls the border.
 *
 * The panel this port came from was 240x135, which held neither
 * dimension. Everything drawn there was either squeezed -- one column in
 * sixteen thrown away -- or windowed, and eight-pixel characters came out
 * five pixels tall, which is why BASIC could not be read. Nothing is
 * resampled here, so there is nothing to choose between and no view to
 * cycle. */
constexpr int kZxScreenWidth  = 256;
constexpr int kZxScreenHeight = 192;

/** @brief Allocate the frame buffer and prepare the panel. */
bool renderer_begin();
void renderer_end();

/**
 * @brief Draw one emulated frame: 256x192 pixels inside its border.
 *
 * @param border_colour the machine's current border, 0-7
 */
void renderer_draw(const uint8_t* screen, uint8_t border_colour);

/**
 * @brief Which Spectrum key a typed character maps to.
 *
 * Mapping by character rather than by matrix position, because the
 * Spectrum's own layout has nothing to do with this keyboard's: what
 * matters is that pressing M produces M. Characters the Spectrum only
 * reaches through symbol shift set @p needs_symbol_shift.
 *
 * @return SPECKEY_NONE when the character has no Spectrum equivalent
 */
SpecKeys map_char(char ch, bool* needs_symbol_shift);

/** @brief Speaker, driven from the emulator's per-frame sample accumulator. */
bool beeper_begin();
void beeper_end();
void beeper_submit_frame(const uint16_t* accumulator);

}  // namespace zx_host
