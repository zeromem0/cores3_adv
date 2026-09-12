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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
volatile bool _opening     = false;
std::uint32_t _opening_since_ms = 0;
bool _stall_reported            = false;

/* Whether this link has been encrypted, and whether we have already
 * asked for it. See the note in tick. */
bool _secured       = false;
bool _security_asked = false;

/* The six usages the last report carried. A report is the whole state of
 * the keyboard, not a change to it, so what went down and what came up
 * are worked out by comparing it with the one before. */
std::uint8_t _previous[6] = {0};
std::uint8_t _previous_modifiers = 0;

/*
 * Keys on their way to the matrix, written by the HID event task and read
 * by the main loop.
 *
 * Injecting straight from the callback put the applications' own key
 * handlers on the HID task -- and several of them draw. Two tasks on one
 * SPI panel is a corrupted screen at best. The same shape the CardKB
 * uses: queue here, one event a frame out of the other end, which is
 * also what a screen that polls for the latest event expects.
 */
struct Pending_t {
    bool state;
    std::uint8_t row;
    std::uint8_t col;
};

constexpr std::size_t kQueueDepth = 32;
Pending_t _queue[kQueueDepth];
volatile std::size_t _queue_head = 0;
volatile std::size_t _queue_tail = 0;

void queue_push(bool state, std::uint8_t row, std::uint8_t col)
{
    const std::size_t next = (_queue_tail + 1) % kQueueDepth;
    if (next == _queue_head) {
        return;  /* full: dropping a key beats scrambling the order */
    }
    _queue[_queue_tail] = {state, row, col};
    _queue_tail         = next;
}

bool queue_pop(Pending_t& out)
{
    if (_queue_head == _queue_tail) {
        return false;
    }
    out         = _queue[_queue_head];
    _queue_head = (_queue_head + 1) % kQueueDepth;
    return true;
}

/* A keyboard that has gone leaves whatever it was in the middle of
 * saying; holding a key down and walking out of range should not press
 * it again when the next one arrives. */
void queue_clear()
{
    _queue_head = _queue_tail;
}

/* Where the modifiers this synthesises live on the matrix, taken from
 * the key table's own layout. */
constexpr std::uint8_t kShiftRow = 2, kShiftCol = 1;
constexpr std::uint8_t kFnRow = 2, kFnCol = 0;

/* HID's own report bits, for the shift the remote keyboard was holding
 * when it sent a usage. */
constexpr std::uint8_t kHidLeftShift  = 0x02;
constexpr std::uint8_t kHidRightShift = 0x20;

/* How long an open is given before it is called stuck. The connection
 * attempt alone is allowed thirty seconds by esp_hidh, and discovery
 * follows it, so this is generous rather than tight: it is not a
 * deadline anything acts on, only one after which the screen stops
 * claiming to be getting somewhere. */
constexpr std::uint32_t kOpenDeadlineMs = 45000;

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
            queue_push(true, kFnRow, kFnCol);
        }
        if (shift) {
            queue_push(true, kShiftRow, kShiftCol);
        }
        queue_push(true, row, col);
        return;
    }

    queue_push(false, row, col);
    if (shift) {
        queue_push(false, kShiftRow, kShiftCol);
    }
    if (needs_fn) {
        queue_push(false, kFnRow, kFnCol);
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
            queue_clear();
            _previous_modifiers = 0;
            _have_found         = false;
            _state              = STATE_SCANNING;
            start_scan();
            break;

        default:
            break;
    }
}

/*
 * Started once, then only scanned or not.
 *
 * esp_hidh_init and its NimBLE half can be brought up but not reliably
 * taken down: deinit refuses while a device is open -- "Please disconnect
 * all devices first!" -- and init afterwards answers "Already
 * initialized", so a job stopped and started again never came back. The
 * radio is raised on the first start and stays up until the board is
 * rebooted; stopping the job stops the looking and drops the link, which
 * is what stopping it is actually for.
 */
bool start()
{
    if (_host_running) {
        /* Already up: just start looking again. */
        _state = STATE_SCANNING;
        start_scan();
        return true;
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "nimble init failed: {}", esp_err_to_name(ret));
        say("radio would not start");
        return false;
    }

    /* Said out loud while this is being brought up on a new board: what
     * the HID host is doing is otherwise entirely silent. */
    esp_log_level_set("ESP_HIDH", ESP_LOG_DEBUG);

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
    _state       = STATE_OFF;
    _have_found  = false;
    _open_wanted = false;
    queue_clear();
    say("off");

    if (!_host_running) {
        return;
    }

    /* The looking stops and the link goes; the host itself stays up. See
     * the note on start for why it cannot be taken down and raised
     * again. */
    ble_gap_disc_cancel();

    /*
     * Not while a keyboard is being opened.
     *
     * esp_hidh's disconnect handler signals the waiting open only for a
     * device it already considers connected; dropping the link halfway
     * through discovery leaves that open blocked on a semaphore nobody
     * will ever give, and nothing can reach it afterwards. Waiting for
     * the attempt to end one way or the other is the only way not to
     * strand it.
     */
    if (!_opening) {
        for (int conn = 0; conn < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; conn++) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

/*
 * Opening a keyboard, on a task of its own.
 *
 * esp_ble_hidh_dev_open waits on a semaphore with no timeout for the
 * connection, and then walks the whole attribute database with a
 * blocking read per characteristic. Called from the job's tick, that is
 * the main loop parked inside GATT discovery: the panel stops, the
 * CardKB stops, and the board looks hung from the moment the keyboard is
 * found -- which is exactly how this first went wrong. Nothing here is
 * wanted on the main loop, and nothing here needs to be.
 */
void open_task(void* param)
{
    (void)param;

    if (esp_hidh_dev_open(_found.val, ESP_HID_TRANSPORT_BLE, _found.type) == nullptr) {
        mclog::tagError(_tag, "open refused");
        say("open refused");
        _have_found = false;
        if (_state != STATE_OFF) {
            _state = STATE_SCANNING;
            start_scan();
        }
    }

    _opening = false;
    vTaskDelete(nullptr);
}

void tick(std::uint32_t now_ms)
{
    (void)now_ms;

    /* One key a pass, which is the rate a scanned matrix delivers them
     * and the rate a screen that polls for the latest event can see
     * them. */
    Pending_t pending;
    if (queue_pop(pending)) {
        GetHAL().keyboard.injectKeyEventRaw({pending.state, pending.row, pending.col});
    }

    /*
     * The link, encrypted, while the HID host is still discovering it.
     *
     * A keyboard keeps its report map behind encryption: read on a clear
     * link it comes back ATT 0x0F, insufficient encryption, and without
     * the map esp_hidh never learns the device is a keyboard and every
     * later read fails the same way. Neither NimBLE nor esp_hidh pairs on
     * its own, and esp_hidh owns the connection's callback, so there is
     * nowhere to be told the link came up -- it is watched for instead.
     * Asked as soon as the connection exists, which is some seconds
     * before discovery reaches the map.
     */
    if (_opening && !_secured && !_security_asked) {
        struct ble_gap_conn_desc desc = {};
        if (ble_gap_conn_find_by_addr(&_found, &desc) == 0) {
            if (desc.sec_state.encrypted) {
                _secured = true;
            } else {
                _security_asked = true;
                const int rc    = ble_gap_security_initiate(desc.conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    mclog::tagError(_tag, "pairing refused: {}", rc);
                } else {
                    mclog::tagInfo(_tag, "asking {} to encrypt the link", _peer);
                }
            }
        }
    }

    /*
     * An open that never ends, said out loud.
     *
     * Discovery walks the whole attribute database with a blocking read
     * apiece, and a keyboard that stops answering halfway leaves that
     * walk waiting on a semaphore esp_hidh only gives for a device it
     * already counts as connected. Nothing here can reach it, so the
     * honest thing is to name it rather than sit at "opening" for ever.
     */
    if (_opening && !_stall_reported &&
        (GetHAL().millis() - _opening_since_ms) > kOpenDeadlineMs) {
        _stall_reported = true;
        mclog::tagWarn(_tag, "{} stopped answering during discovery", _peer);
        std::snprintf(_detail, sizeof(_detail), "%s went quiet, reboot to retry", _peer);
    }

    if (!_open_wanted) {
        return;
    }
    _open_wanted = false;

    if (!_have_found || _opening) {
        return;
    }

    std::snprintf(_peer, sizeof(_peer), "%02x:%02x:%02x:%02x:%02x:%02x", _found.val[5],
                  _found.val[4], _found.val[3], _found.val[2], _found.val[1], _found.val[0]);

    _state = STATE_OPENING;
    std::snprintf(_detail, sizeof(_detail), "opening %s", _peer);
    mclog::tagInfo(_tag, "opening {}", _peer);

    _opening          = true;
    _secured          = false;
    _security_asked   = false;
    _opening_since_ms = GetHAL().millis();
    _stall_reported   = false;
    if (xTaskCreate(open_task, "blekbd_open", 5120, nullptr, 5, nullptr) != pdPASS) {
        mclog::tagError(_tag, "no room for the opening task");
        say("could not start the open");
        _opening    = false;
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
