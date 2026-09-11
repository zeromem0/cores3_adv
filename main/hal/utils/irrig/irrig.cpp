/*
 * SPDX-License-Identifier: MIT
 */
#include "irrig.h"

#include <driver/gpio.h>
#include <mooncake_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace irrig {

namespace {

const std::string _tag = "irrig";

constexpr char kNamespace[] = "sched";
constexpr int kMinutesPerDay = 24 * 60;

/* The schedules the original shipped with, kept as the defaults so a
 * fresh device waters on a sensible programme rather than not at all. */
const char* const kDefaultZones[kZonesMax] = {
    "12:00,12:30,LMMJVS-,A|13:00,13:30,LMMJVSD,A|20:00,20:30,LMMJVSD,A|20:30,21:00,LMMJVS-,A",
    "14:00,14:30,LMMJVSD,A|14:30,15:00,LMMJVSD,A|15:00,15:30,LMMJVSD,A|15:30,16:00,LMMJVSD,A",
    "16:00,16:30,LMMJVSD,A|16:30,17:00,LMMJVSD,A|17:00,17:30,LMMJVSD,A|17:30,18:00,LMMJVSD,A",
    "18:00,18:30,LMMJVSD,A|18:30,19:00,LMMJVSD,A|19:00,19:30,LMMJVSD,A|19:30,20:00,LMMJVSD,A",
};

struct Zone_t {
    Slot_t slots[kSlotsPerZone];
    int pin;
    bool manual_on;
    bool output_on;
};

Zone_t _zones[kZonesMax];
/* Four, as the controller this came from has. It was one here while the
 * screen was a Cardputer's and could show a single zone at a time; this
 * panel is the original's own size and holds all four at once, and the
 * four schedules above were always there waiting for them. A zone with
 * no pin assigned drives nothing, so the three extra ones cost a card on
 * screen and not a valve. */
int _zone_count = kZonesMax;
Mode _mode      = Mode::Auto;
bool _ready;

bool parse_slot(const std::string& text, Slot_t& out)
{
    /* "HH:MM,HH:MM,LMMJVSD,A" -- exactly what the original wrote. */
    unsigned sh = 0, sm = 0, eh = 0, em = 0;
    char days[16] = {0};
    char active   = '-';

    if (std::sscanf(text.c_str(), "%2u:%2u,%2u:%2u,%7[^,],%c", &sh, &sm, &eh, &em, days, &active) != 6) {
        return false;
    }

    out.start_minute = (std::uint16_t)((sh % 24) * 60 + (sm % 60));
    out.end_minute   = (std::uint16_t)((eh % 24) * 60 + (em % 60));
    out.active       = (active == 'A');
    out.days         = 0;
    for (int i = 0; i < kDaysPerWeek && days[i] != '\0'; i++) {
        if (days[i] != '-') {
            out.days |= (std::uint8_t)(1U << i);
        }
    }
    return true;
}

std::string format_slot(const Slot_t& slot)
{
    char days[kDaysPerWeek + 1];
    for (int i = 0; i < kDaysPerWeek; i++) {
        days[i] = (slot.days & (1U << i)) ? kDayLetters[i] : '-';
    }
    days[kDaysPerWeek] = '\0';

    char text[32];
    std::snprintf(text, sizeof(text), "%02u:%02u,%02u:%02u,%s,%c", slot.start_minute / 60U,
                  slot.start_minute % 60U, slot.end_minute / 60U, slot.end_minute % 60U, days,
                  slot.active ? 'A' : '-');
    return text;
}

/* Read one zone's schedule text out of an open handle, leaving what is
 * already loaded alone when the key is missing. */
bool read_zone_text(nvs_handle_t handle, int zone, std::string& out)
{
    char key[8];
    std::snprintf(key, sizeof(key), "s%d", zone);

    std::size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0 || length > 256) {
        return false;
    }

    out.assign(length, '\0');
    if (nvs_get_str(handle, key, &out[0], &length) != ESP_OK) {
        return false;
    }
    out.resize(length > 0 ? length - 1 : 0);
    return true;
}

void load_zone(int zone, const std::string& text)
{
    std::size_t pos = 0;
    for (int slot = 0; slot < kSlotsPerZone; slot++) {
        const std::size_t bar = text.find('|', pos);
        const std::string entry =
            (bar == std::string::npos) ? text.substr(pos) : text.substr(pos, bar - pos);

        if (!parse_slot(entry, _zones[zone].slots[slot])) {
            _zones[zone].slots[slot] = {0, 0, 0, false};
        }
        if (bar == std::string::npos) {
            break;
        }
        pos = bar + 1;
    }
}

void drive(int zone, bool on)
{
    Zone_t& z = _zones[zone];
    if (z.output_on == on) {
        return;
    }
    z.output_on = on;

    if (z.pin >= 0) {
        /* Active low, like the relay boards these drive. */
        gpio_set_level((gpio_num_t)z.pin, on ? 0 : 1);
    }
    mclog::tagInfo(_tag, "zone {} {}", (char)('A' + zone), on ? "on" : "off");
}

bool now_local(struct tm& out)
{
    /* Anything before 2026 means the clock was never set; see the HAL. */
    time_t now = 0;
    time(&now);
    if (now < 1767225600) {
        return false;
    }
    localtime_r(&now, &out);
    return true;
}

}  // namespace

void init()
{
    if (_ready) {
        return;
    }

    for (int zone = 0; zone < kZonesMax; zone++) {
        _zones[zone].pin       = -1;
        _zones[zone].manual_on = false;
        _zones[zone].output_on = false;
        load_zone(zone, kDefaultZones[zone]);
    }

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        for (int zone = 0; zone < kZonesMax; zone++) {
            std::string text;
            if (read_zone_text(handle, zone, text)) {
                load_zone(zone, text);
            }

            char key[8];
            std::snprintf(key, sizeof(key), "p%d", zone);
            std::int32_t pin = -1;
            if (nvs_get_i32(handle, key, &pin) == ESP_OK) {
                _zones[zone].pin = pin;
            }
        }

        std::int32_t count = _zone_count;
        if (nvs_get_i32(handle, "zones", &count) == ESP_OK && count >= 1 && count <= kZonesMax) {
            _zone_count = count;
        }
        nvs_close(handle);
    }

    /* Every assigned pin starts as an output held high, which is a relay
     * that is off. */
    for (int zone = 0; zone < kZonesMax; zone++) {
        if (_zones[zone].pin >= 0) {
            gpio_config_t cfg = {};
            cfg.pin_bit_mask  = 1ULL << _zones[zone].pin;
            cfg.mode          = GPIO_MODE_OUTPUT;
            gpio_config(&cfg);
            gpio_set_level((gpio_num_t)_zones[zone].pin, 1);
        }
    }

    _ready = true;
    mclog::tagInfo(_tag, "ready, {} zones", _zone_count);
}

void reload_schedules()
{
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    for (int zone = 0; zone < kZonesMax; zone++) {
        std::string text;
        if (read_zone_text(handle, zone, text)) {
            load_zone(zone, text);
        } else {
            load_zone(zone, kDefaultZones[zone]);
        }
    }
    nvs_close(handle);
}

int zone_count()
{
    return _zone_count;
}

void set_zone_count(int count)
{
    if (count < 1 || count > kZonesMax) {
        return;
    }
    _zone_count = count;

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "zones", count);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool get_slot(int zone, int slot, Slot_t& out)
{
    if (zone < 0 || zone >= kZonesMax || slot < 0 || slot >= kSlotsPerZone) {
        return false;
    }
    out = _zones[zone].slots[slot];
    return true;
}

bool set_slot(int zone, int slot, const Slot_t& value)
{
    if (zone < 0 || zone >= kZonesMax || slot < 0 || slot >= kSlotsPerZone) {
        return false;
    }
    _zones[zone].slots[slot] = value;
    return true;
}

std::string zone_text(int zone)
{
    if (zone < 0 || zone >= kZonesMax) {
        return "";
    }
    std::string text;
    for (int slot = 0; slot < kSlotsPerZone; slot++) {
        if (slot > 0) {
            text += '|';
        }
        text += format_slot(_zones[zone].slots[slot]);
    }
    return text;
}

bool set_zone_text(int zone, const std::string& text)
{
    if (zone < 0 || zone >= kZonesMax) {
        return false;
    }
    load_zone(zone, text);
    save_zone(zone);
    return true;
}

void save_zone(int zone)
{
    if (zone < 0 || zone >= kZonesMax) {
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        mclog::tagError(_tag, "cannot open nvs for writing");
        return;
    }

    char key[8];
    std::snprintf(key, sizeof(key), "s%d", zone);
    nvs_set_str(handle, key, zone_text(zone).c_str());
    nvs_commit(handle);
    nvs_close(handle);
}

int zone_pin(int zone)
{
    return (zone >= 0 && zone < kZonesMax) ? _zones[zone].pin : -1;
}

void set_zone_pin(int zone, int pin)
{
    if (zone < 0 || zone >= kZonesMax) {
        return;
    }

    /* Let go of the old pin before claiming a new one, and leave it in
     * the state a relay reads as off. */
    if (_zones[zone].pin >= 0) {
        gpio_set_level((gpio_num_t)_zones[zone].pin, 1);
        gpio_reset_pin((gpio_num_t)_zones[zone].pin);
    }

    _zones[zone].pin       = pin;
    _zones[zone].output_on = false;

    if (pin >= 0) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask  = 1ULL << pin;
        cfg.mode          = GPIO_MODE_OUTPUT;
        gpio_config(&cfg);
        gpio_set_level((gpio_num_t)pin, 1);
    }

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        char key[8];
        std::snprintf(key, sizeof(key), "p%d", zone);
        nvs_set_i32(handle, key, pin);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

Mode mode()
{
    return _mode;
}

void set_mode(Mode value)
{
    _mode = value;
    update();
}

void set_manual(int zone, bool on)
{
    if (zone < 0 || zone >= kZonesMax) {
        return;
    }
    _zones[zone].manual_on = on;
    update();
}

bool manual(int zone)
{
    return (zone >= 0 && zone < kZonesMax) && _zones[zone].manual_on;
}

bool zone_on(int zone)
{
    return (zone >= 0 && zone < kZonesMax) && _zones[zone].output_on;
}

bool slot_open_now(int zone, int slot)
{
    if (zone < 0 || zone >= kZonesMax || slot < 0 || slot >= kSlotsPerZone) {
        return false;
    }

    struct tm now = {};
    if (!now_local(now)) {
        return false;
    }

    const Slot_t& s = _zones[zone].slots[slot];
    if (!s.active) {
        return false;
    }

    /* tm_wday counts from Sunday; the schedule counts from Monday. */
    const int weekday = (now.tm_wday == 0) ? 6 : now.tm_wday - 1;
    if ((s.days & (1U << weekday)) == 0) {
        return false;
    }

    const int minute = now.tm_hour * 60 + now.tm_min;
    return minute >= (int)s.start_minute && minute < (int)s.end_minute;
}

void update()
{
    struct tm now = {};
    const bool has_clock = now_local(now);

    for (int zone = 0; zone < _zone_count; zone++) {
        bool wanted = false;

        if (_mode == Mode::Manual) {
            wanted = _zones[zone].manual_on;
        } else if (has_clock) {
            for (int slot = 0; slot < kSlotsPerZone; slot++) {
                if (slot_open_now(zone, slot)) {
                    wanted = true;
                    break;
                }
            }
        }

        drive(zone, wanted);
    }
}

void all_off()
{
    for (int zone = 0; zone < kZonesMax; zone++) {
        _zones[zone].manual_on = false;
        drive(zone, false);
    }
}

}  // namespace irrig
