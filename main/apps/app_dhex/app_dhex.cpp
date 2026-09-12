/*
 * SPDX-License-Identifier: MIT
 */
#include "app_dhex.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal.h>
#include <mooncake_log.h>

#include "assets/dhex_big.h"
#include "assets/dhex_small.h"
#include "dhex_api/dhex_app.h"
#include "dhex_api/dhex_keys.h"
#include "dhex_api/dhex_gfx_lgfx.h"
#include "dhex.h"

using namespace mooncake;

namespace {

// The application expects a tick event on a fixed
// cadence; dhex drains the UART on each one. 20 ms keeps the dump
// responsive without starving the rest of the main loop.
constexpr std::uint32_t kTickIntervalMs = 20;

}  // namespace

AppDhex::AppDhex()
{
    setAppInfo().name     = "dhex";
    setAppInfo().userData = new AppIcon_t(image_data_dhex_big, image_data_dhex_small);
}

AppDhex::~AppDhex()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppDhex::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    app_header::reset();

    /* dhex draws a band of its own already -- its name, the port and the
     * pins, and a rule under them -- and now keeps the top right corner
     * clear for this. Only the button is added, on the way out to the
     * panel, which is as often as anything changes. */
    _gfx = dhex_host_gfx_create(&GetHAL().canvas, []() {
        app_header::draw_back(GetHAL().canvas);
        GetHAL().pushCanvas();
    });
    if (_gfx == nullptr) {
        mclog::tagError(getAppInfo().name, "graphics surface allocation failed");
        _close_requested = true;
        return;
    }

    _ctx = dhex_host_context_create(_gfx);
    if (_ctx == nullptr) {
        mclog::tagError(getAppInfo().name, "context allocation failed");
        dhex_host_gfx_destroy(_gfx);
        _gfx             = nullptr;
        _close_requested = true;
        return;
    }

    // The application is normally launched from a shell, so argv[0] is the
    // command name. dhex reads argc == 1 as "no arguments given, use the
    // saved configuration" and rejects anything between that and a full
    // "dhex <baud> <framing> <rx> <tx> [port]" line.
    static const char* const kArgv[] = {"dhex"};
    dhex_host_context_set_args(_ctx, 1, kArgv);

    const esp_err_t ret = dhex_app.start(_ctx);
    if (ret != ESP_OK) {
        mclog::tagError(getAppInfo().name, "start failed: {}", esp_err_to_name(ret));
        _close_requested = true;
        return;
    }

    // The arrow-marked keys only report KEY_UP/DOWN/LEFT/RIGHT while Fn is
    // held; pressed bare they are ',' '.' '/' ';'. The launcher navigates
    // by physical position instead so they work either way, and this does
    // the same, with the arrows deliberately absent from the converted
    // handler below so a key is never counted twice.
    _handle_key_event_raw_slot_id = GetHAL().keyboard.onKeyEventRaw.connect(
        [this](const Keyboard::KeyEventRaw_t& keyEvent) {
            if (keyEvent.state == false) {
                return;
            }
            if (keyEvent.row == 2 && keyEvent.col == 11) {
                feed_char(DHEX_KEY_UP);
            } else if (keyEvent.row == 3 && keyEvent.col == 11) {
                feed_char(DHEX_KEY_DOWN);
            } else if (keyEvent.row == 3 && keyEvent.col == 10) {
                feed_char(DHEX_KEY_LEFT);
            } else if (keyEvent.row == 3 && keyEvent.col == 12) {
                feed_char(DHEX_KEY_RIGHT);
            }
        });

    _handle_key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) {
            if (keyEvent.isModifier || keyEvent.state == false) {
                return;
            }

            switch (keyEvent.keyCode) {
                case KEY_ENTER:
                    feed_char('\r');
                    return;
                case KEY_ESC:
                    feed_char(DHEX_KEY_ESCAPE);
                    return;
                case KEY_UP:
                case KEY_DOWN:
                case KEY_LEFT:
                case KEY_RIGHT:
                    // Already delivered by the raw handler above.
                    return;
                default:
                    break;
            }

            // Anything else goes through as its printable character, which
            // is what a terminal session would have delivered.
            if (keyEvent.keyName != nullptr && keyEvent.keyName[0] != '\0' &&
                keyEvent.keyName[1] == '\0') {
                feed_char(static_cast<std::uint8_t>(keyEvent.keyName[0]));
            }
        });

    _last_tick_ms = GetHAL().millis();
}

void AppDhex::feed_char(std::uint8_t ch)
{
    if (_ctx == nullptr) {
        return;
    }
    dhex_event_t event = {};
    event.type             = DHEX_EVENT_CHAR;
    event.data.ch          = static_cast<char>(ch);
    dhex_app.event(_ctx, &event);
}

void AppDhex::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        close();
        return;
    }

    if (app_header::back_pressed(GetHAL().canvas.width(), GetHAL().canvasKeyboardBar.width(), 0)) {
        audio::play_random_tone();
        close();
        return;
    }

    if (_ctx == nullptr) {
        return;
    }

    const std::uint32_t now = GetHAL().millis();
    if (now - _last_tick_ms >= kTickIntervalMs) {
        _last_tick_ms          = now;
        dhex_event_t event = {};
        event.type             = DHEX_EVENT_TICK;
        event.data.tick_ms     = now;
        dhex_app.event(_ctx, &event);
    }

    // Escape asks the application to quit; the home button does the same
    // from outside it, matching every other app in this firmware.
    if (dhex_host_context_exit_requested(_ctx) || GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppDhex::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_handle_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_handle_key_event_slot_id);
        _handle_key_event_slot_id = -1;
    }

    if (_handle_key_event_raw_slot_id >= 0) {
        GetHAL().keyboard.onKeyEventRaw.disconnect(_handle_key_event_raw_slot_id);
        _handle_key_event_raw_slot_id = -1;
    }

    if (_ctx != nullptr) {
        dhex_app.stop(_ctx);
        dhex_host_context_destroy(_ctx);
        _ctx = nullptr;
    }

    if (_gfx != nullptr) {
        dhex_host_gfx_destroy(_gfx);
        _gfx = nullptr;
    }
}
