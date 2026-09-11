/*
 * Irrigation engine.
 *
 * Ported from the controller that actually waters the garden: same
 * schedule format, same evaluation rule, same active-low relays. A slot
 * is on when it is active, today's weekday bit is set, and the current
 * time is at or after the start and before the end -- windows do not
 * cross midnight. A zone's relay follows its slots in automatic mode and
 * its manual switch in manual mode.
 *
 * With no trustworthy clock everything is off. Watering blind is worse
 * than not watering, and this board has no battery-backed clock, so
 * after a power cut it stays off until the network sets the time.
 *
 * Schedules live in NVS in the same text form the original used, so a
 * schedule can be moved between the two by copying a string:
 *   "HH:MM,HH:MM,LMMJVSD,A|" x4, one entry per slot, one key per zone.
 *
 * Everything here runs on the main loop; there is no locking.
 */
#pragma once

#include <cstdint>
#include <string>

namespace irrig {

constexpr int kZonesMax       = 4;
constexpr int kSlotsPerZone   = 4;
constexpr int kDaysPerWeek    = 7;

/* Day letters as the original wrote them: Monday first, Romanian
 * initials, a dash where the day is off. */
constexpr char kDayLetters[kDaysPerWeek + 1] = "LMMJVSD";

struct Slot_t {
    std::uint16_t start_minute;  // minutes into the day
    std::uint16_t end_minute;    // exclusive
    std::uint8_t days;           // bit 0 = Monday
    bool active;
};

enum class Mode : std::uint8_t {
    Auto,
    Manual,
};

/** @brief Load the schedules from NVS. Safe to call more than once. */
void init();

/**
 * @brief Re-read the schedules from NVS, discarding unsaved edits.
 *
 * Only the schedules: pins keep their assignment and relays keep their
 * state, so abandoning an edit never moves a valve.
 */
void reload_schedules();

int zone_count();
void set_zone_count(int count);

bool get_slot(int zone, int slot, Slot_t& out);
bool set_slot(int zone, int slot, const Slot_t& value);

/** @brief Write a zone's four slots back to NVS. */
void save_zone(int zone);

/** @brief Which GPIO drives this zone's relay, or -1 when unassigned. */
int zone_pin(int zone);
void set_zone_pin(int zone, int pin);

Mode mode();
void set_mode(Mode value);
void set_manual(int zone, bool on);
bool manual(int zone);

/** @brief What the relay is doing right now. */
bool zone_on(int zone);

/** @brief Whether this slot's window contains the current time today. */
bool slot_open_now(int zone, int slot);

/**
 * @brief Re-evaluate every zone and drive the relays.
 *
 * Called once a second by the daemon. Without a usable clock every zone
 * goes off, whatever the schedules say.
 */
void update();

/** @brief Drop every relay, for shutdown. */
void all_off();

/** @brief The zone's schedule in the text form NVS holds. */
std::string zone_text(int zone);
bool set_zone_text(int zone, const std::string& text);

}  // namespace irrig
