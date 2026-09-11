/*
 * CardKB on Grove Port A, standing in for the Cardputer's own matrix.
 *
 * The keyboard sends one ASCII byte per key over I2C. Every screen in
 * this firmware navigates by raw matrix positions rather than key
 * codes, so each byte is translated back into the position it would
 * occupy on a Cardputer and injected there. Applications cannot tell
 * the difference, and none of them needed changing.
 */
#pragma once

#include <cstdint>

namespace cardkb {

/* Probes Port A for the keyboard. False simply means none is plugged
 * in; the touch layer remains the way in. */
bool init();

bool isPresent();

/* Reads at most one key and injects it. Must run on the task that owns
 * the display, because the applications draw from their handlers. */
void update();

}  // namespace cardkb
