/*
 * SPDX-License-Identifier: MIT
 */
#include "remote_launch.h"

#include <hal/utils/remoted/remoted.h>
#include <mooncake_log.h>

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>

namespace remote_launch {

namespace {

const std::string _tag = "remote_launch";

SemaphoreHandle_t _lock = nullptr;

Request _pending;
std::vector<std::string> _names;
std::string _startup;
std::string _running;

/* Holding the lock is the rule for everything below, including reads:
 * the listing is built on the HTTP task while the launcher may be
 * setting what is running. */
struct Guard {
    bool held = false;
    Guard()
    {
        held = (_lock != nullptr) && (xSemaphoreTake(_lock, pdMS_TO_TICKS(200)) == pdTRUE);
    }
    ~Guard()
    {
        if (held) {
            xSemaphoreGive(_lock);
        }
    }
};

/* Percent escapes are left in place by httpd_query_key_value, and '+' is
 * the form encoding for a space. Application names here have neither,
 * but a name typed by hand might. */
std::string decode(const char* text)
{
    std::string out;
    for (const char* p = text; *p != '\0'; p++) {
        char ch = *p;
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%' && p[1] != '\0' && p[2] != '\0') {
            char hex[3] = {p[1], p[2], '\0'};
            ch = static_cast<char>(strtol(hex, nullptr, 16));
            p += 2;
        }
        out.push_back(ch);
    }
    return out;
}

/* The listing, which is also what a browser gets for a bare /app: one
 * application per line, marked so the state is readable without a
 * second request. */
std::string listing()
{
    Guard guard;
    if (!guard.held) {
        return "busy\n";
    }

    std::string out;
    for (const auto& name : _names) {
        out += (name == _running) ? "* " : "  ";
        out += name;
        if (name == _startup) {
            out += "  (startup)";
        }
        out.push_back('\n');
    }
    if (_running.empty()) {
        out += "\nlauncher on screen\n";
    }
    out += "\nGET /app?open=NAME   open one\n";
    out += "GET /app?close=1     back to the launcher\n";
    return out;
}

bool queue(Action action, const std::string& name)
{
    Guard guard;
    if (!guard.held) {
        return false;
    }
    _pending.action = action;
    _pending.name   = name;
    return true;
}

esp_err_t handle_app(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        const std::string body = listing();
        return httpd_resp_send(req, body.c_str(), body.size());
    }

    char value[64];
    if (httpd_query_key_value(query, "open", value, sizeof(value)) == ESP_OK) {
        const std::string name = decode(value);
        if (!queue(Action::Open, name)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        }
        mclog::tagInfo(_tag, "open requested: {}", name);
        return httpd_resp_send(req, "opening\n", HTTPD_RESP_USE_STRLEN);
    }

    if (httpd_query_key_value(query, "close", value, sizeof(value)) == ESP_OK) {
        if (!queue(Action::Close, "")) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        }
        mclog::tagInfo(_tag, "close requested");
        return httpd_resp_send(req, "closing\n", HTTPD_RESP_USE_STRLEN);
    }

    const std::string body = listing();
    return httpd_resp_send(req, body.c_str(), body.size());
}

}  // namespace

void init()
{
    if (_lock == nullptr) {
        _lock = xSemaphoreCreateMutex();
        if (_lock == nullptr) {
            mclog::tagError(_tag, "mutex allocation failed");
            return;
        }
    }

    static const httpd_uri_t route = {
        .uri = "/app", .method = HTTP_GET, .handler = handle_app, .user_ctx = nullptr};
    remoted::add_route(route);
    remoted::add_link("/app", "applications");
    mclog::tagInfo(_tag, "applications at /app");
}

void publish(const std::vector<std::string>& names, const std::string& startup)
{
    Guard guard;
    if (!guard.held) {
        return;
    }
    _names   = names;
    _startup = startup;
}

void set_running(const std::string& name)
{
    Guard guard;
    if (!guard.held) {
        return;
    }
    _running = name;
}

Request take()
{
    Request out;
    Guard guard;
    if (!guard.held) {
        return out;
    }
    out             = _pending;
    _pending.action = Action::None;
    _pending.name.clear();
    return out;
}

}  // namespace remote_launch
