/*
 * SPDX-License-Identifier: MIT
 */
#include "ble_keyboard.h"

#include <hal/hal.h>
#include <hal/utils/jobs/jobs.h>
#include <mooncake_log.h>

#include <esp_hidh.h>
#include <esp_hid_common.h>

extern "C" {
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
void ble_store_config_init(void);
}

#include <cstdio>
#include <cstring>

namespace ble_keyboard {

namespace {

const std::string _tag = "blekbd";

enum State_t {
    STATE_OFF,
    STATE_SCANNING,
    STATE_OPENING,
    STATE_CONNECTED,
};

State_t _state = STATE_OFF;
char _detail[64] = {0};
char _peer[24]   = {0};

std::uint8_t _own_addr_type = 0;
bool _host_running          = false;

/* The address the scan settled on, kept until the host is asked to open
 * it -- opening happens off the discovery callback, which must not
 * block. */
ble_addr_t _found          = {};
bool _have_found           = false;
volatile bool _open_wanted = false;

/* The six usages the last report carried. A report is the whole state of
 * the keyboard, not a change to it, so what went down and what came up
 * are worked out by comparing it with the one before. */
std::uint8_t _previous[6] = {0};
std::uint8_t _previous_modifiers = 0;

/* Where the modifiers this synthesises live on the matrix, taken from
 * the key table's own layout. */
constexpr std::uint8_t kShiftRow = 2, kShiftCol = 1;
constexpr std::uint8_t kFnRow = 2, kFnCol = 0;

/* HID's own report bits, for the shift the remote keyboard was holding
 * when it sent a usage. */
constexpr std::uint8_t kHidLeftShift  = 0x02;
constexpr std::uint8_t kHidRightShift = 0x20;

void start_scan();

void say(const char* text)
{
    std::snprintf(_detail, sizeof(_detail), "%s", text);
}

/* -------------------------------------------------------------------------- */
/*                             usages to the matrix                           */
/* -------------------------------------------------------------------------- */

/*
 * One usage, pressed or released, as the matrix would have produced it.
 *
 * Shift and Fn are held around the key exactly as fingers would hold
 * them, which is what the on-screen keyboard does too: everything
 * downstream then sees a sequence it cannot tell from a real one.
 */
void send_usage(std::uint8_t usage, bool down, std::uint8_t modifiers)
{
    auto& keyboard = GetHAL().keyboard;

    std::uint8_t row = 0, col = 0;
    bool needs_shift = false, needs_fn = false;
    if (!keyboard.findPositionByKeyCode(usage, row, col, needs_shift, needs_fn)) {
        return;
    }

    /* The remote's own shift counts as well as the key map's: an "A"
     * arrives as the usage for "a" with the shift bit set beside it. */
    const bool shift = needs_shift || (modifiers & (kHidLeftShift | kHidRightShift)) != 0;

    if (down) {
        if (needs_fn) {
            keyboard.injectKeyEventRaw({true, kFnRow, kFnCol});
        }
        if (shift) {
            keyboard.injectKeyEventRaw({true, kShiftRow, kShiftCol});
        }
        keyboard.injectKeyEventRaw({true, row, col});
        return;
    }

    keyboard.injectKeyEventRaw({false, row, col});
    if (shift) {
        keyboard.injectKeyEventRaw({false, kShiftRow, kShiftCol});
    }
    if (needs_fn) {
        keyboard.injectKeyEventRaw({false, kFnRow, kFnCol});
    }
}

bool contains(const std::uint8_t* list, std::uint8_t usage)
{
    for (int i = 0; i < 6; i++) {
        if (list[i] == usage) {
            return true;
        }
    }
    return false;
}

/* A boot-protocol keyboard report: modifiers, a reserved byte, then six
 * usages. Anything shorter is not one and is left alone. */
void handle_report(const std::uint8_t* data, std::size_t length)
{
    if (data == nullptr || length < 8) {
        return;
    }

    const std::uint8_t modifiers = data[0];
    const std::uint8_t* keys     = data + 2;

    for (int i = 0; i < 6; i++) {
        if (_previous[i] != 0 && !contains(keys, _previous[i])) {
            send_usage(_previous[i], false, _previous_modifiers);
        }
    }
    for (int i = 0; i < 6; i++) {
        if (keys[i] != 0 && !contains(_previous, keys[i])) {
            send_usage(keys[i], true, modifiers);
        }
    }

    std::memcpy(_previous, keys, sizeof(_previous));
    _previous_modifiers = modifiers;
}

/* -------------------------------------------------------------------------- */
/*                                  the radio                                 */
/* -------------------------------------------------------------------------- */

/* Whether an advertisement is a keyboard: either it says so in its
 * appearance, or it offers the HID service. */
bool looks_like_a_keyboard(const ble_hs_adv_fields& fields)
{
    if (fields.appearance_is_present) {
        /* 0x03C0 is the generic HID category; keyboards are 0x03C1. */
        if ((fields.appearance & 0xFFC0) == 0x03C0) {
            return true;
        }
    }

    for (int i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == 0x1812) {  // human interface device
            return true;
        }
    }
    return false;
}

int gap_event(struct ble_gap_event* event, void* arg)
{
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields = {};
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
                return 0;
            }
            if (!looks_like_a_keyboard(fields)) {
                return 0;
            }

            _found      = event->disc.addr;
            _have_found = true;

            /* Opening talks to the host and takes its time; the
             * discovery callback is not the place. The job's own tick
             * picks this up. */
            ble_gap_disc_cancel();
            _open_wanted = true;
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (!_have_found && _state == STATE_SCANNING) {
                /* Nothing yet. Keep looking rather than stopping: a
                 * keyboard is usually asleep until a key is pressed. */
                start_scan();
            }
            return 0;

        default:
            return 0;
    }
}

void start_scan()
{
    struct ble_gap_disc_params params = {};
    params.itvl          = 0;
    params.window        = 0;
    params.filter_policy = 0;
    params.limited       = 0;
    params.passive       = 0;  // active, so names and appearances arrive
    params.filter_duplicates = 1;

    const int rc = ble_gap_disc(_own_addr_type, 20000, &params, gap_event, nullptr);
    if (rc != 0) {
        mclog::tagError(_tag, "scan failed: {}", rc);
        say("scan failed");
        return;
    }

    _state = STATE_SCANNING;
    say("looking for a keyboard");
}

void on_reset(int reason)
{
    mclog::tagError(_tag, "host reset, reason {}", reason);
}

void on_sync()
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        mclog::tagError(_tag, "no usable address");
        return;
    }
    if (ble_hs_id_infer_auto(0, &_own_addr_type) != 0) {
        mclog::tagError(_tag, "no address type");
        return;
    }
    start_scan();
}

void host_task(void* param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* -------------------------------------------------------------------------- */
/*                                   the job                                  */
/* -------------------------------------------------------------------------- */

void hidh_callback(void* handler_args, esp_event_base_t base, std::int32_t id, void* event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidh_event_t event         = (esp_hidh_event_t)id;
    esp_hidh_event_data_t* param   = (esp_hidh_event_data_t*)event_data;

    switch (event) {
        case ESP_HIDH_OPEN_EVENT:
            if (param->open.status == ESP_OK) {
                _state = STATE_CONNECTED;
                std::snprintf(_detail, sizeof(_detail), "connected to %s",
                              _peer[0] != '\0' ? _peer : "a keyboard");
                mclog::tagInfo(_tag, "keyboard open");
            } else {
                mclog::tagError(_tag, "open failed");
                say("could not open it");
                _have_found = false;
                _state      = STATE_SCANNING;
                start_scan();
            }
            break;

        case ESP_HIDH_INPUT_EVENT:
            handle_report(param->input.data, param->input.length);
            break;

        case ESP_HIDH_CLOSE_EVENT:
            mclog::tagInfo(_tag, "keyboard gone");
            std::memset(_previous, 0, sizeof(_previous));
            _previous_modifiers = 0;
            _have_found         = false;
            _state              = STATE_SCANNING;
            start_scan();
            break;

        default:
            break;
    }
}

bool start()
{
    if (_host_running) {
        return true;
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "nimble init failed: {}", esp_err_to_name(ret));
        say("radio would not start");
        return false;
    }

    esp_hidh_config_t config = {};
    config.callback          = hidh_callback;
    config.event_stack_size  = 4096;
    config.callback_arg      = nullptr;

    ret = esp_hidh_init(&config);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "hid host init failed: {}", esp_err_to_name(ret));
        say("hid host would not start");
        return false;
    }

    /*
     * After the HID host, not before it.
     *
     * esp_ble_hidh_init ends by putting its own reset and sync callbacks
     * into ble_hs_cfg, and its sync callback is empty -- the comment in
     * it says "no need to perform anything here". Set first, ours were
     * quietly replaced, nothing ever started the scan, and the job sat
     * at "starting" for ever with a radio that was up and idle.
     */
    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 0;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Bonds in NVS, so a keyboard paired once comes back by itself. */
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    _host_running = true;
    _state        = STATE_SCANNING;
    say("starting");
    return true;
}

void stop()
{
    /* Best effort. Bringing the host down while a connection is open is
     * not something NimBLE enjoys, so the link goes first. */
    _state       = STATE_OFF;
    _have_found  = false;
    _open_wanted = false;
    say("off");

    if (!_host_running) {
        return;
    }

    ble_gap_disc_cancel();
    esp_hidh_deinit();

    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    _host_running = false;
}

void tick(std::uint32_t now_ms)
{
    (void)now_ms;

    if (!_open_wanted) {
        return;
    }
    _open_wanted = false;

    if (!_have_found) {
        return;
    }

    std::snprintf(_peer, sizeof(_peer), "%02x:%02x:%02x:%02x:%02x:%02x", _found.val[5],
                  _found.val[4], _found.val[3], _found.val[2], _found.val[1], _found.val[0]);

    _state = STATE_OPENING;
    std::snprintf(_detail, sizeof(_detail), "opening %s", _peer);
    mclog::tagInfo(_tag, "opening {}", _peer);

    if (esp_hidh_dev_open(_found.val, ESP_HID_TRANSPORT_BLE, _found.type) == nullptr) {
        mclog::tagError(_tag, "open refused");
        say("open refused");
        _have_found = false;
        start_scan();
    }
}

const char* detail()
{
    return _detail[0] != '\0' ? _detail : "idle";
}

}  // namespace

void register_ble_keyboard_job()
{
    jobs::Job job = {};
    job.name      = "blekbd";
    job.summary   = "a bluetooth keyboard, as the host";
    job.start     = start;
    job.stop      = stop;
    job.tick      = tick;
    job.detail    = detail;

    /* Off unless asked. The radio is paid for in internal RAM, which is
     * the one thing this board is short of. */
    job.autostart = false;

    jobs::register_job(job);
}

}  // namespace ble_keyboard
