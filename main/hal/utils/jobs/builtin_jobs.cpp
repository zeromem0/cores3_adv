/*
 * SPDX-License-Identifier: MIT
 *
 * The background services this firmware already had, put behind the job
 * list so they can be seen and stopped like anything else. Neither one
 * changes behaviour by being here: remoted still follows the WiFi link up
 * and down on its own, and the rejoin task still scans on its own task.
 */
#include "builtin_jobs.h"

#include <hal/hal.h>
#include <hal/utils/remoted/remoted.h>
#include <hal/utils/wifi_store/wifi_store.h>

#include <string>

#include "jobs.h"

namespace {

/* -------------------------------------------------------------------------- */
/*                                  remoted                                    */
/* -------------------------------------------------------------------------- */
bool _remoted_wanted;

bool remoted_start()
{
    _remoted_wanted = true;

    /* Its own tick only brings the server up when the link changes, so
     * starting the job while the link is already up has to do it here --
     * otherwise stopping and starting from the jobs application would
     * leave the server down until the network came and went. */
    if (GetHAL().isWifiConnected() && !remoted::is_running()) {
        return remoted::start();
    }
    return true;
}

void remoted_stop()
{
    _remoted_wanted = false;
    remoted::stop();
}

void remoted_tick(std::uint32_t now_ms)
{
    (void)now_ms;
    if (_remoted_wanted) {
        remoted::update();
    }
}

const char* remoted_detail()
{
    if (!remoted::is_running()) {
        return "waiting for wifi";
    }
    return remoted::address().c_str();
}

/* -------------------------------------------------------------------------- */
/*                              wifi auto connect                              */
/* -------------------------------------------------------------------------- */
bool _wifi_started;

bool wifi_start()
{
    GetHAL().wifiInit();

    if (!_wifi_started) {
        wifi_store::start_auto_connect();
        _wifi_started = true;
    } else {
        wifi_store::resume_auto_connect();
    }
    return true;
}

void wifi_stop()
{
    /* The driver goes down, not just the rejoining: most of what stopping
     * this frees is the driver's own buffers, and something that needs a
     * large block of heap -- the emulator wants 48 KB in one piece --
     * gets nothing from a task that has merely stopped scanning.
     *
     * Suspend first, so the rejoin task is not halfway through an
     * association when the driver is taken from under it. */
    wifi_store::suspend_auto_connect();
    GetHAL().wifiDeinit();
}

const char* wifi_detail()
{
    static std::string text;
    if (!GetHAL().isWifiConnected()) {
        return "not connected";
    }
    text = GetHAL().getIpAddress();
    return text.empty() ? "connected" : text.c_str();
}

}  // namespace

void register_builtin_jobs()
{
    jobs::register_job({
        "wifid",
        "rejoins a known network",
        wifi_start,
        wifi_stop,
        nullptr,
        wifi_detail,
        true,
    });

    jobs::register_job({
        "remoted",
        "screen and keyboard over http",
        remoted_start,
        remoted_stop,
        remoted_tick,
        remoted_detail,
        true,
    });
}
