/*
 * SPDX-License-Identifier: MIT
 */
#include "jobs.h"

#include <hal/hal.h>
#include <hal/utils/settings/settings.h>
#include <mooncake_log.h>

#include <cstring>

namespace jobs {

namespace {

const std::string _tag = "jobs";

/*
 * Whether each job is wanted, kept across reboots.
 *
 * The autostart flag in the Job struct is only the factory answer: it
 * says what a board with no history should do. Once someone has stopped
 * the irrigation engine, starting it again on the next boot is not a
 * default, it is ignoring them -- so the flag becomes the default for a
 * key here, and the stored value wins from then on.
 *
 * Each access opens its own Settings: the class commits in its
 * destructor, so a long-lived instance would leave writes hanging.
 */
const char* kSettingsNamespace = "jobs";

/* Set while start_autostart is replaying stored state, so restoring a
 * job does not write back the value it was just read from. */
bool _restoring = false;

bool wanted(const Job& job)
{
    Settings settings(kSettingsNamespace);
    return settings.GetBool(job.name, job.autostart);
}

void remember(const char* name, bool on)
{
    if (_restoring) {
        return;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetBool(name, on);
}

/* Jobs are registered from start-up code and never removed, so a fixed
 * array keeps the whole thing out of the heap. */
constexpr std::size_t kMaxJobs = 8;

struct Entry_t {
    Job job;
    State state;
    std::uint32_t since_ms;
};

Entry_t _jobs[kMaxJobs];
std::size_t _count;

Entry_t* find(const char* name)
{
    if (name == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 0; i < _count; i++) {
        if (std::strcmp(_jobs[i].job.name, name) == 0) {
            return &_jobs[i];
        }
    }
    return nullptr;
}

}  // namespace

void register_job(const Job& job)
{
    if (_count >= kMaxJobs || job.name == nullptr) {
        mclog::tagError(_tag, "no room for job");
        return;
    }
    _jobs[_count].job      = job;
    _jobs[_count].state    = State::Stopped;
    _jobs[_count].since_ms = 0;
    _count++;
}

void start_autostart()
{
    _restoring = true;
    for (std::size_t i = 0; i < _count; i++) {
        if (wanted(_jobs[i].job)) {
            start(_jobs[i].job.name);
        }
    }
    _restoring = false;
}

std::size_t count()
{
    return _count;
}

bool status(std::size_t index, Status& out)
{
    if (index >= _count) {
        return false;
    }
    const Entry_t& entry = _jobs[index];
    out.name             = entry.job.name;
    out.summary          = entry.job.summary;
    out.detail           = (entry.job.detail != nullptr) ? entry.job.detail() : nullptr;
    out.state            = entry.state;
    out.since_ms         = entry.since_ms;
    return true;
}

bool start(const char* name)
{
    Entry_t* entry = find(name);
    if (entry == nullptr) {
        return false;
    }
    if (entry->state == State::Running) {
        return true;
    }

    const bool ok  = (entry->job.start == nullptr) || entry->job.start();
    entry->state    = ok ? State::Running : State::Failed;
    entry->since_ms = GetHAL().millis();

    /* Recorded even when the start failed: what is stored is that this
     * job is wanted, and a failure now says nothing about the next
     * boot -- the WiFi job cannot start without a network to join. */
    remember(entry->job.name, true);

    mclog::tagInfo(_tag, "{}: {}", entry->job.name, ok ? "started" : "start failed");
    return ok;
}

bool stop(const char* name)
{
    Entry_t* entry = find(name);
    if (entry == nullptr) {
        return false;
    }
    /* Stopping something that failed to start still has to run its stop
     * hook: a half-built service leaves things behind too. */
    if (entry->state == State::Stopped) {
        return true;
    }

    if (entry->job.stop != nullptr) {
        entry->job.stop();
    }
    entry->state    = State::Stopped;
    entry->since_ms = GetHAL().millis();

    remember(entry->job.name, false);

    mclog::tagInfo(_tag, "{}: stopped", entry->job.name);
    return true;
}

bool is_running(const char* name)
{
    const Entry_t* entry = find(name);
    return entry != nullptr && entry->state == State::Running;
}

void update()
{
    const std::uint32_t now = GetHAL().millis();
    for (std::size_t i = 0; i < _count; i++) {
        if (_jobs[i].state == State::Running && _jobs[i].job.tick != nullptr) {
            _jobs[i].job.tick(now);
        }
    }
}

}  // namespace jobs
