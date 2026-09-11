/*
 * SPDX-License-Identifier: MIT
 */
#include "timed.h"

#include <hal/hal.h>
#include <hal/utils/jobs/jobs.h>
#include <mooncake_log.h>

#include <esp_sntp.h>

#include <cstdio>
#include <ctime>

namespace timed {

namespace {

const std::string _tag = "timed";

/* The Romanian pool, twice: these are two draws from the same country
 * zone rather than one server and a mirror of it, so a machine that is
 * down or lying takes the other with it far less often than a single
 * name would.
 *
 * Here rather than beside the client that consumes them, because they
 * are also what the clock application lists on screen, and a list that
 * is written down twice is a list that will disagree with itself. */
const char* const kServers[] = {"0.ro.pool.ntp.org", "1.ro.pool.ntp.org"};

/* Eastern European time, with the EU daylight saving rule: forward on
 * the last Sunday of March, back on the last Sunday of October. */
const char kTimezone[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";

std::uint32_t _last_sync_ms = 0;
bool _ever_synced           = false;
char _detail[48]            = {0};

/* Called by the SNTP client whenever it has actually set the clock, so
 * what the jobs list shows is a real answer from a real server rather
 * than the moment this job was started. */
void on_time_set(struct timeval* tv)
{
    (void)tv;
    _last_sync_ms = GetHAL().millis();
    _ever_synced  = true;
    mclog::tagInfo(_tag, "clock set from the network");
}

bool start()
{
    sntp_set_time_sync_notification_cb(on_time_set);

    /* Starting without a network is not a failure: the client waits, and
     * WiFi coming up later is what it is waiting for. Reporting failure
     * here would put the job in the failed state on every cold boot,
     * since the radio takes longer to associate than this takes to
     * start. */
    GetHAL().timeSyncStart();
    return true;
}

void stop()
{
    GetHAL().timeSyncStop();
}

const char* detail()
{
    if (!GetHAL().isTimeSynced()) {
        std::snprintf(_detail, sizeof(_detail), "clock not set yet");
        return _detail;
    }

    if (!_ever_synced) {
        /* The clock is good but this job did not set it -- it was
         * already running from a previous start, or WiFi set it before
         * the notification was hooked up. */
        std::snprintf(_detail, sizeof(_detail), "clock set, not by this run");
        return _detail;
    }

    const std::uint32_t age_s = (GetHAL().millis() - _last_sync_ms) / 1000U;
    if (age_s < 60U) {
        std::snprintf(_detail, sizeof(_detail), "synced %us ago", (unsigned)age_s);
    } else if (age_s < 3600U) {
        std::snprintf(_detail, sizeof(_detail), "synced %um ago", (unsigned)(age_s / 60U));
    } else {
        std::snprintf(_detail, sizeof(_detail), "synced %uh ago", (unsigned)(age_s / 3600U));
    }
    return _detail;
}

}  // namespace

bool sync_now()
{
    if (!GetHAL().isWifiConnected()) {
        return false;
    }

    if (!jobs::is_running("timed")) {
        return jobs::start("timed");
    }

    if (esp_sntp_enabled()) {
        /* A restart is what asks now: the client polls on its own
         * interval, and a daily one means the answer to a button press
         * would otherwise arrive tomorrow. */
        esp_sntp_restart();
        return true;
    }

    GetHAL().timeSyncStart();
    return true;
}

bool ever_synced()
{
    return _ever_synced;
}

std::uint32_t last_sync_ms()
{
    return _last_sync_ms;
}

int server_count()
{
    return (int)(sizeof(kServers) / sizeof(kServers[0]));
}

const char* server_name(int index)
{
    if (index < 0 || index >= server_count()) {
        return "";
    }
    return kServers[index];
}

const char* timezone_rule()
{
    return kTimezone;
}

void register_timed_job()
{
    jobs::Job job = {};
    job.name      = "timed";
    job.summary   = "clock from ro.pool.ntp.org, daily";
    job.start     = start;
    job.stop      = stop;
    job.tick      = nullptr;  // the SNTP client runs on its own
    job.detail    = detail;
    job.autostart = true;

    jobs::register_job(job);
}

}  // namespace timed
