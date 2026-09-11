/*
 * Binds a graphics surface to an M5GFX sprite.
 *
 * C++ only -- ported application sources include dhex_gfx.h and
 * never see this.
 */
#pragma once

#include <M5GFX.h>

#include <functional>

#include "dhex_gfx.h"

/**
 * @brief Create a graphics surface drawing into @p sprite.
 *
 * @param sprite  destination sprite, owned by the caller and required to
 *                outlive the returned surface
 * @param present invoked by dhex_gfx_present(), typically pushing the
 *                sprite to the display
 */
dhex_gfx_t* dhex_host_gfx_create(LGFX_Sprite* sprite, std::function<void()> present);

void dhex_host_gfx_destroy(dhex_gfx_t* gfx);
