/*
 * Opening and closing applications from the remote page.
 *
 * The panel mirror could already show what is on screen and inject key
 * presses, but walking the launcher to an application by sending arrow
 * keys is not something to rely on: the menu reads a single-slot key
 * latch that the main loop clears every pass, so an injected press is
 * visible for one turn and a consumer that polls at any other rate can
 * miss it. Naming the application is unambiguous and does not touch the
 * input path at all.
 *
 * The routes answer on the HTTP task, which must not open an application
 * itself -- that would run onOpen, and everything it draws, off the task
 * that owns the display. They leave a request behind instead, and the
 * launcher picks it up on its own update, the same way the irrigation
 * editor applies a saved schedule.
 */
#pragma once
#include <string>
#include <vector>

namespace remote_launch {

enum class Action {
    None,
    Open,
    Close,
};

struct Request {
    Action action = Action::None;
    std::string name;
};

/** @brief Attach /app to the remote server. Safe to call once. */
void init();

/** @brief The menu's applications, in the order they are shown. */
void publish(const std::vector<std::string>& names, const std::string& startup);

/** @brief What is on screen now, "" for the launcher itself. */
void set_running(const std::string& name);

/** @brief Take the pending request, if any. Called from the main loop. */
Request take();

}  // namespace remote_launch
