/*
 * SPDX-License-Identifier: MIT
 *
 * Irrigation daemon. A job rather than part of the application on
 * purpose: watering has to continue whichever application is on screen,
 * or with none open at all. The application is a front end onto the same
 * engine, and closing it changes nothing about the valves.
 */
#include "irrigd.h"

#include <mooncake_log.h>

#include <cstdio>

#include "../jobs/jobs.h"
#include "irrig.h"
#include "irrig_web.h"

namespace {

constexpr std::uint32_t kEvalIntervalMs = 1000;

std::uint32_t _next_eval_ms;

bool irrigd_start()
{
    irrig::init();

    /* The schedule editor rides on the remoted server, so it is here
     * rather than in the application: the page has to answer whether or
     * not anyone is looking at the panel. A failure to attach it never
     * stops the daemon -- watering matters more than the page. */
    irrig_web::start();

    _next_eval_ms = 0;
    return true;
}

void irrigd_stop()
{
    irrig_web::stop();

    /* Never leave a valve open with nobody watching it. */
    irrig::all_off();
}

void irrigd_tick(std::uint32_t now_ms)
{
    if (_next_eval_ms != 0 && (std::int32_t)(now_ms - _next_eval_ms) < 0) {
        return;
    }
    _next_eval_ms = now_ms + kEvalIntervalMs;

    /* Anything the browser saved is applied before this pass, so a
     * schedule change takes effect within the second. */
    irrig_web::tick();
    irrig::update();
}

const char* irrigd_detail()
{
    static char text[32];

    int on = 0;
    for (int zone = 0; zone < irrig::zone_count(); zone++) {
        if (irrig::zone_on(zone)) {
            on++;
        }
    }

    std::snprintf(text, sizeof(text), "%d zone%s, %d on, %s", irrig::zone_count(),
                  irrig::zone_count() == 1 ? "" : "s", on,
                  irrig::mode() == irrig::Mode::Manual ? "manual" : "auto");
    return text;
}

}  // namespace

void register_irrigd_job()
{
    jobs::register_job({
        "irrigd",
        "irrigation schedule engine",
        irrigd_start,
        irrigd_stop,
        irrigd_tick,
        irrigd_detail,
        true,
    });
}
