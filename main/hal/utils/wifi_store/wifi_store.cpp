/*
 * SPDX-License-Identifier: MIT
 */
#include "wifi_store.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mooncake_log.h>

#include <algorithm>

#include "../../hal.h"

namespace wifi_store {

namespace {

const std::string _tag = "wifi_store";

// Entries live under numbered keys rather than one blob, so a corrupt or
// half-written entry costs a single network instead of the whole list.
constexpr const char* kSsidKeyPrefix = "wifi_ssid";
constexpr const char* kPassKeyPrefix = "wifi_pass";

// Time between scans while the link is down. Long enough not to keep the
// radio busy, short enough to have rejoined by the time the device is
// picked up again.
constexpr uint32_t kRetryIntervalMs = 20000;

bool _auto_connect_started = false;
volatile bool _suspended = false;

std::string ssid_key(int slot)
{
    return kSsidKeyPrefix + std::to_string(slot);
}

std::string pass_key(int slot)
{
    return kPassKeyPrefix + std::to_string(slot);
}

struct Entry_t {
    std::string ssid;
    std::string password;
};

std::vector<Entry_t> load()
{
    std::vector<Entry_t> entries;
    auto& settings = GetHAL().getSettings();

    for (int slot = 0; slot < kMaxNetworks; slot++) {
        const std::string ssid = settings.GetString(ssid_key(slot), "");
        if (ssid.empty()) {
            continue;
        }
        entries.push_back({ssid, settings.GetString(pass_key(slot), "")});
    }
    return entries;
}

void store(const std::vector<Entry_t>& entries)
{
    auto& settings = GetHAL().getSettings();

    for (int slot = 0; slot < kMaxNetworks; slot++) {
        if (slot < static_cast<int>(entries.size())) {
            settings.SetString(ssid_key(slot), entries[slot].ssid);
            settings.SetString(pass_key(slot), entries[slot].password);
        } else {
            settings.SetString(ssid_key(slot), "");
            settings.SetString(pass_key(slot), "");
        }
    }
}

void auto_connect_task(void*)
{
    // Let the rest of the system finish coming up before touching the radio.
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (true) {
        if (_suspended || GetHAL().isWifiConnected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        const std::vector<Entry_t> known = load();
        if (known.empty()) {
            vTaskDelay(pdMS_TO_TICKS(kRetryIntervalMs));
            continue;
        }

        // wifiScan() goes straight to esp_wifi_scan_start and does not bring
        // the radio up on its own; wifiInit() is a no-op once it has run.
        GetHAL().wifiInit();

        std::vector<Hal::ScanResult_t> found;
        GetHAL().wifiScan(found);

        // Scan results arrive strongest first, so the first known network
        // in the list is also the best one on the air.
        for (const auto& ap : found) {
            if (_suspended) {
                break;
            }

            const auto match = std::find_if(known.begin(), known.end(),
                                            [&ap](const Entry_t& e) { return e.ssid == ap.second; });
            if (match == known.end()) {
                continue;
            }

            mclog::tagInfo(_tag, "rejoining \"{}\" ({} dBm)", match->ssid, ap.first);
            if (GetHAL().wifiConnect(match->ssid, match->password)) {
                mclog::tagInfo(_tag, "connected to \"{}\"", match->ssid);
                break;
            }
            mclog::tagWarn(_tag, "could not join \"{}\"", match->ssid);
        }

        if (!GetHAL().isWifiConnected()) {
            vTaskDelay(pdMS_TO_TICKS(kRetryIntervalMs));
        }
    }
}

}  // namespace

void remember(const std::string& ssid, const std::string& password)
{
    if (ssid.empty()) {
        return;
    }

    std::vector<Entry_t> entries = load();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&ssid](const Entry_t& e) { return e.ssid == ssid; }),
                  entries.end());

    entries.insert(entries.begin(), {ssid, password});
    if (static_cast<int>(entries.size()) > kMaxNetworks) {
        entries.resize(kMaxNetworks);
    }

    store(entries);
    mclog::tagInfo(_tag, "remembered \"{}\", {} known", ssid, entries.size());
}

bool password_for(const std::string& ssid, std::string& password)
{
    for (const auto& entry : load()) {
        if (entry.ssid == ssid) {
            password = entry.password;
            return true;
        }
    }
    return false;
}

std::vector<std::string> list()
{
    std::vector<std::string> names;
    for (const auto& entry : load()) {
        names.push_back(entry.ssid);
    }
    return names;
}

void forget(const std::string& ssid)
{
    std::vector<Entry_t> entries = load();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&ssid](const Entry_t& e) { return e.ssid == ssid; }),
                  entries.end());
    store(entries);
}

void start_auto_connect()
{
    if (_auto_connect_started) {
        return;
    }
    _auto_connect_started = true;
    xTaskCreate(auto_connect_task, "wifi_auto", 4096, nullptr, 3, nullptr);
}

void suspend_auto_connect()
{
    _suspended = true;
}

void resume_auto_connect()
{
    _suspended = false;
}

}  // namespace wifi_store
