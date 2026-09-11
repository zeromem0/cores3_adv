/*
 * Known WiFi networks, and reconnecting to whichever one is in range.
 *
 * The set wifi application keeps a single SSID and password, so joining a
 * phone hotspot in the field overwrites the network at home and the other
 * way round. This keeps a short list instead, most recently used first,
 * and a background task that rejoins any of them without the application
 * having to be opened at all.
 */
#pragma once

#include <string>
#include <vector>

namespace wifi_store {

constexpr int kMaxNetworks = 4;

/**
 * @brief Add or refresh a network, moving it to the front of the list.
 *
 * The oldest entry falls off the end once the list is full.
 */
void remember(const std::string& ssid, const std::string& password);

/** @brief Password stored for @p ssid, if it is known. */
bool password_for(const std::string& ssid, std::string& password);

/** @brief Known networks, most recently used first. */
std::vector<std::string> list();

void forget(const std::string& ssid);

/**
 * @brief Start the background task that rejoins known networks.
 *
 * Scans whenever the link is down and connects to the strongest known
 * network it finds. Runs on its own task because scanning and associating
 * both block for seconds and the main loop drives the display.
 */
void start_auto_connect();

/**
 * @brief Hold the auto connect task off.
 *
 * The set wifi application drives the same WiFi calls from the main task,
 * and they are not safe to overlap.
 */
void suspend_auto_connect();
void resume_auto_connect();

}  // namespace wifi_store
