/*
 * The clock, kept honest once a day.
 *
 * Fetching the time was something WiFi did on its own the moment it
 * connected, with no way to see it and no way to stop it. It belongs on
 * the job list with the others: visible in the jobs application, and
 * switchable -- which now survives a reboot, so a board told not to ask
 * the network for the time stays that way.
 *
 * The engine is ESP-IDF's own SNTP client in polling mode, given a
 * daily interval, rather than a timer here re-triggering it: the client
 * already knows how to retry, back off and notify.
 */
#pragma once

#include <cstdint>

namespace timed {

/** @brief Add the daily time sync to the job list. Does not start it. */
void register_timed_job();

/**
 * @brief Ask for an answer now instead of at the next daily poll.
 *
 * A stopped job is started rather than refused: pressing sync is a
 * request for the network's time, and the only honest way to grant it is
 * to have the client running -- which the job list will then show, and
 * which survives the reboot, the same as starting it from there.
 *
 * @return false when there is no network to ask over.
 */
bool sync_now();

/** @brief Whether a server has set the clock since this board booted. */
bool ever_synced();

/** @brief When it last did, on the same clock as GetHAL().millis(). */
std::uint32_t last_sync_ms();

/** @brief The servers this firmware asks, in the order it asks them. */
int server_count();
const char* server_name(int index);

/** @brief The rule the clock is displayed in, in TZ form. */
const char* timezone_rule();

}  // namespace timed
