/*
 * Background jobs -- the things that keep running whatever is on screen.
 *
 * The firmware already had several: the HTTP server behind remoted, the
 * task that rejoins a known WiFi network, and now the irrigation engine,
 * which must keep its valves on schedule no matter which application is
 * open, or with none open at all. They were each started from their own
 * corner and could not be seen or stopped from anywhere; this puts them
 * in one list with one lifecycle.
 *
 * A job is registered once at start-up and lives for the life of the
 * firmware; what changes is whether it is running. Every hook is called
 * from the main loop, so a job may touch the display, the panel bus and
 * the shared I2C bus without locking, exactly like an application can.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace jobs {

enum class State : std::uint8_t {
    Stopped,
    Running,
    Failed,
};

/**
 * @brief One background service.
 *
 * @param start   brings the service up; false means it could not start
 * @param stop    takes it down; must leave nothing running behind
 * @param tick    called from the main loop while running, may be null
 * @param detail  one short line about what it is doing, may be null
 */
struct Job {
    const char* name;
    const char* summary;
    bool (*start)();
    void (*stop)();
    void (*tick)(std::uint32_t now_ms);
    const char* (*detail)();
    bool autostart;
};

struct Status {
    const char* name;
    const char* summary;
    const char* detail;
    State state;
    std::uint32_t since_ms;
};

/** @brief Add a job to the list. Registration does not start it. */
void register_job(const Job& job);

/** @brief Start everything marked autostart. Call once, after registration. */
void start_autostart();

std::size_t count();
bool status(std::size_t index, Status& out);

bool start(const char* name);
bool stop(const char* name);
bool is_running(const char* name);

/** @brief Main loop tick: gives every running job its turn. */
void update();

}  // namespace jobs
