/*
 * SPDX-License-Identifier: MIT
 */
#include "remoted.h"
#include "remoted_page.h"
#include "screenshot.h"

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mooncake_log.h>

// Relative on purpose: ESP-IDF ships its own hal component, so <hal.h>
// does not reliably resolve to this firmware's HAL.
#include "../../hal.h"

#include <cstring>
#include <string>

namespace remoted {

namespace {

const std::string _tag = "remoted";

// Panel geometry. The three HAL sprites tile it exactly:
//   keyboard bar at (0, 0), system bar right of it, canvas below that.
constexpr int kFrameWidth = 240;
constexpr int kFrameHeight = 135;

// The snapshot is kept as a handful of separate bands rather than one
// block. A single 64 KB allocation does not survive alongside WiFi, the
// HTTP server and the three sprites, and failed outright; bands of a few
// KB fit a fragmented heap and still let a whole frame be captured in one
// pass. They double as the chunk size on the wire.
constexpr int kBandRows = 16;
constexpr int kBandCount = (kFrameHeight + kBandRows - 1) / kBandRows;

/* A whole frame is 65 KB of buffer, which the heap does not have while
 * something large is running -- the emulator holds 62 KB the moment it
 * is open. Rather than give up the mirror or coarsen it, one quarter of
 * the panel goes out at a time, at full resolution, a different quarter
 * on each pass. The page keeps what it has already been sent and paints
 * each quarter into it as it arrives, so a screen that is not changing
 * fills in completely within four passes, and one that is changing
 * shows its quarters at slightly different ages.
 *
 * The two halves overlap by a row rather than being 67 and 68 tall, so
 * every quarter is the same size and one buffer serves all four. */
constexpr int kTileWidth = kFrameWidth / 2;    // 120
constexpr int kTileHeight = (kFrameHeight / 2) + 1;  // 68
constexpr int kTileCount = 4;

int _tile = 0;           // the quarter to capture next
int _snapshot_tile = 0;  // the quarter the buffer actually holds
bool _tiling = false;

int tile_origin_x(int tile)
{
    return _tiling ? ((tile & 1) ? kTileWidth : 0) : 0;
}

int tile_origin_y(int tile)
{
    return _tiling ? ((tile & 2) ? (kFrameHeight - kTileHeight) : 0) : 0;
}

int frame_width()
{
    return _tiling ? kTileWidth : kFrameWidth;
}

int frame_height()
{
    return _tiling ? kTileHeight : kFrameHeight;
}

int band_rows(int band)
{
    const int rows = frame_height() - (band * kBandRows);
    if (rows <= 0) {
        return 0;
    }
    return (rows < kBandRows) ? rows : kBandRows;
}

// How often a fresh snapshot is taken while a client is watching, and how
// long after the last request the snapshot buffer is handed back to the
// heap. 64 KB is worth holding only while somebody is actually looking.
constexpr uint32_t kCaptureIntervalMs = 200;
constexpr uint32_t kIdleReleaseMs = 10000;

// Reading the panel back shares the bus with whatever is drawing on it,
// so a full-screen application is sampled less often than a sprite would
// be. A quarter takes some eight milliseconds, and four of these fill
// the picture in under two seconds.
constexpr uint32_t kPanelCaptureIntervalMs = 400;

// "M5F2": the header carries where the piece goes, which "M5F1" did not.
constexpr uint32_t kFrameMagic = 0x3246354D;

// Deep enough for a short typed string plus its shift wrapping.
constexpr int kKeyQueueLength = 64;

// Injected presses are released one tick later so applications that act on
// release, like the launcher opening an app, still see a complete press.
constexpr int kKeyHoldMs = 30;

struct QueuedKey_t {
    uint8_t row;
    uint8_t col;
    bool state;
};

httpd_handle_t _server = nullptr;

/* Routes other modules asked to have served here. Kept so they can be
 * put back every time the server restarts with the network; four of the
 * handlers the server allows are this module's own, and the irrigation
 * editor, the application list and the screenshot take four more. */
constexpr size_t kMaxExtraRoutes = 8;
httpd_uri_t _extra_routes[kMaxExtraRoutes];
size_t _extra_count = 0;

/* What the front page offers as links. Separate from the routes above
 * because the two are not the same list: /irrig/save is a route nobody
 * types, and /shot is one page reachable two ways. */
struct Link_t {
    const char* path;
    const char* title;
};
constexpr size_t kMaxLinks = 8;
Link_t _links[kMaxLinks];
size_t _link_count = 0;
QueueHandle_t _key_queue = nullptr;
std::string _address;
bool _was_connected = false;

// Snapshot of the panel, filled on the main task and served from here.
uint16_t* _bands[kBandCount] = {};
bool _bands_ready = false;
SemaphoreHandle_t _snapshot_lock = nullptr;
bool _snapshot_valid = false;
uint32_t _snapshot_ms = 0;
uint32_t _last_request_ms = 0;
bool _alloc_failure_logged = false;

void release_bands()
{
    for (int i = 0; i < kBandCount; i++) {
        free(_bands[i]);
        _bands[i] = nullptr;
    }
    _bands_ready = false;
    _snapshot_valid = false;
}

bool reserve_for(bool tiling)
{
    _tiling = tiling;
    for (int i = 0; i < kBandCount; i++) {
        const int rows = band_rows(i);
        if (rows <= 0) {
            continue;
        }
        _bands[i] = static_cast<uint16_t*>(malloc(frame_width() * rows * sizeof(uint16_t)));
        if (_bands[i] == nullptr) {
            return false;
        }
    }
    return true;
}

/* Quarters while an application owns the panel, whether or not a whole
 * frame would fit: that application is the one short of memory -- the
 * emulator wants 48 KB in one piece -- and 48 KB of mirror buffer is
 * worth less than the thing being mirrored being able to run at all. */
bool wanted_tiling()
{
    return GetHAL().isFullScreenApp();
}

bool reserve_bands()
{
    const bool wanted = wanted_tiling();

    if (reserve_for(wanted)) {
        _bands_ready = true;
        if (wanted) {
            mclog::tagInfo(_tag, "mirroring in {}x{} quarters", frame_width(), frame_height());
        }
        return true;
    }
    release_bands();

    mclog::tagWarn(_tag, "frame will not fit, {} bytes free, largest {}", esp_get_free_heap_size(),
                   heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!wanted && reserve_for(true)) {
        _bands_ready = true;
        mclog::tagInfo(_tag, "mirroring in {}x{} quarters", frame_width(), frame_height());
        return true;
    }

    release_bands();
    _tiling = false;
    return false;
}

void queue_key(uint8_t row, uint8_t col, bool state)
{
    if (_key_queue == nullptr) {
        return;
    }
    const QueuedKey_t key = {row, col, state};
    xQueueSend(_key_queue, &key, 0);
}

void queue_tap(uint8_t row, uint8_t col, bool fn, bool shift)
{
    // Fn and shift are ordinary matrix positions, so wrapping a key in
    // them is just three taps in the right order.
    if (fn) {
        queue_key(2, 0, true);
    }
    if (shift) {
        queue_key(2, 1, true);
    }
    queue_key(row, col, true);
    queue_key(row, col, false);
    if (shift) {
        queue_key(2, 1, false);
    }
    if (fn) {
        queue_key(2, 0, false);
    }
}

/* ------------------------------- Handlers ------------------------------- */

/*
 * The page, with the list of everything else this board serves dropped
 * into the hole left for it. Built here rather than fetched by the page
 * because the answer never changes while the board is up, and a second
 * request for six links would cost more than sending them.
 */
esp_err_t handle_root(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html");

    static const char kMarker[] = "<!--LINKS-->";
    const char* hole = strstr(REMOTED_PAGE, kMarker);
    if (hole == nullptr) {
        return httpd_resp_send(req, REMOTED_PAGE, HTTPD_RESP_USE_STRLEN);
    }

    std::string links;
    for (size_t i = 0; i < _link_count; i++) {
        links += "<a href=\"";
        links += _links[i].path;
        links += "\">";
        links += _links[i].title;
        links += "</a>";
    }
    if (_link_count == 0) {
        links = "<span>nothing else is registered</span>";
    }

    esp_err_t ret = httpd_resp_send_chunk(req, REMOTED_PAGE, hole - REMOTED_PAGE);
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, links.c_str(), links.size());
    }
    if (ret == ESP_OK) {
        const char* rest = hole + sizeof(kMarker) - 1;
        ret = httpd_resp_send_chunk(req, rest, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ret;
}

// Composes the three HAL sprites into one frame. Pixels come out in
// M5GFX's own 16bpp layout, which is RGB565 with the two bytes swapped,
// and go on the wire that way; the page turns them round while it is
// already walking every pixel to build ImageData.
//
// Called only from the main task. Reading the sprites from the HTTP task
// instead sampled a screen that was still being redrawn, so a launcher
// frame arrived stitched together out of several animation steps.
void capture_frame()
{
    /* Not while /shot has the panel: that read runs on the HTTP task and
     * this one on the main task, and two tasks on the same bus is a
     * corrupted picture at best. */
    if (screenshot::is_frozen()) {
        return;
    }

    auto& hal = GetHAL();

    /* An application that has taken the whole panel draws onto the glass
     * and not into the sprites, so the sprites hold whatever was there
     * before it opened. Reading the panel back is the only way to mirror
     * what is actually on it. It costs more -- the panel reads at 16 MHz
     * against a sprite in RAM -- which is why it is not the usual path. */
    const int bar_w       = hal.canvasKeyboardBar.width();
    const bool from_panel = hal.isFullScreenApp();

    _snapshot_tile     = _tile;
    const int origin_x = tile_origin_x(_tile);
    const int origin_y = tile_origin_y(_tile);
    const int width    = frame_width();

    for (int band = 0; band < kBandCount; band++) {
        const int rows = band_rows(band);
        if (rows <= 0) {
            break;
        }

        /* Reading the glass takes whole bands at once. */
        if (from_panel) {
            hal.display.readRect(origin_x, origin_y + band * kBandRows, width, rows, _bands[band]);
            continue;
        }

        for (int i = 0; i < rows; i++) {
            const int src_y = origin_y + (band * kBandRows) + i;
            uint16_t* out   = _bands[band] + (i * width);

            /* Out of the sprites a row may straddle the keyboard bar and
             * the application canvas holding the rest of that line. */
            const int right = origin_x + width;
            if (origin_x < bar_w) {
                const int take = (right < bar_w ? right : bar_w) - origin_x;
                hal.canvasKeyboardBar.readRect(origin_x, src_y, take, 1, out);
            }
            if (right > bar_w) {
                const int from = (origin_x > bar_w ? origin_x : bar_w) - bar_w;
                const int take = right - bar_w - from;
                uint16_t* into = out + (bar_w > origin_x ? bar_w - origin_x : 0);
                hal.canvas.readRect(from, src_y, take, 1, into);
            }
        }
    }

    _snapshot_valid = true;
}

esp_err_t handle_frame(httpd_req_t* req)
{
    _last_request_ms = GetHAL().millis();

    // The first request after an idle stretch lands before the main loop
    // has had a chance to allocate the buffer and fill it.
    for (int wait = 0; wait < 25 && !_snapshot_valid; wait++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!_snapshot_valid) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Where this piece belongs and how big the whole is, so the page can
     * keep what it already has and paint only what just arrived. */
    uint8_t header[16];
    memcpy(header, &kFrameMagic, 4);
    const uint16_t fields[6] = {
        (uint16_t)kFrameWidth,               (uint16_t)kFrameHeight,
        (uint16_t)tile_origin_x(_snapshot_tile), (uint16_t)tile_origin_y(_snapshot_tile),
        (uint16_t)frame_width(),             (uint16_t)frame_height(),
    };
    memcpy(header + 4, fields, sizeof(fields));

    esp_err_t ret = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(header), sizeof(header));

    // Held across the whole send, so the main task cannot start overwriting
    // the bands halfway through it.
    xSemaphoreTake(_snapshot_lock, portMAX_DELAY);
    for (int band = 0; ret == ESP_OK && band < kBandCount; band++) {
        const int rows = band_rows(band);
        if (rows <= 0 || _bands[band] == nullptr) {
            continue;
        }
        ret = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(_bands[band]),
                                    rows * frame_width() * sizeof(uint16_t));
    }
    xSemaphoreGive(_snapshot_lock);

    httpd_resp_send_chunk(req, nullptr, 0);
    return ret;
}

bool query_int(const char* query, const char* key, int& out)
{
    char value[8];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    out = atoi(value);
    return true;
}

esp_err_t handle_key(httpd_req_t* req)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    }

    int row = -1;
    int col = -1;
    if (!query_int(query, "r", row) || !query_int(query, "c", col) ||
        row < 0 || row > 3 || col < 0 || col > 13) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key position");
    }

    int fn = 0;
    int shift = 0;
    query_int(query, "fn", fn);
    query_int(query, "shift", shift);

    queue_tap(static_cast<uint8_t>(row), static_cast<uint8_t>(col), fn != 0, shift != 0);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_text(httpd_req_t* req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    }

    char encoded[192];
    if (httpd_query_key_value(query, "s", encoded, sizeof(encoded)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing text");
    }

    for (const char* p = encoded; *p != '\0'; p++) {
        // httpd_query_key_value leaves percent escapes in place, and '+'
        // is the form encoding for a space.
        char ch = *p;
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = {p[1], p[2], '\0'};
            ch = static_cast<char>(strtol(hex, nullptr, 16));
            p += 2;
        }

        uint8_t row = 0;
        uint8_t col = 0;
        bool needs_shift = false;
        if (GetHAL().keyboard.findKeyPosition(ch, row, col, needs_shift)) {
            queue_tap(row, col, false, needs_shift);
        }
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

}  // namespace

/* -------------------------------- Daemon -------------------------------- */

bool start()
{
    if (_server != nullptr) {
        return true;
    }

    if (_key_queue == nullptr) {
        _key_queue = xQueueCreate(kKeyQueueLength, sizeof(QueuedKey_t));
        if (_key_queue == nullptr) {
            mclog::tagError(_tag, "key queue allocation failed");
            return false;
        }
    }

    if (_snapshot_lock == nullptr) {
        _snapshot_lock = xSemaphoreCreateMutex();
        if (_snapshot_lock == nullptr) {
            mclog::tagError(_tag, "snapshot lock allocation failed");
            return false;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;

    const esp_err_t ret = httpd_start(&_server, &config);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "server start failed: {}", esp_err_to_name(ret));
        _server = nullptr;
        return false;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = nullptr},
        {.uri = "/frame", .method = HTTP_GET, .handler = handle_frame, .user_ctx = nullptr},
        {.uri = "/key", .method = HTTP_GET, .handler = handle_key, .user_ctx = nullptr},
        {.uri = "/text", .method = HTTP_GET, .handler = handle_text, .user_ctx = nullptr},
    };
    for (const auto& route : routes) {
        httpd_register_uri_handler(_server, &route);
    }

    // Pages other parts of the firmware asked to have served here.
    for (size_t i = 0; i < _extra_count; i++) {
        httpd_register_uri_handler(_server, &_extra_routes[i]);
    }

    esp_netif_ip_info_t ip = {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        char text[32];
        snprintf(text, sizeof(text), "http://" IPSTR "/", IP2STR(&ip.ip));
        _address = text;
    } else {
        _address.clear();
    }

    mclog::tagInfo(_tag, "listening on {}", _address.empty() ? "?" : _address);
    return true;
}

void stop()
{
    if (_server == nullptr) {
        return;
    }
    httpd_stop(_server);
    _server = nullptr;
    _address.clear();

    release_bands();

    mclog::tagInfo(_tag, "stopped");
}

bool is_running()
{
    return _server != nullptr;
}

const std::string& address()
{
    return _address;
}

void add_route(const httpd_uri_t& route)
{
    if (_extra_count >= kMaxExtraRoutes) {
        mclog::tagError(_tag, "no room for another route");
        return;
    }

    /* Registering the same path twice would give the server two handlers
     * for it, and a job that is stopped and started again would do just
     * that. */
    for (size_t i = 0; i < _extra_count; i++) {
        if (strcmp(_extra_routes[i].uri, route.uri) == 0 && _extra_routes[i].method == route.method) {
            return;
        }
    }

    _extra_routes[_extra_count++] = route;
    if (_server != nullptr) {
        httpd_register_uri_handler(_server, &route);
    }
}

void add_link(const char* path, const char* title)
{
    if (path == nullptr || title == nullptr) {
        return;
    }
    if (_link_count >= kMaxLinks) {
        mclog::tagError(_tag, "no room for another link");
        return;
    }
    for (size_t i = 0; i < _link_count; i++) {
        if (strcmp(_links[i].path, path) == 0) {
            return;
        }
    }
    _links[_link_count++] = {path, title};
}

void update()
{
    const bool connected = GetHAL().isWifiConnected();
    if (connected != _was_connected) {
        _was_connected = connected;
        if (connected) {
            start();
        } else {
            stop();
        }
    }

    if (_server != nullptr) {
        const uint32_t now = GetHAL().millis();
        const bool watched = (now - _last_request_ms) < kIdleReleaseMs;

        /* An application taking or giving up the panel changes how much
         * of the heap the mirror may keep, so the buffer is handed back
         * and taken again at the size that now applies. */
        if (_bands_ready && _tiling != wanted_tiling()) {
            xSemaphoreTake(_snapshot_lock, portMAX_DELAY);
            release_bands();
            xSemaphoreGive(_snapshot_lock);
        }

        if (watched && !_bands_ready) {
            if (reserve_bands()) {
                _alloc_failure_logged = false;
            } else if (!_alloc_failure_logged) {
                _alloc_failure_logged = true;
                mclog::tagError(_tag, "snapshot allocation failed, {} bytes free",
                                esp_get_free_heap_size());
            }
        } else if (!watched && _bands_ready) {
            // Nobody is looking; 64 KB is better off back in the heap,
            // where WiFi and the applications can use it.
            xSemaphoreTake(_snapshot_lock, portMAX_DELAY);
            release_bands();
            xSemaphoreGive(_snapshot_lock);
        }

        const uint32_t interval =
            GetHAL().isFullScreenApp() ? kPanelCaptureIntervalMs : kCaptureIntervalMs;
        if (_bands_ready && (now - _snapshot_ms) >= interval) {
            // Skipped rather than waited on while a frame is going out, so
            // the interface never stalls on the network.
            if (xSemaphoreTake(_snapshot_lock, 0) == pdTRUE) {
                capture_frame();
                _snapshot_ms = now;
                /* On to the next quarter, so four passes cover the panel
                 * and a screen that is not moving fills in completely. */
                if (_tiling) {
                    _tile = (_tile + 1) % kTileCount;
                }
                xSemaphoreGive(_snapshot_lock);
            }
        }
    }

    if (_key_queue == nullptr) {
        return;
    }

    // Bounded so a long typed string cannot monopolise a single loop pass,
    // and so each key gets its own frame for applications that redraw on
    // every event.
    static uint32_t next_key_ms = 0;
    if (GetHAL().millis() < next_key_ms) {
        return;
    }

    QueuedKey_t key;
    if (xQueueReceive(_key_queue, &key, 0) == pdTRUE) {
        Keyboard::KeyEventRaw_t event;
        event.row = key.row;
        event.col = key.col;
        event.state = key.state;
        GetHAL().keyboard.injectKeyEventRaw(event);
        next_key_ms = GetHAL().millis() + kKeyHoldMs;
    }
}

}  // namespace remoted
