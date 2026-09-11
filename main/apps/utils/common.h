/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>

#define FW_VERSION "V0.4"

/* The circle that grew over the screen on the way into an application
 * and shrank on the way out is gone: both drew into the canvas and
 * pushed it a dozen times, which the desktop cannot survive -- it draws
 * only when something about it changed, so the shrinking circle ate the
 * icons and nothing was left marked as needing them back. */

struct AppIcon_t {
public:
    AppIcon_t(const uint16_t* iconBig, const uint16_t* iconSmall)
    {
        this->iconBig   = iconBig;
        this->iconSmall = iconSmall;
    }

    const uint16_t* iconBig;
    const uint16_t* iconSmall;
};
