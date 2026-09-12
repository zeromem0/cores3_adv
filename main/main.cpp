/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.h>
#include <M5Unified.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps.h>
#include <hal.h>
#include <hal/utils/ble_keyboard/ble_keyboard.h>
#include <hal/utils/osk/osk.h>
#include <hal/utils/wifi_store/wifi_store.h>
#include <hal/utils/irrig/irrigd.h>
#include <hal/utils/jobs/builtin_jobs.h>
#include <hal/utils/jobs/jobs.h>
#include <hal/utils/remoted/remoted.h>
#include <hal/utils/remoted/screenshot.h>
#include <hal/utils/timed/timed.h>

/* A network seeded at the first boot, so a board with an empty NVS joins
 * one without anything having to be typed on it. Which network is the
 * builder's own business and not this repository's: drop a header next
 * to this one and it is used, leave it out and nothing is seeded. See
 * wifi_credentials.h.example. */
#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#define HAS_WIFI_SEED 1
#endif

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_debug);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();

    // The 2.8 inch panel on the expansion header, if one is plugged in.
    // Applications that can use it ask for it; the rest never notice.
    // No expansion-header panel on this board, and the Cardputer pins it
    // would use belong to something else here.
    // external_display::init();

    // Background services. Rejoining a known network and serving the
    // remote page were already running on their own; they are on the job
    // list now so the jobs application can show and stop them, alongside
    // the irrigation engine, which has to keep its schedule whatever is
    // on screen.
    /* Remembering is idempotent, and the entry can be removed from
     * SetWiFi like any other. */
#ifdef HAS_WIFI_SEED
    wifi_store::remember(WIFI_SEED_SSID, WIFI_SEED_PASSWORD);
#endif

    register_builtin_jobs();
    register_irrigd_job();
    timed::register_timed_job();
    ble_keyboard::register_ble_keyboard_job();
    jobs::start_autostart();
    screenshot::init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Install apps
    GetMooncake().installApp(std::make_unique<Launcher>());
    GetMooncake().installApp(std::make_unique<AppRecord>());
    /* Remote and StringIR drive an infrared LED, which this board does
     * not carry; Keyboard forwards a matrix that is not here either.
     * ZX Ext is left out by choice. */
    GetMooncake().installApp(std::make_unique<AppSetWiFi>());
    GetMooncake().installApp(std::make_unique<AppClock>());
    GetMooncake().installApp(std::make_unique<AppImu>());
    GetMooncake().installApp(std::make_unique<AppSdcard>());
    GetMooncake().installApp(std::make_unique<AppDhex>());
    GetMooncake().installApp(std::make_unique<AppAprecio>());
    GetMooncake().installApp(std::make_unique<AppZX>());
    GetMooncake().installApp(std::make_unique<AppIrriga>());
    GetMooncake().installApp(std::make_unique<AppJobs>());
    GetMooncake().installApp(std::make_unique<AppAbout>());
    GetMooncake().installApp(std::make_unique<AppWilma>());
    GetMooncake().installApp(std::make_unique<AppBClock>());
    GetMooncake().installApp(std::make_unique<AppTaskman>());

    // Main loop
    audio::set_keyboard_sfx_enable(true);
    while (1) {
        GetHAL().feedTheDog();
        GetHAL().update();
        jobs::update();

        /* A screenshot in progress parks this: the application is the
         * only thing that draws, so not updating it is what makes the
         * panel safe to read a row at a time. The keyboard above keeps
         * running, so the board is still answering while it holds. */
        screenshot::tick();
        if (!screenshot::is_frozen()) {
            GetMooncake().update();

            /* After the application has drawn, not before: drawing the
             * band first meant it was painted and then wiped, with
             * nothing left to mark it as needing redrawing. */
            osk::render();
        }
    }
}
