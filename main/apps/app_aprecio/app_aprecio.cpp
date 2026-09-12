/*
 * SPDX-License-Identifier: MIT
 */
#include "app_aprecio.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <driver/uart.h>
#include <mooncake_log.h>
#include <nvs.h>

#include <cstdio>
#include <ctime>

#include "assets/aprecio_big.h"
#include "assets/aprecio_small.h"
#include "hal/hal_config.h"

using namespace mooncake;

namespace {

const std::string _tag = "aprecio";

constexpr const char* kNvsNamespace = "aprecio";
constexpr int kUartDriverTxBytes = 512;

// What the fixed payload starts out as, before anything is typed in.
constexpr const char* kFixedPayloadDefault = "Apreciometru";

// Cycled through rather than nudged one millisecond at a time.
const std::uint32_t kIntervals[] = {1000, 2000, 5000, 60000};
constexpr int kIntervalCount = sizeof(kIntervals) / sizeof(kIntervals[0]);

const std::uint32_t kBauds[] = {300,   600,   1200,   2400,   4800,  9600,
                                19200, 38400, 57600, 115200, 230400, 460800, 921600};
constexpr int kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);

constexpr int kFieldCount = 8;
const char* kFieldNames[kFieldCount] = {"Baud", "Data bits", "Parity", "Stop bits",
                                        "Interval", "Payload", "Suffix", "Text"};

constexpr int kRowText = 7;

// Longer than the payload row can show, but the whole of it is still sent.
constexpr std::size_t kTextMax = 24;

// One row past the fields, so applying is a deliberate choice rather than
// a gesture that has to be remembered.
constexpr int kSaveRow  = kFieldCount;
constexpr int kRowCount = kFieldCount + 1;

constexpr int kHeaderHeight = 34;
constexpr int kMargin       = 6;
constexpr int kLabelGap     = 24;

uart_word_length_t word_length(std::uint8_t bits)
{
    switch (bits) {
        case 5: return UART_DATA_5_BITS;
        case 6: return UART_DATA_6_BITS;
        case 7: return UART_DATA_7_BITS;
        default: return UART_DATA_8_BITS;
    }
}

uart_parity_t parity_of(char parity)
{
    switch (parity) {
        case 'E': return UART_PARITY_EVEN;
        case 'O': return UART_PARITY_ODD;
        default: return UART_PARITY_DISABLE;
    }
}

std::string interval_text(std::uint32_t ms)
{
    char text[16];
    if (ms >= 1000 && (ms % 1000) == 0) {
        snprintf(text, sizeof(text), "%us", static_cast<unsigned>(ms / 1000));
    } else {
        snprintf(text, sizeof(text), "%ums", static_cast<unsigned>(ms));
    }
    return text;
}

}  // namespace

AppAprecio::AppAprecio()
{
    setAppInfo().name     = "Aprecio";
    setAppInfo().userData = new AppIcon_t(image_data_aprecio_big, image_data_aprecio_small);
}

AppAprecio::~AppAprecio()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

/* -------------------------------------------------------------------------- */
/*                                   Config                                   */
/* -------------------------------------------------------------------------- */
void AppAprecio::defaults(Config_t& cfg)
{
    cfg.port        = 2;  // the Grove connector, wired as UART2
    cfg.tx_pin      = HAL_PIN_GPS_TX;
    cfg.rx_pin      = HAL_PIN_GPS_RX;
    cfg.baud        = 9600;
    cfg.data_bits   = 8;
    cfg.parity      = 'N';
    cfg.stop_bits   = 1;
    cfg.interval_ms = 1000;
    cfg.payload     = PAYLOAD_TIME;
    cfg.ending      = ENDING_CRLF;
}

void AppAprecio::load_config()
{
    defaults(_config);
    _text = kFixedPayloadDefault;

    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    char text[kTextMax + 1] = {0};
    std::size_t text_len    = sizeof(text);
    if (nvs_get_str(handle, "text", text, &text_len) == ESP_OK && text[0] != '\0') {
        _text = text;
    }

    std::uint32_t u32 = 0;
    std::uint8_t u8   = 0;
    if (nvs_get_u32(handle, "baud", &u32) == ESP_OK) _config.baud = u32;
    if (nvs_get_u32(handle, "interval", &u32) == ESP_OK) _config.interval_ms = u32;
    if (nvs_get_u8(handle, "bits", &u8) == ESP_OK) _config.data_bits = u8;
    if (nvs_get_u8(handle, "stop", &u8) == ESP_OK) _config.stop_bits = u8;
    if (nvs_get_u8(handle, "parity", &u8) == ESP_OK) _config.parity = static_cast<char>(u8);
    if (nvs_get_u8(handle, "payload", &u8) == ESP_OK) _config.payload = u8;
    if (nvs_get_u8(handle, "ending", &u8) == ESP_OK) _config.ending = u8;

    nvs_close(handle);

    // A stored interval may predate a change to the list; snap it to the
    // nearest offered value so cycling always starts somewhere valid.
    std::uint32_t best = kIntervals[0];
    std::uint32_t best_delta = UINT32_MAX;
    for (int i = 0; i < kIntervalCount; i++) {
        const std::uint32_t delta = kIntervals[i] > _config.interval_ms ? kIntervals[i] - _config.interval_ms
                                                                       : _config.interval_ms - kIntervals[i];
        if (delta < best_delta) {
            best_delta = delta;
            best       = kIntervals[i];
        }
    }
    _config.interval_ms = best;

    if (_config.payload >= PAYLOAD_COUNT_) {
        _config.payload = PAYLOAD_TIME;
    }
    if (_config.ending >= ENDING_COUNT_) {
        _config.ending = ENDING_CRLF;
    }
}

void AppAprecio::save_config()
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        mclog::tagWarn(_tag, "could not open NVS for writing");
        return;
    }

    nvs_set_u32(handle, "baud", _config.baud);
    nvs_set_u32(handle, "interval", _config.interval_ms);
    nvs_set_u8(handle, "bits", _config.data_bits);
    nvs_set_u8(handle, "stop", _config.stop_bits);
    nvs_set_u8(handle, "parity", static_cast<std::uint8_t>(_config.parity));
    nvs_set_u8(handle, "payload", _config.payload);
    nvs_set_u8(handle, "ending", _config.ending);
    nvs_set_str(handle, "text", _text.c_str());

    nvs_commit(handle);
    nvs_close(handle);
}

/* -------------------------------------------------------------------------- */
/*                                    UART                                    */
/* -------------------------------------------------------------------------- */
bool AppAprecio::uart_start(const Config_t& cfg)
{
    uart_config_t uart = {};
    uart.baud_rate     = static_cast<int>(cfg.baud);
    uart.data_bits     = word_length(cfg.data_bits);
    uart.parity        = parity_of(cfg.parity);
    uart.stop_bits     = cfg.stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    uart.flow_ctrl     = UART_HW_FLOWCTRL_DISABLE;
    uart.source_clk    = UART_SCLK_DEFAULT;

    const uart_port_t port = static_cast<uart_port_t>(cfg.port);

    if (uart_is_driver_installed(port)) {
        uart_driver_delete(port);
    }

    // Receive buffer is required by the driver even though nothing is read
    // here; the minimum it accepts is enough.
    if (uart_driver_install(port, UART_HW_FIFO_LEN(port) + 16, kUartDriverTxBytes, 0, nullptr, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(port, &uart) != ESP_OK) {
        uart_driver_delete(port);
        return false;
    }
    if (uart_set_pin(port, cfg.tx_pin, cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(port);
        return false;
    }

    return true;
}

void AppAprecio::uart_stop()
{
    const uart_port_t port = static_cast<uart_port_t>(_config.port);
    if (uart_is_driver_installed(port)) {
        uart_driver_delete(port);
    }
    _uart_ready = false;
}

/* -------------------------------------------------------------------------- */
/*                                   Payload                                  */
/* -------------------------------------------------------------------------- */
std::string AppAprecio::build_payload()
{
    std::string body;

    if (_config.payload != PAYLOAD_FIXED) {
        time_t now = 0;
        struct tm info = {};
        time(&now);
        localtime_r(&now, &info);

        char text[20];
        if (_config.payload == PAYLOAD_TIME) {
            snprintf(text, sizeof(text), "%02d%02d", info.tm_hour, info.tm_min);
        } else if (_config.payload == PAYLOAD_DATETIME) {
            snprintf(text, sizeof(text), "%04d%02d%02d%02d%02d", info.tm_year + 1900, info.tm_mon + 1,
                     info.tm_mday, info.tm_hour, info.tm_min);
        } else {
            snprintf(text, sizeof(text), "%04d%02d%02d%02d%02d%02d", info.tm_year + 1900, info.tm_mon + 1,
                     info.tm_mday, info.tm_hour, info.tm_min, info.tm_sec);
        }
        body = text;
    } else {
        body = _text;
    }

    switch (_config.ending) {
        case ENDING_CR: body += "\r"; break;
        case ENDING_LF: body += "\n"; break;
        case ENDING_CRLF: body += "\r\n"; break;
        default: break;
    }

    return body;
}

void AppAprecio::send_now()
{
    if (!_uart_ready) {
        return;
    }

    const std::string payload = build_payload();
    uart_write_bytes(static_cast<uart_port_t>(_config.port), payload.data(), payload.size());

    _last_payload = payload;
    _sent_count++;
}

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                 */
/* -------------------------------------------------------------------------- */
void AppAprecio::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    app_header::reset();

    load_config();
    _uart_ready = uart_start(_config);
    if (!_uart_ready) {
        mclog::tagWarn(getAppInfo().name, "uart start failed");
    }

    _running      = false;
    _config_mode  = false;
    _field        = 0;
    _sent_count   = 0;
    _last_payload.clear();

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& keyEvent) {
        if (keyEvent.isModifier || keyEvent.state == false) {
            return;
        }
        if (keyEvent.keyCode == KEY_ENTER) {
            handle_char('\r');
        } else if (keyEvent.keyCode == KEY_ESC) {
            handle_char(0x1b);
        } else if (keyEvent.keyCode == KEY_BACKSPACE) {
            handle_char(0x08);
        } else if (keyEvent.keyName != nullptr && keyEvent.keyName[0] != '\0' &&
                   keyEvent.keyName[1] == '\0') {
            // Single-character key names are the printable keys; they only
            // mean anything while the text field is open.
            handle_char(static_cast<std::uint8_t>(keyEvent.keyName[0]));
        }
    });

    // Arrow keys by physical position, matching the launcher and dhex, so
    // they work whether or not Fn is held.
    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& keyEvent) {
        if (keyEvent.state == false) {
            return;
        }
        if (keyEvent.row == 2 && keyEvent.col == 11) handle_char(0x80);
        else if (keyEvent.row == 3 && keyEvent.col == 11) handle_char(0x81);
        else if (keyEvent.row == 3 && keyEvent.col == 10) handle_char(0x82);
        else if (keyEvent.row == 3 && keyEvent.col == 12) handle_char(0x83);
    });

    render();
}

void AppAprecio::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        close();
        return;
    }

    if (app_header::back_pressed(GetHAL().canvas.width(), GetHAL().canvasKeyboardBar.width(), 0)) {
        _close_requested = true;
        return;
    }

    if (_running && _uart_ready) {
        const std::uint32_t now = GetHAL().millis();
        if (static_cast<std::int32_t>(now - _next_send_ms) >= 0) {
            _next_send_ms = now + _config.interval_ms;
            send_now();
            if (!_config_mode) {
                render();
            }
        }
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppAprecio::onClose()
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

    uart_stop();
}

/* -------------------------------------------------------------------------- */
/*                                    Input                                   */
/* -------------------------------------------------------------------------- */
void AppAprecio::handle_char(std::uint8_t ch)
{
    // Either axis means the same thing, so the encoder's current axis
    // never has to be known.
    const bool back    = (ch == 0x80 || ch == 0x82);
    const bool forward = (ch == 0x81 || ch == 0x83);

    if (!_config_mode) {
        switch (ch) {
            case '\r':
                _draft       = _config;
                _field       = 0;
                _editing     = false;
                _config_mode = true;
                break;
            // Arrows or space start and stop transmission; outside the
            // settings screen there is nothing else for them to mean.
            case ' ':
            case 0x80:
            case 0x81:
            case 0x82:
            case 0x83:
                _running = !_running;
                if (_running) {
                    _next_send_ms = GetHAL().millis();
                }
                break;
            case 0x1b:
                _close_requested = true;
                return;
            default:
                return;
        }
        render();
        return;
    }

    if (_text_editing) {
        // The one field that is typed rather than chosen from a list.
        if (ch == '\r') {
            _text          = _draft_text;
            _text_editing  = false;
            _editing       = false;
        } else if (ch == 0x1b) {
            _text_editing = false;
            _editing      = false;
        } else if (ch == 0x08 || ch == 0x7f) {
            if (!_draft_text.empty()) {
                _draft_text.pop_back();
            }
        } else if (ch >= 0x20 && ch < 0x7f && _draft_text.size() < kTextMax) {
            _draft_text.push_back(static_cast<char>(ch));
        } else {
            return;
        }
        render();
        return;
    }

    if (_editing) {
        // Inside a field: turning walks that field's options, clicking
        // settles on the one shown.
        if (back) {
            adjust_field(-1);
        } else if (forward) {
            adjust_field(1);
        } else if (ch == '\r' || ch == 0x1b) {
            _editing = false;
        } else {
            return;
        }
        render();
        return;
    }

    if (back) {
        _field = _field == 0 ? kRowCount - 1 : _field - 1;
    } else if (forward) {
        _field = (_field + 1) % kRowCount;
    } else if (ch == '\r') {
        if (_field == kSaveRow) {
            _config      = _draft;
            _config_mode = false;
            save_config();
            uart_stop();
            _uart_ready = uart_start(_config);
        } else if (_field == kRowText) {
            _draft_text   = _text;
            _text_editing = true;
            _editing      = true;
        } else {
            _editing = true;
        }
    } else if (ch == 0x1b) {
        _config_mode = false;
    } else {
        return;
    }
    render();
}

void AppAprecio::adjust_field(int delta)
{
    switch (_field) {
        case 0: {
            int index = 0;
            for (int i = 0; i < kBaudCount; i++) {
                if (kBauds[i] == _draft.baud) index = i;
            }
            index        = (index + delta + kBaudCount) % kBaudCount;
            _draft.baud  = kBauds[index];
            break;
        }
        case 1:
            _draft.data_bits = static_cast<std::uint8_t>(_draft.data_bits + delta);
            if (_draft.data_bits < 5) _draft.data_bits = 8;
            if (_draft.data_bits > 8) _draft.data_bits = 5;
            break;
        case 2:
            _draft.parity = _draft.parity == 'N' ? (delta > 0 ? 'E' : 'O')
                            : _draft.parity == 'E' ? (delta > 0 ? 'O' : 'N')
                                                   : (delta > 0 ? 'N' : 'E');
            break;
        case 3:
            _draft.stop_bits = _draft.stop_bits == 1 ? 2 : 1;
            break;
        case 4: {
            int index = 0;
            for (int i = 0; i < kIntervalCount; i++) {
                if (kIntervals[i] == _draft.interval_ms) index = i;
            }
            index               = (index + delta + kIntervalCount) % kIntervalCount;
            _draft.interval_ms  = kIntervals[index];
            break;
        }
        case 5:
            _draft.payload = static_cast<std::uint8_t>((_draft.payload + PAYLOAD_COUNT_ + delta) % PAYLOAD_COUNT_);
            break;
        case 6:
            _draft.ending = static_cast<std::uint8_t>((_draft.ending + ENDING_COUNT_ + delta) % ENDING_COUNT_);
            break;
        default:
            break;
    }
}

/* -------------------------------------------------------------------------- */
/*                                   Drawing                                  */
/* -------------------------------------------------------------------------- */
void AppAprecio::render()
{
    if (_config_mode) {
        render_config();
    } else {
        render_main();
    }
    GetHAL().pushCanvas();
}

void AppAprecio::render_main()
{
    auto& c = GetHAL().canvas;
    c.fillScreen(TFT_BLACK);

    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextColor(TFT_WHITE);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString("Aprecio", kMargin, 24);

    c.setFont(&fonts::Font0);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);

    // Port and state share the top line; the framing goes underneath. On
    // this panel the framing line is too long to sit beside the title
    // without running into it.
    const int right_edge = app_header::back_left(c.width()) - 8;
    const char* state = _running ? "SENDING" : "STOPPED";
    c.setTextColor(_running ? THEME_COLOR_SYSTEM_BAR : (uint32_t)0x888888);
    c.drawString(state, right_edge, 14);

    char line[64];
    snprintf(line, sizeof(line), "UART%d", _config.port);
    c.setTextColor((uint32_t)0xAAAAAA);
    c.drawString(line, right_edge - c.textWidth(state) - 6, 14);

    snprintf(line, sizeof(line), "TX=%d %u %u%c%u", _config.tx_pin,
             static_cast<unsigned>(_config.baud), static_cast<unsigned>(_config.data_bits), _config.parity,
             static_cast<unsigned>(_config.stop_bits));
    c.drawString(line, right_edge, 30);

    c.drawLine(0, kHeaderHeight, c.width() - 1, kHeaderHeight, (uint32_t)0x555555);

    /* The way out, in the corner the port line has just been kept clear
     * of. The rest of this band is aprecio's own: it is where the band
     * every other screen carries came from. */
    app_header::draw_back(c);

    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.setTextColor((uint32_t)0xAAAAAA);
    snprintf(line, sizeof(line), "every %s   sent %u", interval_text(_config.interval_ms).c_str(),
             static_cast<unsigned>(_sent_count));
    c.drawString(line, kMargin, kHeaderHeight + 18);

    render_payload_row();
}

/*
 * The payload one character per column, as dhex draws its bottom row.
 * A single centred string overflowed as soon as the twelve character
 * date joined a CR/LF suffix, and the suffix has to stay visible rather
 * than silently moving the line.
 */
void AppAprecio::render_payload_row()
{
    auto& c = GetHAL().canvas;

    if (_last_payload.empty()) {
        c.setFont(&fonts::Font0);
        c.setTextColor((uint32_t)0x888888);
        c.setTextDatum(lgfx::textdatum_t::middle_center);
        c.drawString("nothing sent yet", c.width() / 2, kHeaderHeight + 70);
        c.setTextDatum(lgfx::textdatum_t::baseline_left);
        return;
    }

    // Same grid and constants as dhex's bottom row, deliberately: sixteen
    // fixed columns across the panel. The longest payload here is the
    // fourteen character timestamp plus CR and LF, which fills the grid.
    //
    // dhex drops the ruler ticks and steps the font down on a panel this
    // narrow, where a column is only twelve pixels wide; the same applies
    // here, so the two read alike.
    const int columns = 16;
    const int col_w   = c.width() / columns;
    const int base_y  = c.height() - 6;   // DHEX_SECTION_GAP / 2
    const int line_h  = 14;               // DHEX_BOTTOM_LINE_HEIGHT
    const bool compact = c.width() < 240 || c.height() < 140;

    const lgfx::IFont* big_font = compact ? &fonts::FreeMono9pt7b : &fonts::FreeMono18pt7b;

    c.setTextDatum(lgfx::textdatum_t::baseline_left);

    if (!compact) {
        const int font_nominal = 29;
        const int tick_gap     = 5;
        const int tick_y       = base_y - font_nominal - tick_gap;
        for (int i = 0; i < columns; i++) {
            c.fillRect(i * col_w, tick_y, 1, 2, TFT_WHITE);
        }
    }

    const int count = static_cast<int>(_last_payload.size());
    for (int i = 0; i < count && i < columns; i++) {
        const char ch = _last_payload[i];
        const int x   = i * col_w;

        if (ch == '\r' || ch == '\n') {
            // Inverse video, two letters stacked, so a control character
            // can never be mistaken for a literal C or L byte. col_w - 2
            // leaves a gap so a CR and the LF after it stay distinct.
            const int block_w = col_w - 2;

            c.fillRect(x, base_y - (2 * line_h) + 4, block_w, 2 * line_h, TFT_WHITE);
            c.setFont(&fonts::Font0);
            c.setTextColor(TFT_BLACK);

            const char* top    = ch == '\r' ? "C" : "L";
            const char* bottom = ch == '\r' ? "R" : "F";
            const int letter_x = x + ((block_w - c.textWidth(top)) / 2);
            c.drawString(top, letter_x, base_y - line_h);
            c.drawString(bottom, letter_x, base_y);
        } else if (ch >= 0x20 && ch <= 0x7E) {
            c.setFont(big_font);
            c.setTextColor(TFT_WHITE);
            const char text[2] = {ch, '\0'};
            c.drawString(text, x, base_y);
        } else {
            c.setFont(&fonts::Font0);
            c.setTextColor((uint32_t)0xAAAAAA);
            c.drawString(".", x, base_y);
        }
    }
}

void AppAprecio::render_config()
{
    auto& c = GetHAL().canvas;
    c.fillScreen(TFT_BLACK);

    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextColor(TFT_WHITE);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString("Aprecio config", kMargin, 24);

    c.drawLine(0, kHeaderHeight, c.width() - 1, kHeaderHeight, (uint32_t)0x555555);

    char baud[16], bits[8], stop[8];
    snprintf(baud, sizeof(baud), "%u", static_cast<unsigned>(_draft.baud));
    snprintf(bits, sizeof(bits), "%u", static_cast<unsigned>(_draft.data_bits));
    snprintf(stop, sizeof(stop), "%u", static_cast<unsigned>(_draft.stop_bits));

    static const char* kEndings[ENDING_COUNT_] = {"none", "CR", "LF", "CRLF"};
    const std::string interval = interval_text(_draft.interval_ms);

    // A caret while the text is being typed, so it is obvious the field is
    // taking keys rather than showing a value.
    const std::string text_shown = _text_editing ? _draft_text + "_" : _text;

    const char* values[kFieldCount] = {
        baud,
        bits,
        _draft.parity == 'E' ? "Even" : _draft.parity == 'O' ? "Odd" : "None",
        stop,
        interval.c_str(),
        _draft.payload == PAYLOAD_TIME           ? "Time (HHMM)"
        : _draft.payload == PAYLOAD_DATETIME     ? "Date+time (12)"
        : _draft.payload == PAYLOAD_DATETIME_SEC ? "Date+time+s (14)"
                                                 : "Fixed",
        kEndings[_draft.ending % ENDING_COUNT_],
        text_shown.c_str(),
    };

    c.setFont(&fonts::Font0);

    // Values share a column past the widest label, so they line up.
    int value_x = 0;
    for (int i = 0; i < kFieldCount; i++) {
        const int w = c.textWidth(kFieldNames[i]);
        if (w > value_x) value_x = w;
    }
    value_x += kMargin + kLabelGap;

    const int top = kHeaderHeight + 4;

    // Rows keep a readable height and the list scrolls instead of being
    // squeezed; on a short panel not all of them fit at once.
    constexpr int kMinRowHeight = 11;
    const int available = c.height() - top;
    int row_h  = available / kRowCount;
    int window = kRowCount;
    if (row_h < kMinRowHeight) {
        row_h  = kMinRowHeight;
        window = available / row_h;
    }

    // Keep the selection inside the window, scrolling by whole rows.
    int first = 0;
    if (window < kRowCount) {
        first = _field - window / 2;
        if (first < 0) first = 0;
        if (first > kRowCount - window) first = kRowCount - window;
    }

    for (int slot = 0; slot < window; slot++) {
        const int i = first + slot;
        if (i >= kRowCount) {
            break;
        }
        const int row_top  = top + slot * row_h;
        const int baseline = row_top + row_h - 3;
        const bool current = (i == _field);

        // Selected row is inverted; while its value is being changed the
        // highlight turns to the accent colour, so the two levels can
        // never be confused for one another.
        if (current) {
            c.fillRect(0, row_top, c.width(), row_h,
                       _editing ? (uint32_t)THEME_COLOR_SYSTEM_BAR : (uint32_t)TFT_WHITE);
            c.setTextColor(TFT_BLACK);
        } else {
            c.setTextColor(TFT_WHITE);
        }

        if (i == kSaveRow) {
            c.drawString("Save and apply", kMargin, baseline);
            continue;
        }

        c.drawString(kFieldNames[i], kMargin, baseline);
        c.drawString(values[i], value_x, baseline);
    }
}
