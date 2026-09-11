/*
 * SPDX-License-Identifier: MIT
 */
#include "screenshot.h"

#include "remoted.h"

#include <hal.h>
#include <mooncake_log.h>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdlib>
#include <cstring>

namespace screenshot {

namespace {

const std::string _tag = "screenshot";

/* Set by the HTTP task, cleared by it or by the deadline. */
volatile bool _wanted = false;

/* Set by the main loop once it has seen the request. The HTTP task waits
 * for this before the first read: the request alone means nothing, the
 * acknowledgement means the application will not draw. */
volatile bool _frozen = false;

/*
 * How long the loop may stay parked. A client that vanishes mid-capture
 * must not leave the board with a still picture and no way back.
 *
 * Four seconds was not enough and failed in the worst way it could: the
 * deadline fired halfway down the panel, the main loop resumed, the
 * /frame mirror started reading the glass from it, and two tasks on one
 * SPI bus turned the rest of the picture into noise. The read is much
 * faster now, and this is generous on top of that -- but the capture
 * also checks that it still holds the freeze rather than trusting it.
 */
constexpr uint32_t kMaxFreezeMs = 12000;
uint32_t _deadline = 0;

/* Rows per read and per chunk. One row at a time meant 135 SPI
 * transactions and 135 TCP writes for a 240x135 panel, and the writes
 * were what cost the seconds. */
constexpr int kStripRows = 8;

/* How long to wait for the loop to park. Missing this means something is
 * blocking the main task -- an application in a long operation of its
 * own -- and the honest answer is to say so rather than read a panel
 * somebody else is drawing on. */
constexpr int kAcknowledgeMs = 800;

const char kPage[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport "
    "content=\"width=device-width,initial-scale=1\">"
    "<title>Screenshot</title>"
    "<style>"
    "body{background:#15161a;color:#e8e6e1;font:15px/1.5 system-ui,sans-serif;margin:0;"
    "padding:24px;display:flex;flex-direction:column;align-items:center;gap:16px}"
    "h1{font-size:17px;font-weight:600;margin:0;letter-spacing:.01em}"
    "p{margin:0;color:#9b968d;font-size:13px;max-width:34em;text-align:center}"
    "button{font:inherit;font-weight:600;color:#15161a;background:#ff8a5b;border:0;"
    "border-radius:4px;padding:9px 18px;cursor:pointer}"
    "button:hover{background:#ffa176}"
    "button:disabled{background:#4a4640;color:#8d887f;cursor:default}"
    "img{image-rendering:pixelated;width:min(96vw,720px);border:1px solid #34323a;"
    "border-radius:3px;background:#000}"
    "a{color:#ff8a5b}"
    "</style>"
    "<h1>Screenshot</h1>"
    "<p>The application is stopped for as long as the panel takes to read, so a moving "
    "picture comes back whole rather than in pieces from four different moments.</p>"
    "<button id=b>Capture</button>"
    "<img id=i alt=\"\">"
    "<p id=s></p>"
    "<script>"
    "const b=document.getElementById('b'),i=document.getElementById('i'),"
    "s=document.getElementById('s');"
    "b.onclick=()=>{b.disabled=true;s.textContent='reading the panel...';"
    "const t=Date.now();const u='/shot?bmp=1&t='+t;"
    "i.onload=()=>{b.disabled=false;s.innerHTML='taken in '+(Date.now()-t)+' ms &middot; "
    "<a download=\"cardputer.bmp\" href=\"'+u+'\">save</a>';};"
    "i.onerror=()=>{b.disabled=false;s.textContent='the panel could not be read';};"
    "i.src=u;};"
    "</script>";

void put32(uint8_t* at, uint32_t value)
{
    at[0] = (uint8_t)(value & 0xFF);
    at[1] = (uint8_t)((value >> 8) & 0xFF);
    at[2] = (uint8_t)((value >> 16) & 0xFF);
    at[3] = (uint8_t)((value >> 24) & 0xFF);
}

/*
 * A plain 24-bit BMP, which every browser shows and saves without help.
 * Sixteen-bit BMPs would halve the bytes on the wire and need bitfield
 * masks that browsers disagree about; the wire is not the scarce thing
 * here.
 */
esp_err_t send_bitmap(httpd_req_t* req, int width, int height)
{
    const int stride = width * 3;  // 240 * 3 is already a multiple of four
    const uint32_t image_size = (uint32_t)stride * (uint32_t)height;

    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    put32(header + 2, 54 + image_size);
    put32(header + 10, 54);
    put32(header + 14, 40);
    put32(header + 18, (uint32_t)width);
    put32(header + 22, (uint32_t)height);  // positive: rows run bottom to top
    header[26] = 1;                        // planes
    header[28] = 24;                       // bits per pixel
    put32(header + 34, image_size);
    put32(header + 38, 2835);  // 72 dpi, in pixels per metre
    put32(header + 42, 2835);

    uint16_t* strip565 = (uint16_t*)malloc((size_t)width * kStripRows * sizeof(uint16_t));
    uint8_t* strip24   = (uint8_t*)malloc((size_t)stride * kStripRows);
    if (strip565 == nullptr || strip24 == nullptr) {
        free(strip565);
        free(strip24);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no room for a strip");
    }

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t ret = httpd_resp_send_chunk(req, (const char*)header, sizeof(header));

    auto& display = GetHAL().display;

    /* Bottom upwards, because that is the order a BMP stores its rows
     * in. A strip is read top-down, as the panel is addressed, and then
     * emitted in reverse. */
    for (int y = height - 1; y >= 0 && ret == ESP_OK;) {
        /* The freeze is the whole basis of this being coherent, so it is
         * checked rather than assumed. Losing it means the picture from
         * here down is worthless, and a truncated file the client can
         * see is broken beats a whole one quietly full of noise. */
        if (!_frozen) {
            mclog::tagWarn(_tag, "the freeze was lost, abandoning the capture");
            break;
        }

        const int rows = (y + 1 < kStripRows) ? (y + 1) : kStripRows;
        const int top  = y - rows + 1;
        display.readRect(0, top, width, rows, strip565);

        for (int r = 0; r < rows; r++) {
            const uint16_t* in = strip565 + (size_t)(rows - 1 - r) * width;
            uint8_t* out       = strip24 + (size_t)r * stride;
            for (int x = 0; x < width; x++) {
                /* readRect hands back M5GFX's own 16bpp layout, which is
                 * RGB565 with the two bytes the other way round. Read as
                 * it comes, orange arrives as blue -- 0xFD20 turned into
                 * 0x20FD. The mirror on /frame leaves the swapping to
                 * the page; here the file has to be right on its own. */
                const uint16_t raw = in[x];
                const uint16_t v   = (uint16_t)((raw >> 8) | (raw << 8));

                /* Five and six bits stretched back over eight, so white
                 * comes out white rather than a shade under it. */
                out[x * 3 + 0] = (uint8_t)((v & 0x1F) * 255 / 31);          // blue first, as BMP wants
                out[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
                out[x * 3 + 2] = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
            }
        }

        ret = httpd_resp_send_chunk(req, (const char*)strip24, (size_t)stride * rows);
        y -= rows;
    }

    free(strip565);
    free(strip24);

    httpd_resp_send_chunk(req, nullptr, 0);
    return ret;
}

esp_err_t handle_shot(httpd_req_t* req)
{
    char query[64];
    const bool wants_image = (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) &&
                             (httpd_query_key_value(query, "bmp", query, sizeof(query)) == ESP_OK);

    if (!wants_image) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
    }

    const int width  = GetHAL().display.width();
    const int height = GetHAL().display.height();

    _deadline = GetHAL().millis() + kMaxFreezeMs;
    _wanted   = true;

    /* Wait to be told the loop has parked, rather than assuming it. */
    for (int waited = 0; !_frozen && waited < kAcknowledgeMs; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!_frozen) {
        _wanted = false;
        mclog::tagWarn(_tag, "the main loop did not stop");
        return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "the screen is busy");
    }

    const uint32_t started = GetHAL().millis();
    const esp_err_t ret    = send_bitmap(req, width, height);
    const uint32_t took    = GetHAL().millis() - started;

    _wanted = false;
    mclog::tagInfo(_tag, "{}x{} read in {} ms", width, height, took);
    return ret;
}

}  // namespace

void init()
{
    static const httpd_uri_t route = {
        .uri = "/shot", .method = HTTP_GET, .handler = handle_shot, .user_ctx = nullptr};
    remoted::add_route(route);
    remoted::add_link("/shot", "screenshot");
    mclog::tagInfo(_tag, "capture at /shot");
}

void tick()
{
    if (!_wanted) {
        _frozen = false;
        return;
    }

    if (!_frozen) {
        _frozen = true;
        return;
    }

    if ((int32_t)(GetHAL().millis() - _deadline) > 0) {
        mclog::tagWarn(_tag, "capture overran, letting the screen go");
        _wanted = false;
        _frozen = false;
    }
}

bool is_frozen()
{
    return _frozen;
}

}  // namespace screenshot
