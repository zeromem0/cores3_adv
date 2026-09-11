/*
 * The external display, an ILI9341 on the expansion header.
 *
 * A 2.8 inch, 320x240 panel wired to the header pins, sharing SPI3 with
 * the card. Pinout and driver settings follow
 * github.com/AndyAiCardputer/zx-spectrum-cardputer-ili9341, which is
 * where this hardware arrangement comes from:
 *
 *   VCC pin 15   GND pin 11   CS pin 13 (G5)   RST pin 1 (G3)
 *   DC  pin 5 (G6)   MOSI pin 9 (G14)   SCK pin 7 (G40)
 *
 * The panel is not readable, so nothing can be read back off it -- the
 * remote mirror included. An application drawing here is not mirrored.
 *
 * Applications ask for it rather than assume it: `screen()` gives the
 * external panel when one answered at start-up and the built-in one
 * otherwise, so the same application works with or without it.
 */
#pragma once

#include <M5GFX.h>

namespace external_display {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

/**
 * @brief Bring the panel up, if one is there.
 *
 * @return false when no panel answered, in which case nothing was
 *         claimed and the header pins are left alone
 */
bool init();

bool is_present();

/** @brief The external panel, or the built-in one when there is none. */
lgfx::LGFX_Device& screen();

/** @brief The external panel only; null when there is none. */
lgfx::LGFX_Device* external();

}  // namespace external_display
