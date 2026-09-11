#include "cardkb.h"

#include <M5Unified.h>
#include <hal/hal.h>
#include <mooncake_log.h>

namespace cardkb {

namespace {

constexpr char _tag[] = "cardkb";

/* Grove Port A, which M5Unified already owns as its external bus. Going
 * through that rather than opening a second master on the same pins is
 * the whole difference between the keyboard answering and NACKing every
 * read. */
constexpr uint8_t kAddress = 0x5f;
constexpr uint32_t kSpeedHz = 100000;
constexpr uint32_t kPollIntervalMs = 15;

/* CardKB reports the arrows and Escape outside printable ASCII. On a
 * Cardputer those live on the Fn layer, so they are injected with Fn
 * held rather than as characters. */
constexpr uint8_t kKeyEsc = 0x1b;
constexpr uint8_t kKeyTab = 0x09;
constexpr uint8_t kKeyBackspace = 0x08;
constexpr uint8_t kKeyEnter = 0x0d;
constexpr uint8_t kKeyLeft = 0xb4;
constexpr uint8_t kKeyUp = 0xb5;
constexpr uint8_t kKeyDown = 0xb6;
constexpr uint8_t kKeyRight = 0xb7;

bool _present = false;
uint32_t _last_poll_ms = 0;

struct Position {
    uint8_t row;
    uint8_t col;
    bool fn;
    bool shift;
};

/* The matrix positions this firmware's key table already defines. */
constexpr Position kEscape = {0, 0, true, false};
constexpr Position kDelete = {0, 13, false, false};
constexpr Position kTab = {1, 0, false, false};
constexpr Position kEnter = {2, 13, false, false};
constexpr Position kUp = {2, 11, true, false};
constexpr Position kLeft = {3, 10, true, false};
constexpr Position kDown = {3, 11, true, false};
constexpr Position kRight = {3, 12, true, false};

void inject_key(uint8_t row, uint8_t col, bool pressed)
{
    Keyboard::KeyEventRaw_t key;
    key.row = row;
    key.col = col;
    key.state = pressed;
    GetHAL().keyboard.injectKeyEventRaw(key);
}

/*
 * Screens read input by asking for the latest event rather than by
 * subscribing, so an event has to stand for a whole frame to be seen.
 * Pressing and releasing in one pass leaves only the release behind and
 * the key looks dead -- which is exactly how this first went wrong.
 * Events are queued and handed out one per frame, the way a scanned
 * matrix delivers them.
 */
struct PendingKey {
    uint8_t row;
    uint8_t col;
    bool state;
};

constexpr size_t kQueueDepth = 12;
PendingKey _queue[kQueueDepth];
size_t _queue_head = 0;
size_t _queue_tail = 0;

bool queue_empty()
{
    return _queue_head == _queue_tail;
}

void queue_push(uint8_t row, uint8_t col, bool state)
{
    const size_t next = (_queue_tail + 1) % kQueueDepth;
    if (next == _queue_head) {
        return;  /* full: dropping a key beats scrambling the order */
    }
    _queue[_queue_tail] = {row, col, state};
    _queue_tail = next;
}

bool queue_pop(PendingKey& out)
{
    if (queue_empty()) {
        return false;
    }
    out = _queue[_queue_head];
    _queue_head = (_queue_head + 1) % kQueueDepth;
    return true;
}

/* Fn and shift are ordinary matrix positions, so wrapping a key in them
 * is just three taps in the right order -- the same shape the remote
 * page uses to type over HTTP. */
void queue_tap(const Position& pos)
{
    if (pos.fn) {
        queue_push(2, 0, true);
    }
    if (pos.shift) {
        queue_push(2, 1, true);
    }
    queue_push(pos.row, pos.col, true);
    queue_push(pos.row, pos.col, false);
    if (pos.shift) {
        queue_push(2, 1, false);
    }
    if (pos.fn) {
        queue_push(2, 0, false);
    }
}

bool translate(uint8_t code, Position& out)
{
    switch (code) {
        case kKeyEsc: out = kEscape; return true;
        case kKeyTab: out = kTab; return true;
        case kKeyBackspace: out = kDelete; return true;
        case kKeyEnter: out = kEnter; return true;
        case kKeyUp: out = kUp; return true;
        case kKeyDown: out = kDown; return true;
        case kKeyLeft: out = kLeft; return true;
        case kKeyRight: out = kRight; return true;
        default: break;
    }

    if (code < 0x20 || code > 0x7e) {
        return false;
    }

    uint8_t row = 0;
    uint8_t col = 0;
    bool needs_shift = false;
    if (!GetHAL().keyboard.findKeyPosition(static_cast<char>(code), row, col, needs_shift)) {
        return false;
    }
    out = {row, col, false, needs_shift};
    return true;
}

bool read_byte(uint8_t& code)
{
    if (!M5.Ex_I2C.start(kAddress, true, kSpeedHz)) {
        return false;
    }
    const bool ok = M5.Ex_I2C.read(&code, 1, true);
    M5.Ex_I2C.stop();
    return ok;
}

}  // namespace

bool init()
{
    if (!M5.Ex_I2C.begin()) {
        mclog::tagWarn(_tag, "port A bus unavailable");
        return false;
    }

    /* A bare probe is enough: the keyboard acknowledges its address even
     * when it has no key to report. */
    if (!M5.Ex_I2C.start(kAddress, false, kSpeedHz)) {
        M5.Ex_I2C.stop();
        mclog::tagInfo(_tag, "no keyboard on port A; touch only");
        return false;
    }
    M5.Ex_I2C.stop();

    _present = true;
    mclog::tagInfo(_tag, "keyboard ready on port A");
    return true;
}

bool isPresent()
{
    return _present;
}

void update()
{
    if (!_present) {
        return;
    }

    /* Whatever was injected last frame has been seen by now; clearing it
     * is what the matrix scan did at the top of its own update, and
     * without it a polled screen would act on the same key for ever. */
    GetHAL().keyboard.clearKeyEvent();

    PendingKey pending;
    if (queue_pop(pending)) {
        inject_key(pending.row, pending.col, pending.state);
        return;
    }

    /* Frame rate is far faster than anyone types, and every poll costs a
     * bus transaction. */
    const uint32_t now = GetHAL().millis();
    if (now - _last_poll_ms < kPollIntervalMs) {
        return;
    }
    _last_poll_ms = now;

    uint8_t code = 0;
    /* One report per press, and a repeat about every 850 ms while the
     * key is held -- measured on this keyboard. The repeat is useful for
     * scrolling a list, so it is passed through as another press. */
    const bool got = read_byte(code) && code != 0;

    if (!got) {
        return;
    }


    Position pos;
    if (translate(code, pos)) {
        queue_tap(pos);
    }
}

}  // namespace cardkb
