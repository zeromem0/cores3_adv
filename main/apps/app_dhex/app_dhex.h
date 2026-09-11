/*
 * dhex -- hex/ascii UART dump.
 *
 * The application itself is dhex.c, carried in unmodified. This
 * wrapper hosts it: it owns the context and graphics surface, drives
 * the application's lifecycle from mooncake's, and translates key
 * events into the character codes it expects.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>

struct dhex_context;
struct dhex_gfx;

class AppDhex : public mooncake::AppAbility {
public:
    AppDhex();
    ~AppDhex();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    dhex_context* _ctx        = nullptr;
    dhex_gfx* _gfx            = nullptr;
    int _handle_key_event_slot_id     = -1;
    int _handle_key_event_raw_slot_id = -1;
    std::uint32_t _last_tick_ms   = 0;
    // AppAbility::baseUpdate() forces StateRunning right after onOpen()
    // returns, so a close() from inside onOpen() is discarded. A failed
    // start has to be carried over to onRunning() to actually take effect.
    bool _close_requested = false;

    void feed_char(std::uint8_t ch);
};
