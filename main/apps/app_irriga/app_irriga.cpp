/*
 * SPDX-License-Identifier: MIT
 */
#include "app_irriga.h"

#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal.h>
#include <hal/utils/irrig/irrig.h>
#include <hal/utils/jobs/jobs.h>
#include <mooncake_log.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "assets/irriga_big.h"
#include "assets/irriga_small.h"

using namespace mooncake;

namespace {

/* The controller's own palette: black behind, white text, orange for
 * anything that is on. */
constexpr std::uint16_t kBg     = 0x0000;
constexpr std::uint16_t kText   = 0xFFFF;
constexpr std::uint16_t kActive = 0xFC00;
constexpr std::uint16_t kDim    = 0x8410;

/* Once a second. The clock only shows seconds, nothing else moves faster
 * than the schedule does, and drawing onto the panel while the remote
 * page reads it back shares one bus -- redrawing more often bought
 * nothing and showed as tearing. */
constexpr std::uint32_t kRefreshMs = 1000;

/* Twelve steps through a slot, as the original walked them: the two
 * times, the seven weekdays, then the active flag. */
constexpr int kFieldStartHour = 0;
constexpr int kFieldStartMin  = 1;
constexpr int kFieldEndHour   = 2;
constexpr int kFieldEndMin    = 3;
constexpr int kFieldFirstDay  = 4;
constexpr int kFieldActive    = 11;
constexpr int kFieldCount     = 12;

const char* const kDaysRo[7] = {"Dum", "Lun", "Mar", "Mie", "Joi", "Vin", "Sam"};

/* The pins a relay may be wired to on this board, and nothing else: the
 * rest carry the panel, the card, the keyboard or the radio, and handing
 * one of those to a valve would take the device down. G1 and G2 are the
 * Grove connector; 13 and 15 are the header pins the GPS unit uses. */
constexpr int kPinChoices[] = {-1, 1, 2, 13, 15};
constexpr int kPinChoiceCount = (int)(sizeof(kPinChoices) / sizeof(kPinChoices[0]));

bool local_now(struct tm& out)
{
    if (!GetHAL().isTimeSynced()) {
        return false;
    }
    time_t now = 0;
    time(&now);
    localtime_r(&now, &out);
    return true;
}

void slot_text(const irrig::Slot_t& slot, char* out, std::size_t len)
{
    char days[irrig::kDaysPerWeek + 1];
    for (int i = 0; i < irrig::kDaysPerWeek; i++) {
        days[i] = (slot.days & (1U << i)) ? irrig::kDayLetters[i] : '-';
    }
    days[irrig::kDaysPerWeek] = '\0';

    std::snprintf(out, len, "%02u:%02u-%02u:%02u %s %c", slot.start_minute / 60U,
                  slot.start_minute % 60U, slot.end_minute / 60U, slot.end_minute % 60U, days,
                  slot.active ? 'A' : '-');
}

}  // namespace

AppIrriga::AppIrriga()
{
    setAppInfo().name     = "irriga";
    setAppInfo().userData = new AppIcon_t(image_data_irriga_big, image_data_irriga_small);
}

AppIrriga::~AppIrriga()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

/* -------------------------------------------------------------------------- */
/*                                   Drawing                                   */
/* -------------------------------------------------------------------------- */

void AppIrriga::draw_header()
{
    auto& canvas = GetHAL().display;

    struct tm now = {};
    const bool has_time = local_now(now);

    canvas.setTextDatum(top_left);
    canvas.setTextSize(2);
    canvas.setTextColor(has_time ? kText : kDim, kBg);

    char text[32];
    if (has_time) {
        std::snprintf(text, sizeof(text), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
    } else {
        std::snprintf(text, sizeof(text), "--:--:--");
    }
    canvas.drawString(text, 2, 2);

    /* One lettered box per zone, filled while its relay is on -- the
     * badge the original carried in its own header. */
    const int count = irrig::zone_count();
    const int box   = 20;
    int x           = canvas.width() - (count * (box + 4)) - 2;
    const int boxes_left = x;
    for (int zone = 0; zone < count; zone++, x += box + 4) {
        const bool on = irrig::zone_on(zone);
        canvas.fillRect(x, 2, box, box, kBg);
        if (on) {
            canvas.fillRoundRect(x, 2, box, box, 3, kActive);
        } else {
            canvas.drawRoundRect(x, 2, box, box, 3, kText);
        }
        const char letter[2] = {(char)('A' + zone), '\0'};
        canvas.setTextSize(1);
        canvas.setTextColor(on ? kBg : kText, on ? kActive : kBg);
        canvas.drawString(letter, x + 7, 8);
    }

    /* Date on the left, address under the zone boxes on the same line. */
    canvas.setTextSize(1);
    canvas.setTextColor(has_time ? kText : kDim, kBg);
    if (has_time) {
        std::snprintf(text, sizeof(text), "%02d/%02d/%04d %s", now.tm_mday, now.tm_mon + 1,
                      now.tm_year + 1900, kDaysRo[now.tm_wday]);
    } else {
        std::snprintf(text, sizeof(text), "fara ora, nu se uda");
    }
    canvas.drawString(text, 2, 26);

    const std::string ip = GetHAL().getIpAddress();
    canvas.setTextDatum(top_right);
    canvas.setTextColor(ip.empty() ? kDim : kText, kBg);
    /* Cleared first: an address is not always the same width, and the
     * one before it must not show through. */
    canvas.fillRect(boxes_left - 4, 26, canvas.width() - boxes_left + 2, 8, kBg);
    canvas.drawString(ip.empty() ? "no network" : ip.c_str(), canvas.width() - 2, 26);
    canvas.setTextDatum(top_left);

    canvas.drawFastHLine(0, 37, canvas.width(), kDim);
}

void AppIrriga::draw_home()
{
    auto& canvas = GetHAL().display;
    draw_header();

    char text[32];
    std::snprintf(text, sizeof(text), "Zone %c", (char)('A' + _zone));
    canvas.setTextSize(1);
    canvas.setTextColor(irrig::zone_on(_zone) ? kActive : kText, kBg);
    canvas.drawString(text, 2, 43);

    canvas.setTextDatum(top_right);
    canvas.setTextColor(irrig::mode() == irrig::Mode::Manual ? kActive : kDim, kBg);
    canvas.fillRect(canvas.width() - 46, 43, 44, 8, kBg);
    canvas.drawString(irrig::mode() == irrig::Mode::Manual ? "MANUAL" : "AUTO", canvas.width() - 2, 43);
    canvas.setTextDatum(top_left);

    for (int slot = 0; slot < irrig::kSlotsPerZone; slot++) {
        irrig::Slot_t value = {};
        if (!irrig::get_slot(_zone, slot, value)) {
            continue;
        }
        slot_text(value, text, sizeof(text));

        /* Two pixels of leading and room for the marker on the left: the
         * four slots fill the panel between the rule and the hint. */
        const int y = 58 + slot * 14;

        /* The slot whose window holds the current time is inverted, so
         * what is watering right now reads at a glance. */
        const bool open    = irrig::slot_open_now(_zone, slot);
        const bool current = (slot == _slot);

        canvas.fillRect(0, y - 3, canvas.width(), 13, open ? kText : kBg);
        canvas.setTextColor(open ? kBg : (value.active ? kText : kDim), open ? kText : kBg);
        canvas.setTextSize(1);
        canvas.drawString(text, 12, y);

        if (current) {
            canvas.setTextColor(open ? kBg : kActive, open ? kText : kBg);
            canvas.drawString(">", 3, y);
        }
    }

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("Enter:edit M:manual P:pins", 2, canvas.height() - 9);
}

void AppIrriga::draw_manual()
{
    auto& canvas = GetHAL().display;
    draw_header();

    canvas.setTextSize(1);
    canvas.setTextColor(kActive, kBg);
    canvas.drawString("MANUAL", 2, 43);

    for (int zone = 0; zone < irrig::zone_count(); zone++) {
        const int y      = 58 + zone * 14;
        const bool on    = irrig::manual(zone);
        const bool cur   = (zone == _zone);

        char text[24];
        std::snprintf(text, sizeof(text), "Zone %c  %s", (char)('A' + zone), on ? "ON " : "OFF");

        canvas.setTextColor(on ? kActive : kText, kBg);
        canvas.drawString(text, 12, y);
        if (cur) {
            canvas.setTextColor(kActive, kBg);
            canvas.drawString(">", 2, y);
        }
    }

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("Enter:toggle  M:auto", 2, canvas.height() - 9);
}

void AppIrriga::draw_edit()
{
    auto& canvas = GetHAL().display;

    irrig::Slot_t value = {};
    irrig::get_slot(_zone, _slot, value);

    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);
    canvas.setTextColor(kText, kBg);

    char title[24];
    std::snprintf(title, sizeof(title), "Zone %c  slot %d", (char)('A' + _zone), _slot + 1);
    canvas.drawString(title, 2, 2);
    canvas.drawFastHLine(0, 13, canvas.width(), kDim);

    /* The whole slot on one line, the field being edited in the accent
     * colour: the same idea as the original's inverted field. */
    int x = 6;
    const int y = 24;
    char part[8];

    auto field = [&](int index, const char* text) {
        canvas.setTextColor(_field == index ? kActive : kText, kBg);
        canvas.drawString(text, x, y);
        x += (int)std::strlen(text) * 6;
    };
    auto plain = [&](const char* text) {
        canvas.setTextColor(kText, kBg);
        canvas.drawString(text, x, y);
        x += (int)std::strlen(text) * 6;
    };

    std::snprintf(part, sizeof(part), "%02u", value.start_minute / 60U);
    field(kFieldStartHour, part);
    plain(":");
    std::snprintf(part, sizeof(part), "%02u", value.start_minute % 60U);
    field(kFieldStartMin, part);
    plain("-");
    std::snprintf(part, sizeof(part), "%02u", value.end_minute / 60U);
    field(kFieldEndHour, part);
    plain(":");
    std::snprintf(part, sizeof(part), "%02u", value.end_minute % 60U);
    field(kFieldEndMin, part);
    plain(" ");

    for (int day = 0; day < irrig::kDaysPerWeek; day++) {
        const bool set = (value.days & (1U << day)) != 0;
        part[0] = set ? irrig::kDayLetters[day] : '-';
        part[1] = '\0';
        field(kFieldFirstDay + day, part);
    }
    plain(" ");
    part[0] = value.active ? 'A' : '-';
    part[1] = '\0';
    field(kFieldActive, part);

    /* What the field being edited actually is, spelled out: a single
     * highlighted character is not obvious on a panel this size. */
    static const char* const kFieldNames[kFieldCount] = {
        "start hour", "start minute", "end hour", "end minute",
        "Monday",     "Tuesday",      "Wednesday", "Thursday",
        "Friday",     "Saturday",     "Sunday",    "slot active",
    };
    /* Wiped before it is written: these names are different lengths --
     * "start minute" against "end hour" -- and opaque text only covers
     * its own glyphs, so the tail of the longer one stayed on screen
     * under the shorter one as the fields were walked. */
    canvas.fillRect(0, 42, canvas.width(), canvas.fontHeight(), kBg);
    canvas.setTextColor(kActive, kBg);
    canvas.drawString(kFieldNames[_field], 6, 42);

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("Tab/Enter: next field", 6, 62);
    canvas.drawString("Up/Down or +/-: change", 6, 73);
    canvas.drawString("S: save    Esc: cancel", 6, 84);
}

void AppIrriga::draw_pins()
{
    auto& canvas = GetHAL().display;

    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);
    canvas.setTextColor(kText, kBg);
    canvas.drawString("Relay pins", 2, 2);
    canvas.drawFastHLine(0, 13, canvas.width(), kDim);

    for (int zone = 0; zone < irrig::zone_count(); zone++) {
        const int y   = 20 + zone * 13;
        const int pin = irrig::zone_pin(zone);

        char text[32];
        if (pin < 0) {
            std::snprintf(text, sizeof(text), "Zone %c  not wired", (char)('A' + zone));
        } else {
            std::snprintf(text, sizeof(text), "Zone %c  GPIO %d", (char)('A' + zone), pin);
        }

        canvas.setTextColor(pin >= 0 ? kText : kDim, kBg);
        canvas.drawString(text, 12, y);
        if (zone == _zone) {
            canvas.setTextColor(kActive, kBg);
            canvas.drawString(">", 2, y);
        }
    }

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("Left/Right: pin", 2, canvas.height() - 20);
    canvas.drawString("Relays are active low", 2, canvas.height() - 9);
}

void AppIrriga::cycle_pin(int delta)
{
    const int current = irrig::zone_pin(_zone);

    int index = 0;
    for (int i = 0; i < kPinChoiceCount; i++) {
        if (kPinChoices[i] == current) {
            index = i;
            break;
        }
    }

    index = (index + delta + kPinChoiceCount) % kPinChoiceCount;
    irrig::set_zone_pin(_zone, kPinChoices[index]);
    _dirty = true;
}

void AppIrriga::draw()
{
    auto& canvas = GetHAL().display;
    canvas.setFont(&fonts::Font0);

    /* Only when the view changes: every field paints its own background,
     * so a clear on each refresh would be a flicker for nothing. */
    if (_needs_clear) {
        canvas.fillScreen(kBg);
        _needs_clear = false;
    }

    switch (_screen) {
        case SCREEN_MANUAL:
            draw_manual();
            break;
        case SCREEN_EDIT:
            draw_edit();
            break;
        case SCREEN_PINS:
            draw_pins();
            break;
        default:
            draw_home();
            break;
    }

    _last_draw_ms = GetHAL().millis();
    _dirty        = false;
}

/* -------------------------------------------------------------------------- */
/*                                   Editing                                   */
/* -------------------------------------------------------------------------- */

void AppIrriga::adjust(int delta)
{
    irrig::Slot_t value = {};
    if (!irrig::get_slot(_zone, _slot, value)) {
        return;
    }

    int hour   = value.start_minute / 60;
    int minute = value.start_minute % 60;
    int e_hour = value.end_minute / 60;
    int e_min  = value.end_minute % 60;

    switch (_field) {
        case kFieldStartHour:
            hour = (hour + delta + 24) % 24;
            break;
        case kFieldStartMin:
            minute = (minute + delta + 60) % 60;
            break;
        case kFieldEndHour:
            e_hour = (e_hour + delta + 24) % 24;
            break;
        case kFieldEndMin:
            e_min = (e_min + delta + 60) % 60;
            break;
        case kFieldActive:
            value.active = !value.active;
            break;
        default:
            if (_field >= kFieldFirstDay && _field < kFieldActive) {
                value.days ^= (std::uint8_t)(1U << (_field - kFieldFirstDay));
            }
            break;
    }

    value.start_minute = (std::uint16_t)(hour * 60 + minute);
    value.end_minute   = (std::uint16_t)(e_hour * 60 + e_min);
    irrig::set_slot(_zone, _slot, value);
    _dirty = true;
}

void AppIrriga::handle_char(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        ch = (char)(ch - 'A' + 'a');
    }

    if (_screen == SCREEN_EDIT) {
        switch (ch) {
            case '+':
            case '=':
                adjust(1);
                return;
            case '-':
                adjust(-1);
                return;
            case 's':
                irrig::save_zone(_zone);
                irrig::update();
                show(SCREEN_HOME);
                audio::play_random_tone();
                return;
            default:
                return;
        }
    }

    switch (ch) {
        case 'm':
            /* Manual is a mode of the engine, not of this application:
             * leaving the screen does not put the valves back. */
            if (irrig::mode() == irrig::Mode::Manual) {
                irrig::set_mode(irrig::Mode::Auto);
                show(SCREEN_HOME);
            } else {
                irrig::set_mode(irrig::Mode::Manual);
                show(SCREEN_MANUAL);
            }
            break;
        case 'p':
            show(_screen == SCREEN_PINS ? SCREEN_HOME : SCREEN_PINS);
            break;
        default:
            break;
    }
}

/* Any change of view leaves the previous one's pixels behind, so the
 * next draw starts from a cleared panel. */
void AppIrriga::show(std::uint8_t screen)
{
    if (_screen != screen) {
        _screen      = screen;
        _needs_clear = true;
    }
    _dirty = true;
}

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                  */
/* -------------------------------------------------------------------------- */

void AppIrriga::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    /* The engine belongs to the daemon; opening this must not be what
     * brings it up, but a stopped daemon leaves nothing to show. */
    irrig::init();
    if (!jobs::is_running("irrigd")) {
        mclog::tagWarn(getAppInfo().name, "irrigd is stopped, schedules will not run");
    }

    /* The whole panel: this screen carries its own clock, date and
     * address, so the launcher's bars would only take room from the
     * schedule and repeat what is already there. */
    GetHAL().setFullScreenApp(true);

    _screen      = SCREEN_HOME;
    _zone        = 0;
    _slot        = 0;
    _field       = 0;
    _dirty       = true;
    _needs_clear = true;

    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& key) {
        if (!key.state) {
            return;
        }
        const bool up    = (key.row == 2 && key.col == 11);
        const bool down  = (key.row == 3 && key.col == 11);
        const bool left  = (key.row == 3 && key.col == 10);
        const bool right = (key.row == 3 && key.col == 12);
        if (!up && !down && !left && !right) {
            return;
        }

        if (_screen == SCREEN_EDIT) {
            if (up) {
                adjust(1);
            } else if (down) {
                adjust(-1);
            } else {
                _field = (_field + (right ? 1 : kFieldCount - 1)) % kFieldCount;
                _dirty = true;
            }
            return;
        }

        if (_screen == SCREEN_MANUAL || _screen == SCREEN_PINS) {
            if (up || down) {
                const int count = irrig::zone_count();
                _zone           = (_zone + (up ? count - 1 : 1)) % count;
                _dirty          = true;
            } else if (_screen == SCREEN_PINS) {
                cycle_pin(right ? 1 : -1);
            }
            return;
        }

        if (up || down) {
            _slot  = (_slot + (up ? irrig::kSlotsPerZone - 1 : 1)) % irrig::kSlotsPerZone;
            _dirty = true;
        } else {
            const int count = irrig::zone_count();
            _zone           = (_zone + (left ? count - 1 : 1)) % count;
            _dirty          = true;
        }
    });

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& key) {
        if (key.isModifier || !key.state) {
            return;
        }

        switch (key.keyCode) {
            case KEY_UP:
            case KEY_DOWN:
            case KEY_LEFT:
            case KEY_RIGHT:
                return;  // Already delivered by the raw handler.

            case KEY_ESC:
                if (_screen == SCREEN_HOME) {
                    _close_requested = true;
                } else {
                    /* Leaving the editor without saving puts back what
                     * NVS still holds. */
                    if (_screen == SCREEN_EDIT) {
                        irrig::reload_schedules();
                    }
                    show(SCREEN_HOME);
                }
                return;

            case KEY_TAB:
                if (_screen == SCREEN_EDIT) {
                    _field = (_field + 1) % kFieldCount;
                    _dirty = true;
                }
                return;

            case KEY_ENTER:
                if (_screen == SCREEN_EDIT) {
                    _field = (_field + 1) % kFieldCount;
                    _dirty = true;
                } else if (_screen == SCREEN_MANUAL) {
                    irrig::set_manual(_zone, !irrig::manual(_zone));
                    _dirty = true;
                } else {
                    _field = 0;
                    show(SCREEN_EDIT);
                }
                return;

            default:
                break;
        }

        if (key.keyName != nullptr && key.keyName[0] != '\0' && key.keyName[1] == '\0') {
            handle_char(key.keyName[0]);
        }
    });
}

void AppIrriga::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        audio::play_random_tone();
        close();
        return;
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    /* The clock ticks and the relays follow the schedule while this is
     * open, so the view refreshes on its own beat as well as on a key. */
    if (_dirty || (GetHAL().millis() - _last_draw_ms) >= kRefreshMs) {
        draw();
    }
}

void AppIrriga::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    if (_key_raw_slot >= 0) {
        GetHAL().keyboard.onKeyEventRaw.disconnect(_key_raw_slot);
        _key_raw_slot = -1;
    }

    /* Giving the panel back also builds the launcher's sprites again,
     * empty; it notices and redraws them itself. */
    GetHAL().setFullScreenApp(false);
}
