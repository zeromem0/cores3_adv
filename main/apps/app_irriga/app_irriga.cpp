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
#include <hal/utils/touch_keys/touch_keys.h>
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

/*
 * The original's geometry, kept to the pixel. The panel is the same size
 * and the numbers were chosen for it: a header across the top and four
 * cards two by two under it, with nothing left over.
 */
constexpr int kHeaderX = 5;
constexpr int kHeaderY = 0;
constexpr int kHeaderW = 310;
constexpr int kHeaderH = 50;

constexpr int kBtnW  = 150;
constexpr int kBtnH  = 85;
/* Two buttons to a quarter, with the original's own six pixels between
 * them: 72 and 72 make the 150 a whole one is. */
constexpr int kHalfW = 72;
constexpr int kBtnX1 = 5;
constexpr int kBtnX2 = 165;
constexpr int kBtnY1 = 55;
constexpr int kBtnY2 = 148;

/* The BACK button, in the header's own coordinates and again in the
 * panel's, because the drawing and the hit test have to agree. */
constexpr int kBackX = 0;
constexpr int kBackY = 10;
constexpr int kBackW = 100;
constexpr int kBackH = 32;

/* Once a second. The clock shows seconds, nothing else moves faster than
 * the schedule does, and drawing onto the panel while the remote page
 * reads it back shares one bus -- redrawing more often bought nothing
 * and showed as tearing. */
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

/* Whether a tap landed inside a rectangle. Written once because every
 * screen here is rectangles. */
bool inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/*
 * Which of the four quarters a tap is in, or -1.
 *
 * Numbered down the left column and then down the right, which is the
 * order the original filled them in and the order the zones sit in: A
 * and B on the left, C and D on the right. The cards, the menu buttons
 * and the zone buttons all share these four places, so one answer serves
 * every screen and a quarter's number is a zone's number without a table
 * in between.
 */
int quadrant_at(int x, int y)
{
    if (inside(x, y, kBtnX1, kBtnY1, kBtnW, kBtnH)) return 0;
    if (inside(x, y, kBtnX1, kBtnY2, kBtnW, kBtnH)) return 1;
    if (inside(x, y, kBtnX2, kBtnY1, kBtnW, kBtnH)) return 2;
    if (inside(x, y, kBtnX2, kBtnY2, kBtnW, kBtnH)) return 3;
    return -1;
}

int quadrant_x(int index)
{
    return (index < 2) ? kBtnX1 : kBtnX2;
}

int quadrant_y(int index)
{
    return (index % 2 == 0) ? kBtnY1 : kBtnY2;
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

/*
 * The band across the top.
 *
 * On the home screen it is the clock, the date, the address and a badge
 * per zone. Anywhere else the clock's corner becomes the way back, and
 * the right half either keeps the badges -- which is what the manual
 * screen wants, since it is the screen that turns them on -- or carries
 * the name of where you are.
 */
void AppIrriga::draw_header(bool back, const char* title, bool with_zones)
{
    auto& s = _header;
    if (s.width() <= 0) {
        return;
    }
    s.fillScreen(kBg);
    s.setFont(&fonts::Font0);

    struct tm now        = {};
    const bool has_time  = local_now(now);
    const int zones_x    = 195;
    const int zones_y    = 22;

    if (back) {
        s.drawRoundRect(kBackX, kBackY, kBackW, kBackH, 4, kText);
        s.setTextColor(kText, kBg);
        s.setTextSize(2);
        s.setCursor(kBackX + 15, kBackY + 8);
        s.print("< BACK");
    } else {
        s.setTextSize(3);
        s.setTextColor(has_time ? kText : kDim, kBg);
        s.setCursor(0, 4);
        if (has_time) {
            s.printf("%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
        } else {
            s.print("FARA ORA");
        }

        s.setTextSize(1);
        s.setCursor(0, 32);
        if (has_time) {
            s.printf("%02d/%02d/%04d %s", now.tm_mday, now.tm_mon + 1, now.tm_year + 1900,
                     kDaysRo[now.tm_wday]);
        } else {
            s.print("nu se uda fara ora");
        }
    }

    if (!back || with_zones) {
        const std::string ip = GetHAL().getIpAddress();
        s.setTextSize(1);
        s.setTextColor(ip.empty() ? kDim : kText, kBg);
        s.setCursor(zones_x, 5);
        s.print(ip.empty() ? "no network" : ("IP:" + ip).c_str());

        const int count = irrig::zone_count();
        for (int i = 0; i < count && i < 4; i++) {
            const int x  = zones_x + i * 27;
            const bool on = irrig::zone_on(i);
            if (on) {
                s.fillRoundRect(x, zones_y, 23, 22, 4, kActive);
                s.setTextColor(kBg, kActive);
            } else {
                s.drawRoundRect(x, zones_y, 23, 22, 4, kText);
                s.setTextColor(kText, kBg);
            }
            s.setTextSize(2);
            s.setCursor(x + 6, zones_y + 4);
            s.printf("%c", (char)('A' + i));
        }
    } else if (title != nullptr && title[0] != '\0') {
        /* Right-aligned, so a long name and a short one both end where
         * the badges would have. */
        s.setTextSize(2);
        s.setTextColor(kActive, kBg);
        int x = kHeaderW - (int)std::strlen(title) * 12 - 5;
        if (x < kBackX + kBackW + 10) {
            x = kBackX + kBackW + 10;
        }
        s.setCursor(x, kBackY + 8);
        s.print(title);
    }

    s.pushSprite(kHeaderX, kHeaderY);
}

/*
 * One zone, as a card: its name, and its four slots one to a line.
 *
 * The slot whose window holds the current time is drawn inverted, which
 * is what tells you across a garden which one is running. While the zone
 * is being edited the same card carries the cursor instead, with the
 * field being changed in the accent colour.
 */
void AppIrriga::draw_zone_card(int zone, int x, int y, bool editing)
{
    auto& s = _card;
    if (s.width() <= 0) {
        return;
    }
    s.fillScreen(kBg);
    s.setFont(&fonts::Font0);

    const bool on            = irrig::zone_on(zone);
    const std::uint16_t edge = on ? kActive : kText;

    s.drawRoundRect(0, 0, kBtnW, kBtnH, 4, edge);

    s.setTextSize(1);
    s.setTextColor(on ? kActive : kText, kBg);
    s.setCursor(16, 5);
    s.printf("Zone %c", (char)('A' + zone));

    for (int slot = 0; slot < irrig::kSlotsPerZone; slot++) {
        irrig::Slot_t value = {};
        if (!irrig::get_slot(zone, slot, value)) {
            continue;
        }

        const int row = 20 + slot * 16;

        if (editing && slot == _slot) {
            /* The row being edited, written field by field so the one
             * under the cursor can be told apart. */
            s.setCursor(10, row);
            auto part = [&](int index, const char* text) {
                s.setTextColor(_field == index ? kActive : kText, kBg);
                s.print(text);
            };
            auto plain = [&](const char* text) {
                s.setTextColor(kText, kBg);
                s.print(text);
            };

            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02u", value.start_minute / 60U);
            part(kFieldStartHour, buf);
            plain(":");
            std::snprintf(buf, sizeof(buf), "%02u", value.start_minute % 60U);
            part(kFieldStartMin, buf);
            plain("-");
            std::snprintf(buf, sizeof(buf), "%02u", value.end_minute / 60U);
            part(kFieldEndHour, buf);
            plain(":");
            std::snprintf(buf, sizeof(buf), "%02u", value.end_minute % 60U);
            part(kFieldEndMin, buf);
            plain(" ");

            for (int day = 0; day < irrig::kDaysPerWeek; day++) {
                buf[0] = (value.days & (1U << day)) ? irrig::kDayLetters[day] : '-';
                buf[1] = '\0';
                part(kFieldFirstDay + day, buf);
            }
            plain(" ");
            buf[0] = value.active ? 'A' : '-';
            buf[1] = '\0';
            part(kFieldActive, buf);
            continue;
        }

        char text[32];
        slot_text(value, text, sizeof(text));

        const bool open = !editing && irrig::slot_open_now(zone, slot);
        if (open) {
            s.fillRect(6, row - 3, kBtnW - 12, 14, kText);
        }
        s.setTextColor(open ? kBg : (value.active ? kText : kDim), open ? kText : kBg);
        s.setCursor(10, row);
        s.print(text);
    }

    s.pushSprite(x, y);
}

void AppIrriga::draw_button(const char* label, int x, int y, int w, bool filled,
                            std::uint16_t colour, bool focused)
{
    auto& canvas = GetHAL().display;

    canvas.fillRect(x, y, w, kBtnH, kBg);
    if (filled) {
        canvas.fillRoundRect(x, y, w, kBtnH, 4, colour);
        canvas.setTextColor(kBg, colour);
    } else {
        canvas.drawRoundRect(x, y, w, kBtnH, 4, colour);
        canvas.setTextColor(colour, kBg);
    }

    /* Where the arrow keys are pointing, drawn inside the button's own
     * edge so that it cannot touch the one beside it. A button that is
     * already filled shows it in the background colour, which is the
     * only thing that reads against the fill. */
    if (focused) {
        canvas.drawRoundRect(x + 3, y + 3, w - 6, kBtnH - 6, 4, filled ? kBg : kActive);
        canvas.drawRoundRect(x + 4, y + 4, w - 8, kBtnH - 8, 3, filled ? kBg : kActive);
    }

    canvas.setTextSize(2);
    canvas.setTextDatum(middle_center);
    canvas.drawString(label, x + w / 2, y + kBtnH / 2);
    canvas.setTextDatum(top_left);
}

void AppIrriga::draw_home()
{
    draw_header(false, "", true);

    const int count = irrig::zone_count();
    for (int zone = 0; zone < 4; zone++) {
        const int x = quadrant_x(zone);
        const int y = quadrant_y(zone);
        if (zone < count) {
            draw_zone_card(zone, x, y, false);
        } else {
            GetHAL().display.fillRect(x, y, kBtnW, kBtnH, kBg);
        }
    }
}

void AppIrriga::draw_menu()
{
    draw_header(true, "Menu", false);
    draw_button("Automat", kBtnX1, kBtnY1, kBtnW, false, kText, _focus == 0);
    draw_button("Manual", kBtnX1, kBtnY2, kBtnW, false,
                irrig::mode() == irrig::Mode::Manual ? kActive : kText, _focus == 1);
    draw_button("Edit Zone", kBtnX2, kBtnY1, kBtnW, false, kText, _focus == 2);
    draw_button("Pins", kBtnX2, kBtnY2, kBtnW, false, kText, _focus == 3);
}

void AppIrriga::draw_manual()
{
    /* The badges stay: this is the screen that turns them on, and the
     * answer belongs next to the question. */
    draw_header(true, "", true);

    for (int zone = 0; zone < 4; zone++) {
        const int x = quadrant_x(zone);
        const int y = quadrant_y(zone);
        if (zone >= irrig::zone_count()) {
            GetHAL().display.fillRect(x, y, kBtnW, kBtnH, kBg);
            continue;
        }
        char label[12];
        std::snprintf(label, sizeof(label), "Zone %c", (char)('A' + zone));
        const bool on = irrig::manual(zone);
        draw_button(label, x, y, kBtnW, on, on ? kActive : kText, _focus == zone);
    }
}

void AppIrriga::draw_zone_select()
{
    draw_header(true, "Select Zone", false);

    for (int zone = 0; zone < 4; zone++) {
        const int x = quadrant_x(zone);
        const int y = quadrant_y(zone);
        if (zone >= irrig::zone_count()) {
            GetHAL().display.fillRect(x, y, kBtnW, kBtnH, kBg);
            continue;
        }
        char label[12];
        std::snprintf(label, sizeof(label), "Zone %c", (char)('A' + zone));
        draw_button(label, x, y, kBtnW, false, kText, _focus == zone);
    }
}

void AppIrriga::draw_edit()
{
    char title[16];
    std::snprintf(title, sizeof(title), "Edit %c", (char)('A' + _zone));
    draw_header(true, title, false);

    draw_zone_card(_zone, kBtnX1, kBtnY1, true);

    draw_button("SET", kBtnX2, kBtnY1, kBtnW, false, kActive);

    /* The walk both ways, sharing a quarter the way the two steps do.
     * The original had only NEXT, which is forty-seven presses to reach
     * the field before the one you are on. */
    draw_button("PREV", kBtnX1, kBtnY2, kHalfW, false, kText);
    draw_button("NEXT", kBtnX1 + kHalfW + 6, kBtnY2, kHalfW, false, kText);
    draw_button("+1", kBtnX2, kBtnY2, kHalfW, false, kText);
    draw_button("-1", kBtnX2 + kHalfW + 6, kBtnY2, kHalfW, false, kText);
}

void AppIrriga::draw_pins()
{
    auto& canvas = GetHAL().display;

    draw_header(true, "Pins", false);

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);

    for (int zone = 0; zone < irrig::zone_count(); zone++) {
        const int y   = 62 + zone * 20;
        const int pin = irrig::zone_pin(zone);

        char text[40];
        if (pin < 0) {
            std::snprintf(text, sizeof(text), "Zone %c   not wired", (char)('A' + zone));
        } else {
            std::snprintf(text, sizeof(text), "Zone %c   GPIO %d", (char)('A' + zone), pin);
        }

        canvas.fillRect(0, y - 4, 160, 18, kBg);
        canvas.setTextColor(pin >= 0 ? kText : kDim, kBg);
        canvas.setTextSize(2);
        canvas.drawString(text, 16, y);
        canvas.setTextColor(kActive, kBg);
        canvas.drawString(zone == _zone ? ">" : " ", 4, y);
    }

    canvas.setTextSize(1);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("Relays are active low", 16, 150);

    draw_button("NEXT", kBtnX1, kBtnY2, kBtnW, false, kText);
    draw_button("+1", kBtnX2, kBtnY2, kHalfW, false, kText);
    draw_button("-1", kBtnX2 + kHalfW + 6, kBtnY2, kHalfW, false, kText);
}

void AppIrriga::draw()
{
    auto& canvas = GetHAL().display;
    canvas.setFont(&fonts::Font0);

    /* Only when the view changes: the header and the cards are sprites
     * that cover their own ground, so a clear on every refresh would be
     * a flicker for nothing. */
    if (_needs_clear) {
        canvas.fillScreen(kBg);
        _needs_clear = false;
    }

    switch (_screen) {
        case SCREEN_MENU:
            draw_menu();
            break;
        case SCREEN_MANUAL:
            draw_manual();
            break;
        case SCREEN_ZONE_SELECT:
            draw_zone_select();
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
/*                                    Touch                                    */
/* -------------------------------------------------------------------------- */

void AppIrriga::handle_touch()
{
    std::int32_t tx = 0;
    std::int32_t ty = 0;
    const bool touching = GetHAL().display.getTouch(&tx, &ty) > 0;

    const std::uint32_t now = GetHAL().millis();
    if (touching && !_was_touching && (now - _last_tap_ms) > 250) {
        _last_tap_ms = now;
        handle_tap((int)tx, (int)ty);
    }
    _was_touching = touching;
}

void AppIrriga::handle_tap(int x, int y)
{
    /* The way back, in the same place on every screen that has one. */
    if (_screen != SCREEN_HOME &&
        inside(x, y, kHeaderX + kBackX, kHeaderY + kBackY, kBackW, kBackH)) {
        if (_screen == SCREEN_EDIT) {
            /* Leaving the editor without SET puts back what NVS still
             * holds. */
            irrig::reload_schedules();
            show(SCREEN_ZONE_SELECT);
        } else if (_screen == SCREEN_MENU) {
            show(SCREEN_HOME);
        } else {
            show(SCREEN_MENU);
        }
        return;
    }

    const int quadrant = quadrant_at(x, y);

    switch (_screen) {
        case SCREEN_HOME:
            /* Anywhere opens the menu, as the original did: there is
             * nothing else on this screen to press. */
            show(SCREEN_MENU);
            return;

        case SCREEN_MENU:
        case SCREEN_MANUAL:
        case SCREEN_ZONE_SELECT:
            if (quadrant >= 0) {
                _focus = quadrant;
                activate(quadrant);
            }
            return;

        case SCREEN_EDIT:
            if (inside(x, y, kBtnX2, kBtnY1, kBtnW, kBtnH)) {
                irrig::save_zone(_zone);
                irrig::update();
                audio::play_random_tone();
                show(SCREEN_ZONE_SELECT);
            } else if (inside(x, y, kBtnX1, kBtnY2, kHalfW, kBtnH)) {
                next_field(-1);
            } else if (inside(x, y, kBtnX1 + kHalfW + 6, kBtnY2, kHalfW, kBtnH)) {
                next_field(1);
            } else if (inside(x, y, kBtnX2, kBtnY2, kHalfW, kBtnH)) {
                adjust(1);
            } else if (inside(x, y, kBtnX2 + kHalfW + 6, kBtnY2, kHalfW, kBtnH)) {
                adjust(-1);
            } else if (inside(x, y, kBtnX1, kBtnY1, kBtnW, kBtnH)) {
                /* A tap on the card picks the line it landed on, which
                 * is quicker than walking to it twelve steps at a
                 * time. */
                const int row = (y - kBtnY1 - 17) / 16;
                if (row >= 0 && row < irrig::kSlotsPerZone) {
                    _slot  = row;
                    _field = 0;
                    _dirty = true;
                }
            }
            return;

        case SCREEN_PINS:
            if (inside(x, y, kBtnX1, kBtnY2, kBtnW, kBtnH)) {
                _zone  = (_zone + 1) % irrig::zone_count();
                _dirty = true;
            } else if (inside(x, y, kBtnX2, kBtnY2, kHalfW, kBtnH)) {
                cycle_pin(1);
            } else if (inside(x, y, kBtnX2 + kHalfW + 6, kBtnY2, kHalfW, kBtnH)) {
                cycle_pin(-1);
            }
            return;

        default:
            return;
    }
}

bool AppIrriga::has_buttons() const
{
    return _screen == SCREEN_MENU || _screen == SCREEN_MANUAL || _screen == SCREEN_ZONE_SELECT;
}

/*
 * One of the four buttons, pressed.
 *
 * Written once because two things press them: a finger, which lands on
 * the quarter it means, and Enter, which presses whatever the arrows are
 * pointing at. The two used to be separate and had already started to
 * disagree.
 */
void AppIrriga::activate(int quadrant)
{
    if (quadrant < 0 || quadrant > 3) {
        return;
    }

    switch (_screen) {
        case SCREEN_MENU:
            switch (quadrant) {
                case 0:
                    show(SCREEN_HOME);
                    return;
                case 1:
                    /* Manual is a mode of the engine, not of this
                     * screen: leaving does not put the valves back. */
                    irrig::set_mode(irrig::Mode::Manual);
                    show(SCREEN_MANUAL);
                    return;
                case 2:
                    show(SCREEN_ZONE_SELECT);
                    return;
                default:
                    show(SCREEN_PINS);
                    return;
            }

        case SCREEN_MANUAL:
            if (quadrant < irrig::zone_count()) {
                irrig::set_manual(quadrant, !irrig::manual(quadrant));
                _dirty = true;
            }
            return;

        case SCREEN_ZONE_SELECT:
            if (quadrant < irrig::zone_count()) {
                _zone  = quadrant;
                _slot  = 0;
                _field = 0;
                show(SCREEN_EDIT);
            }
            return;

        default:
            return;
    }
}

/* -------------------------------------------------------------------------- */
/*                                   Editing                                   */
/* -------------------------------------------------------------------------- */

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

/*
 * Forty-eight steps: twelve through a slot, then on to the next one.
 *
 * The original walked the whole zone this way because the whole zone is
 * on screen at once, and so it is here: NEXT off the end of the last
 * field lands on the first field of the line below rather than back at
 * the top of the same one.
 */
void AppIrriga::next_field(int delta)
{
    int step = _slot * kFieldCount + _field + delta;
    const int total = irrig::kSlotsPerZone * kFieldCount;
    step = ((step % total) + total) % total;

    _slot  = step / kFieldCount;
    _field = step % kFieldCount;
    _dirty = true;
}

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
                audio::play_random_tone();
                show(SCREEN_ZONE_SELECT);
                return;
            default:
                return;
        }
    }

    switch (ch) {
        case 'm':
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
        /* A screen of buttons is arrived at with the first one under the
         * cursor, wherever the cursor happened to be on the last one. */
        if (has_buttons()) {
            _focus = 0;
        }
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
     * address, so anything the launcher drew would only take room from
     * the schedule and repeat what is already there. */
    GetHAL().setFullScreenApp(true);

    /* Out of PSRAM, like every other sprite on this board: together they
     * are 57 KB, and the internal half is the scarce one. */
    auto& panel = GetHAL().display;
    _header.setPsram(true);
    _card.setPsram(true);
    _header.setColorDepth(panel.getColorDepth());
    _card.setColorDepth(panel.getColorDepth());
    if (!_header.createSprite(kHeaderW, kHeaderH) || !_card.createSprite(kBtnW, kBtnH)) {
        mclog::tagError(getAppInfo().name, "no room for the header and card sprites");
    }

    _screen      = SCREEN_HOME;
    _zone        = 0;
    _slot        = 0;
    _field       = 0;
    _dirty       = true;
    _needs_clear = true;
    _was_touching = false;

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
                next_field(right ? 1 : -1);
            }
            return;
        }

        /* Four buttons in two columns: up and down walk a column, left
         * and right change column. The quarters are numbered down the
         * left and then down the right, so a column is a step of one and
         * the other column is a step of two. */
        if (has_buttons()) {
            if (up || down) {
                /* A column is two buttons, so either arrow lands on the
                 * other one. */
                _focus = (_focus % 2 == 0) ? _focus + 1 : _focus - 1;
            } else {
                _focus = (_focus + 2) % 4;
            }
            _dirty = true;
            return;
        }

        if (_screen == SCREEN_PINS) {
            if (up || down) {
                const int count = irrig::zone_count();
                _zone           = (_zone + (up ? count - 1 : 1)) % count;
            } else {
                cycle_pin(right ? 1 : -1);
            }
            _dirty = true;
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
                } else if (_screen == SCREEN_EDIT) {
                    irrig::reload_schedules();
                    show(SCREEN_ZONE_SELECT);
                } else if (_screen == SCREEN_MENU) {
                    show(SCREEN_HOME);
                } else {
                    show(SCREEN_MENU);
                }
                return;

            case KEY_TAB:
                if (_screen == SCREEN_EDIT) {
                    next_field(1);
                }
                return;

            case KEY_ENTER:
                if (_screen == SCREEN_EDIT) {
                    next_field(1);
                } else if (has_buttons()) {
                    activate(_focus);
                } else if (_screen == SCREEN_HOME) {
                    show(SCREEN_MENU);
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

    /*
     * This screen owns the glass.
     *
     * The panel is otherwise divided into nine cells standing in for a
     * keyboard, and the middle band of them sends left, enter and right
     * -- which is where this screen's buttons are. Held down every pass
     * rather than asked for once, because the launcher resumes the cells
     * on the way out of the desktop, which happens after onOpen.
     */
    touch_keys::suspend();
    handle_touch();

    /* The clock ticks and the relays follow the schedule while this is
     * open, so the view refreshes on its own beat as well as on a tap. */
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

    _header.deleteSprite();
    _card.deleteSprite();

    touch_keys::resume();

    /* Giving the panel back also builds the launcher's canvas again,
     * empty; it notices and redraws it itself. */
    GetHAL().setFullScreenApp(false);
}
