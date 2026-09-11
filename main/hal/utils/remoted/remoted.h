/*
 * remoted -- remote screen and keyboard over HTTP.
 *
 * Serves a single page that mirrors what the panel is showing and sends
 * key presses back. The keys are injected into the keyboard HAL, so the
 * whole firmware responds to them, launcher included, not just whichever
 * application happens to be open.
 *
 * Runs as a daemon: start() brings the server up, update() has to be
 * called from the main loop and both drains queued remote keys and
 * follows the WiFi connection up and down.
 */
#pragma once

#include <esp_http_server.h>

#include <string>

namespace remoted {

/**
 * @brief Attach a page of someone else's to this server.
 *
 * The server comes and goes with the network, so a route registered once
 * here is registered again on every restart. Anything that wants a page
 * of its own attaches to this one rather than opening a second server:
 * a listening socket and its task cost more than the page is worth.
 */
void add_route(const httpd_uri_t& route);

/**
 * @brief Offer a path on the front page, so it can be found again.
 *
 * A route is not the same thing as a link: some of them are endpoints
 * nobody types, and one page can be reached at more than one address.
 * Whatever asks for a link here appears in the list at the bottom of the
 * remote page, which is the only place all of this is written down.
 */
void add_link(const char* path, const char* title);

/** @brief Bring the HTTP server up. Requires an established WiFi link. */
bool start();

void stop();

bool is_running();

/** @brief Where the page is reachable, e.g. "http://192.168.1.20/". */
const std::string& address();

/**
 * @brief Main loop tick.
 *
 * Starts and stops the server as WiFi comes and goes, then replays any
 * key events the server queued. Injection happens here, on the main
 * task, because application key handlers draw to the panel and the HTTP
 * task must not touch the display bus.
 */
void update();

}  // namespace remoted
