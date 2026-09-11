/*
 * aprecio -- periodic serial transmitter.
 *
 * Sends a payload out of a configurable serial port at a configurable
 * interval. The counterpart to dhex, which watches the same line: this
 * one drives it.
 *
 * The port settings screen mirrors dhex's, down to the field order, so
 * the two applications feel the same. Only one of them can hold the port
 * at a time, which on this board is the console UART; both install the
 * driver on open and release it on close.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>
#include <string>

class AppAprecio : public mooncake::AppAbility {
public:
    AppAprecio();
    ~AppAprecio();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum Payload_t {
        PAYLOAD_FIXED,
        PAYLOAD_TIME,         // HHMM, 4 characters
        PAYLOAD_DATETIME,     // YYYYMMDDHHMM, 12 characters
        PAYLOAD_DATETIME_SEC, // YYYYMMDDHHMMSS, 14 characters
        PAYLOAD_COUNT_,
    };

    enum Ending_t {
        ENDING_NONE,
        ENDING_CR,
        ENDING_LF,
        ENDING_CRLF,
        ENDING_COUNT_,
    };

    struct Config_t {
        int port          = 0;
        int tx_pin        = 0;
        int rx_pin        = 0;
        std::uint32_t baud = 9600;
        std::uint8_t data_bits = 8;
        char parity            = 'N';
        std::uint8_t stop_bits = 1;
        std::uint32_t interval_ms = 1000;
        std::uint8_t payload      = PAYLOAD_TIME;
        std::uint8_t ending       = ENDING_CRLF;
    };

    Config_t _config;
    Config_t _draft;

    // The fixed payload. Typed in on the settings screen, which needs a
    // keyboard -- locally on boards that have one, over remoted on those
    // that do not.
    std::string _text;
    std::string _draft_text;
    bool _text_editing = false;

    bool _uart_ready   = false;
    bool _running      = false;
    bool _config_mode  = false;
    // Two levels inside the settings screen: picking a row, then changing
    // that row's value. Turning means the same thing at both levels, so
    // which way the encoder is currently sending never matters.
    bool _editing       = false;
    std::uint8_t _field = 0;

    std::uint32_t _next_send_ms = 0;
    std::uint32_t _sent_count   = 0;
    std::string _last_payload;

    int _key_slot     = -1;
    int _key_raw_slot = -1;
    bool _close_requested = false;

    void load_config();
    void save_config();
    void defaults(Config_t& cfg);

    bool uart_start(const Config_t& cfg);
    void uart_stop();

    std::string build_payload();
    void send_now();

    void adjust_field(int delta);
    void render();
    void render_main();
    void render_payload_row();
    void render_config();

    void handle_char(std::uint8_t ch);
};
