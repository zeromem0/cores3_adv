/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_set_wifi.h"
#include "assets/set_wifi_big.h"
#include "assets/set_wifi_small.h"
#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <assets.h>
#include <hal/utils/wifi_store/wifi_store.h>

using namespace mooncake;

AppSetWiFi::AppSetWiFi()
{
    setAppInfo().name     = "SetWiFi";
    setAppInfo().userData = new AppIcon_t(image_data_set_wifi_big, image_data_set_wifi_small);
}

AppSetWiFi::~AppSetWiFi()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppSetWiFi::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // This application drives scan and connect from the main task, which
    // must not overlap with the background rejoin task doing the same.
    wifi_store::suspend_auto_connect();
    app_header::reset();

    // Reset status
    _wifi_ssid.clear();
    _wifi_password.clear();
    _input_buffer.clear();
    _current_state     = STATE_INIT;
    _connection_result = false;

    load_saved_wifi_settings();

    // Setup keyboard event handler
    _key_event_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_key_event(keyEvent); });

    /* Matrix positions, for the list only: ; and . move the selection
     * and enter chooses, with or without Fn, which is how every other
     * screen in this firmware behaves. */
    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& key) {
        if (!key.state || _current_state != STATE_PICK_NETWORK) {
            return;
        }
        const int rows = static_cast<int>(_networks.size()) + 1;
        if (key.row == 2 && key.col == 11) {
            _selected = (_selected + rows - 1) % rows;
            render_network_list();
        } else if (key.row == 3 && key.col == 11) {
            _selected = (_selected + 1) % rows;
            render_network_list();
        } else if (key.row == 2 && key.col == 13) {
            choose_selected_network();
        }
    });

    scan_networks();
}

void AppSetWiFi::onRunning()
{
    // Update cursor blinking
    update_cursor();

    // Process state machine
    process_state_machine();

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    if (app_header::back_pressed(GetHAL().canvas.width(), GetHAL().canvasKeyboardBar.width(), 0)) {
        audio::play_random_tone();
        close();
    }
}

void AppSetWiFi::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    // Disconnect keyboard event handler
    if (_key_event_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_event_slot_id);
    }
    if (_key_raw_slot >= 0) {
        GetHAL().keyboard.onKeyEventRaw.disconnect(_key_raw_slot);
        _key_raw_slot = -1;
    }

    wifi_store::resume_auto_connect();
}

void AppSetWiFi::render_interface()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setTextScroll(true);
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setCursor(0, 0);

    show_ssid_prompt();
}

/* -------------------------------------------------------------------------- */
/*                              Choosing a network                            */
/* -------------------------------------------------------------------------- */
/*
 * Typing an SSID by hand was the only way in, which suited a board with
 * a keyboard. Here it suited nothing: the panel has no keys, and the
 * touch layer can move a selection and press enter but cannot spell.
 *
 * So the way in is the list of what is actually on the air, and a
 * password is only ever asked for when none is already known for the
 * network picked. Typing survives as the last row, for a hidden SSID.
 */
void AppSetWiFi::scan_networks()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setFont(&fonts::Font0);
    GetHAL().canvas.setTextSize(GetHAL().canvas.width() >= 480 ? 3 : 2);
    GetHAL().canvas.setCursor(GetHAL().canvas.width() >= 480 ? 10 : 4, 8);
    GetHAL().canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    GetHAL().canvas.println("Scanning...");
    GetHAL().pushCanvas();

    _networks.clear();
    GetHAL().wifiScan(_networks);
    _selected = 0;

    mclog::tagInfo(getAppInfo().name, "scan found {} networks", _networks.size());
    _current_state = STATE_PICK_NETWORK;
    render_network_list();
}

void AppSetWiFi::render_network_list()
{
    auto& canvas = GetHAL().canvas;

    /* Sized from the panel, because this runs on both boards: 240x111 on
     * the Cardputer and 800x456 on the LCD-5. One step of text size is
     * the difference between a readable list and an unreadable one. */
    const int width       = canvas.width();
    const int kTextScale  = width >= 480 ? 2 : 1;
    const int kTitleScale = kTextScale + 1;
    const int kMarginX    = width >= 480 ? 10 : 4;

    canvas.fillScreen(THEME_COLOR_BG);
    app_header::draw(canvas, "SetWiFi");
    canvas.setTextDatum(top_left);

    /*
     * The font is set here rather than assumed. Whatever ran before
     * leaves its own on the canvas, and the first cut of this screen
     * worked out row heights from the built-in 6x8 glyph while a much
     * larger face was actually being drawn -- so the text overflowed
     * rows that were too tight and the selection bar, sized from the
     * same wrong arithmetic, came out shorter than the line it was
     * meant to cover.
     */
    canvas.setFont(&fonts::Font0);

    canvas.setTextSize(kTitleScale);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString("Choose a network", kMarginX, app_header::height() + 8);
    const int title_height = canvas.fontHeight();

    canvas.setTextSize(kTextScale);

    /* Measured, not derived: fontHeight() knows what was actually set,
     * and the bar is exactly as tall as the row it fills. */
    const int glyph_w   = canvas.textWidth("0");
    const int row_air   = canvas.fontHeight();
    const int row_pitch = canvas.fontHeight() + row_air;
    const int list_top  = app_header::height() + 8 + title_height + 12;

    const int rows = static_cast<int>(_networks.size()) + 1;

    /*
     * Only as many rows as actually fit, scrolled to keep the selection
     * on screen.
     *
     * Without this the list simply ran off the bottom edge: on a 111
     * pixel panel four rows fit and six were drawn, so pressing down
     * moved the highlight to a row nobody could see and the keys looked
     * dead. The hint line gets its own reserved band for the same
     * reason -- it was being drawn underneath the overflowing rows.
     */
    const int hint_h = canvas.fontHeight() + 8;
    int visible      = (canvas.height() - list_top - hint_h) / row_pitch;
    if (visible < 1) {
        visible = 1;
    }

    int first = 0;
    if (_selected >= visible) {
        first = _selected - visible + 1;
    }
    if (first > rows - visible) {
        first = rows - visible;
    }
    if (first < 0) {
        first = 0;
    }

    for (int i = first; i < rows && i < first + visible; i++) {
        /* The bar starts half the air above the text and is a whole
         * pitch tall, so consecutive rows tile with the glyphs centred
         * in them. */
        const int y            = list_top + (i - first) * row_pitch;
        const bool is_selected = (i == _selected);

        if (is_selected) {
            canvas.fillRect(0, y - row_air / 2, canvas.width(), row_pitch, TFT_DARKGREEN);
        }
        canvas.setTextColor(is_selected ? TFT_WHITE : TFT_LIGHTGREY,
                            is_selected ? TFT_DARKGREEN : THEME_COLOR_BG);

        if (i == static_cast<int>(_networks.size())) {
            canvas.drawString("Other (type the name)", kMarginX, y);
            continue;
        }

        /* A network whose password is already stored needs no typing,
         * and saying so is the difference between one keypress and
         * thirty. */
        std::string known_password;
        const bool saved = wifi_store::password_for(_networks[i].second, known_password);

        std::string line = _networks[i].second;
        if (saved) {
            line += "  [saved]";
        }
        canvas.drawString(line.c_str(), kMarginX, y);

        canvas.setTextColor(is_selected ? TFT_WHITE : TFT_DARKGREY,
                            is_selected ? TFT_DARKGREEN : THEME_COLOR_BG);
        /* Right aligned by measurement rather than a guessed offset,
         * because the signal figure changes width. */
        const std::string signal = std::to_string(_networks[i].first) + " dBm";
        canvas.drawString(signal.c_str(),
                          canvas.width() - kMarginX - static_cast<int>(signal.size()) * glyph_w, y);
    }

    canvas.setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    if (rows > visible) {
        /* Said, rather than left for the user to discover by pressing
         * down and watching nothing happen. */
        char hint[64];
        std::snprintf(hint, sizeof(hint), "%d-%d of %d   Up/Down   Enter", first + 1,
                      first + visible, rows);
        canvas.drawString(hint, kMarginX, canvas.height() - hint_h + 4);
    } else {
        canvas.drawString("Up/Down: choose   Enter: connect", kMarginX,
                          canvas.height() - hint_h + 4);
    }
    GetHAL().pushCanvas();
}

void AppSetWiFi::choose_selected_network()
{
    /* The row past the end is the manual one. */
    if (_selected >= static_cast<int>(_networks.size())) {
        GetHAL().canvas.fillScreen(THEME_COLOR_BG);
        GetHAL().canvas.setTextSize(1);
        GetHAL().canvas.setCursor(0, 0);
        _wifi_ssid.clear();
        _wifi_password.clear();
        _input_buffer.clear();
        show_ssid_prompt();
        return;
    }

    _wifi_ssid = _networks[_selected].second;

    std::string known_password;
    if (wifi_store::password_for(_wifi_ssid, known_password)) {
        mclog::tagInfo(getAppInfo().name, "\"{}\" is already known, connecting", _wifi_ssid);
        _wifi_password = known_password;
        GetHAL().canvas.fillScreen(THEME_COLOR_BG);
        GetHAL().canvas.setTextSize(1);
        GetHAL().canvas.setCursor(0, 0);
        _current_state = STATE_CONNECTING;
        show_connection_status();
        return;
    }

    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setCursor(0, 0);
    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().canvas.println(_wifi_ssid.c_str());
    _input_buffer.clear();
    /* Cleared so the prompt does not helpfully offer some other
     * network's password, which is what the saved-value auto-fill would
     * otherwise do. */
    _wifi_password.clear();
    _current_state = STATE_WAIT_PASSWORD;
    show_password_prompt();
}

void AppSetWiFi::render_input_prompt()
{
    render_current_input_line();
}

void AppSetWiFi::render_current_input_line()
{
    // Get current cursor position - this is where we'll redraw the input line
    int cursor_y = GetHAL().canvas.getCursorY();

    // Clear the entire current line with extra space to ensure old cursor is removed
    GetHAL().canvas.fillRect(0, cursor_y, GetHAL().canvas.width(), 15, THEME_COLOR_BG);

    // Set cursor to beginning of line and redraw prompt + input + cursor
    GetHAL().canvas.setCursor(0, cursor_y);
    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().canvas.print(">>> ");
    GetHAL().canvas.print(_input_buffer.c_str());

    // Draw blinking cursor
    if (_cursor_state) {
        GetHAL().canvas.print("_");
    } else {
        GetHAL().canvas.print(" ");
    }

    GetHAL().pushCanvas();
}

void AppSetWiFi::handle_key_event(const Keyboard::KeyEvent_t& keyEvent)
{
    /* The network list is answered by the raw-position handler alone.
     * Answering it here as well moved the selection twice for one press,
     * because Fn+";" arrives as both a position and as KEY_UP. */

    // Only handle key press events for input states, skip modifiers
    if (!keyEvent.state || keyEvent.isModifier) {
        return;
    }


    // Only handle input in waiting states and failed state
    if (_current_state != STATE_WAIT_SSID && _current_state != STATE_WAIT_PASSWORD && _current_state != STATE_FAILED) {
        return;
    }

    switch (keyEvent.keyCode) {
        case KEY_ENTER:
            handle_enter_key();
            break;

        case KEY_BACKSPACE:
            handle_backspace();
            break;

        case KEY_SPACE:
            if (_input_buffer.length() < INPUT_BUFFER_SIZE - 1) {
                _input_buffer += ' ';
                render_input_prompt();
            }
            break;

        default:
            // Add regular characters
            if (keyEvent.keyName && strlen(keyEvent.keyName) == 1 && _input_buffer.length() < INPUT_BUFFER_SIZE - 1) {
                _input_buffer += keyEvent.keyName;
                render_input_prompt();
            }
            break;
    }
}

void AppSetWiFi::handle_enter_key()
{
    // Get ssid
    if (_current_state == STATE_WAIT_SSID) {
        if (_input_buffer.empty()) {
            return;
        }

        // Clear the current input line and redraw without cursor
        int cursor_y = GetHAL().canvas.getCursorY();
        GetHAL().canvas.fillRect(0, cursor_y, GetHAL().canvas.width(), 15, THEME_COLOR_BG);
        GetHAL().canvas.setCursor(0, cursor_y);
        GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
        GetHAL().canvas.print(">>> ");
        GetHAL().canvas.print(_input_buffer.c_str());
        GetHAL().canvas.println();  // Move to next line

        _wifi_ssid = _input_buffer;
        mclog::tagInfo(getAppInfo().name, "WiFi SSID set: \"{}\"", _wifi_ssid);
        _input_buffer.clear();
        _current_state = STATE_WAIT_PASSWORD;
        GetHAL().pushCanvas();  // Ensure screen is updated before showing next prompt
        show_password_prompt();
    }

    // Get password
    else if (_current_state == STATE_WAIT_PASSWORD) {
        if (_input_buffer.empty()) {
            return;
        }

        // Clear the current input line and redraw without cursor
        int cursor_y = GetHAL().canvas.getCursorY();
        GetHAL().canvas.fillRect(0, cursor_y, GetHAL().canvas.width(), 15, THEME_COLOR_BG);
        GetHAL().canvas.setCursor(0, cursor_y);
        GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
        GetHAL().canvas.print(">>> ");
        GetHAL().canvas.print(_input_buffer.c_str());
        GetHAL().canvas.println();  // Move to next line

        _wifi_password = _input_buffer;
        mclog::tagInfo(getAppInfo().name, "WiFi password set: \"{}\"", _wifi_password);
        _input_buffer.clear();
        _current_state = STATE_CONNECTING;
        GetHAL().pushCanvas();  // Ensure screen is updated before showing connection status
        show_connection_status();
    }

    // Retry: back to the list rather than to a blank SSID prompt, since
    // what is on the air may well have changed while we were trying.
    else if (_current_state == STATE_FAILED) {
        _wifi_ssid.clear();
        _wifi_password.clear();
        _input_buffer.clear();
        scan_networks();
    }
}

void AppSetWiFi::handle_backspace()
{
    if (!_input_buffer.empty()) {
        _input_buffer.pop_back();

        // Clear a larger area to ensure old cursor is completely removed
        int cursor_y = GetHAL().canvas.getCursorY();
        GetHAL().canvas.fillRect(0, cursor_y, GetHAL().canvas.width(), 15, THEME_COLOR_BG);

        // Redraw the input line
        render_current_input_line();
    }
}

void AppSetWiFi::update_cursor()
{
    if (GetHAL().millis() - _cursor_update_time > CURSOR_BLINK_PERIOD) {
        _cursor_state       = !_cursor_state;
        _cursor_update_time = GetHAL().millis();
        if (_current_state == STATE_WAIT_SSID || _current_state == STATE_WAIT_PASSWORD) {
            render_input_prompt();
        }
    }
}

void AppSetWiFi::process_state_machine()
{
    if (_current_state == STATE_CONNECTING) {
        connect_to_wifi();
        _current_state = _connection_result ? STATE_SUCCESS : STATE_FAILED;
        show_connection_result();
    }
}

void AppSetWiFi::show_ssid_prompt()
{
    GetHAL().canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    GetHAL().canvas.println("WiFi SSID:");
    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().canvas.print(">>> ");
    GetHAL().pushCanvas();

    _current_state = STATE_WAIT_SSID;

    // Auto-fill saved SSID if available
    auto_fill_saved_ssid();
}

void AppSetWiFi::show_password_prompt()
{
    GetHAL().canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    GetHAL().canvas.println("WiFi Password:");
    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().canvas.print(">>> ");
    GetHAL().pushCanvas();

    // Auto-fill saved password if available
    auto_fill_saved_password();
}

void AppSetWiFi::show_connection_status()
{
    GetHAL().canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    GetHAL().canvas.printf("WiFi config:\n- %s\n- %s\n", _wifi_ssid.c_str(), _wifi_password.c_str());
    GetHAL().canvas.println("Connecting...");
    GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
    GetHAL().pushCanvas();
}

void AppSetWiFi::connect_to_wifi()
{
    mclog::tagInfo(getAppInfo().name, "attempting WiFi connection to \"{}\"", _wifi_ssid);

    // Init and connect
    GetHAL().wifiInit();
    _connection_result = GetHAL().wifiConnect(_wifi_ssid, _wifi_password);

    mclog::tagInfo(getAppInfo().name, "WiFi connection result: {}", _connection_result ? "success" : "failed");
}

void AppSetWiFi::show_connection_result()
{
    if (_connection_result) {
        GetHAL().canvas.setTextColor(TFT_GREEN, THEME_COLOR_BG);
        GetHAL().canvas.println("Connected successfully!");

        // Save WiFi settings on successful connection
        save_wifi_settings();

        GetHAL().canvas.setTextColor(TFT_CYAN, THEME_COLOR_BG);
        GetHAL().canvas.println("WiFi settings saved");

        // The address remoted serves on, reported here so it does not have
        // to be looked up on the router.
        const std::string ip = GetHAL().getIpAddress();
        if (!ip.empty()) {
            GetHAL().canvas.print("http://");
            GetHAL().canvas.println(ip.c_str());
        }

        GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
        GetHAL().canvas.println("Press Home to exit");
    } else {
        GetHAL().canvas.setTextColor(TFT_RED, THEME_COLOR_BG);
        GetHAL().canvas.println("Connection failed!");
        GetHAL().canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);
        GetHAL().canvas.println("Press Enter to retry");
        GetHAL().canvas.println("Press Home to exit");
    }

    GetHAL().pushCanvas();
}

void AppSetWiFi::load_saved_wifi_settings()
{
    // Load saved SSID and password from settings
    _wifi_ssid     = GetHAL().getSettings().GetString("wifi_ssid", "");
    _wifi_password = GetHAL().getSettings().GetString("wifi_password", "");

    if (!_wifi_ssid.empty()) {
        mclog::tagInfo(getAppInfo().name, "loaded saved WiFi SSID: \"{}\"", _wifi_ssid);
    }
    if (!_wifi_password.empty()) {
        mclog::tagInfo(getAppInfo().name, "loaded saved WiFi password (length: {})", _wifi_password.length());
    }
}

void AppSetWiFi::save_wifi_settings()
{
    // Save SSID and password to settings
    GetHAL().getSettings().SetString("wifi_ssid", _wifi_ssid);
    GetHAL().getSettings().SetString("wifi_password", _wifi_password);

    // Also add it to the known list, which keeps several networks rather
    // than only the one connected to last.
    wifi_store::remember(_wifi_ssid, _wifi_password);

    mclog::tagInfo(getAppInfo().name, "saved WiFi settings - SSID: \"{}\"", _wifi_ssid);
}

void AppSetWiFi::auto_fill_saved_ssid()
{
    // Auto-fill the input buffer with saved SSID if available
    if (!_wifi_ssid.empty()) {
        _input_buffer = _wifi_ssid;
        render_input_prompt();
        mclog::tagInfo(getAppInfo().name, "auto-filled SSID: \"{}\"", _wifi_ssid);
    }
}

void AppSetWiFi::auto_fill_saved_password()
{
    // Auto-fill the input buffer with saved password if available
    if (!_wifi_password.empty()) {
        _input_buffer = _wifi_password;
        render_input_prompt();
        mclog::tagInfo(getAppInfo().name, "auto-filled password (length: {})", _wifi_password.length());
    }
}
