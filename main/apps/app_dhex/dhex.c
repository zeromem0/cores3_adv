#include "dhex.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "dhex_board.h"
#include "dhex_gfx.h"
#include "dhex_keys.h"
#include "dhex_log.h"
#include "dhex_uart.h"

/* Same physical UART the aqm app parses PM1.0/2.5/10 frames from --
 * dhex just shows the raw bytes. Port/pins/baud/framing all used to be
 * fixed at compile time (DHEX_BOARD_PM_UART_*, 9600 8E1); now
 * they're runtime-configurable via launch args and persisted in NVS
 * (see dhex_uart_config_t below), with the board macros only used as
 * the very first, never-configured-yet default. */
#define DHEX_NVS_NAMESPACE "dhex"
#define DHEX_UART_DRIVER_RX_BYTES 1024U
#define DHEX_POLL_CHUNK 64U

/* Exactly 16 bytes, no more, no less: 2 rows of 8 in the hex/ascii
 * ("wireshark-style") view up top, 16 columns in the big-ascii strip
 * at the bottom. */
#define DHEX_BUFFER_SIZE 16U
#define DHEX_ROW_BYTES 8U

/* Wireshark section stays on a MONO font -- it needs consistent
 * character width for the hex/address column grid to line up, and
 * the API has no "bold mono" variant. The single-char-per-column big
 * ascii row isn't grid-sensitive the same way, so it gets the bolder,
 * heavier BOLD font instead of MONO for better legibility. */
#define DHEX_SMALL_FONT DHEX_GFX_FONT_MONO_14
#define DHEX_TOP_MARGIN 4
#define DHEX_ROW_HEIGHT 18
#define DHEX_ADDR_GAP 4
#define DHEX_HEX_ASCII_GAP 5
#define DHEX_SECTION_GAP 12
#define DHEX_BOTTOM_LINE_HEIGHT 14

/* Header: "dhex" title on the left, serial params + RX/TX pins on the
 * right (two small lines), a divider line under the whole thing. */
#define DHEX_TITLE_FONT DHEX_GFX_FONT_BOLD_18
#define DHEX_HEADER_MARGIN 4
#define DHEX_HEADER_TITLE_BASELINE 22
#define DHEX_HEADER_LINE1_BASELINE 14
#define DHEX_HEADER_LINE2_BASELINE 30
#define DHEX_HEADER_HEIGHT 34

static const char *TAG = "dhex";

/* Arduino-style "8E1" framing spec: data bits (5-8), parity (N/E/O),
 * stop bits (1-2) -- the exact notation the reference project this was
 * ported from already used (SERIAL_8E1), so launch args mirror
 * Serial2.begin(baud, config, rx, tx)'s argument order instead of
 * inventing new flag names. */
typedef struct {
    int port;
    int tx_pin;
    int rx_pin;
    uint32_t baud;
    uint8_t data_bits;
    char parity;
    uint8_t stop_bits;
} dhex_uart_config_t;

typedef struct {
    bool uart_ready;
    dhex_uart_config_t active_config;
    uint8_t buffer[DHEX_BUFFER_SIZE];
    size_t total_received;
    size_t last_rendered_total;
    /* Casio-editor-style settings screen: LEFT/RIGHT move between
     * fields, UP/DOWN adjust the selected field's value (both always
     * available at once, same convention as irriga's zone editor --
     * the rotary encoder's own click toggles which pair its rotation
     * currently sends). ENTER applies the draft to the UART and saves
     * it; ESCAPE discards it. Nothing is touched until then. */
    bool config_mode;
    uint8_t config_field;
    dhex_uart_config_t draft_config;
    /* Space toggles a reading-at-a-distance view: the last four bytes
     * only, side by side, as large as the panel allows. */
    bool quad_mode;
} dhex_state_t;

/* PSRAM, not internal SRAM -- same convention dhex_lua.c and
 * dhex_python.c use for their app state, and one this file didn't
 * follow when the autobaud feature was added, costing LCD-5's already
 * razor-thin internal SRAM margin ~340 bytes for no reason. */
static EXT_RAM_BSS_ATTR dhex_state_t dhex_state;

/* Cycling through a fixed list of standard rates is far more usable
 * than nudging a raw integer one step at a time. 0 is the "Autobaud"
 * sentinel: applying the config with this selected runs edge-timing
 * detection on the RX pin instead of using a fixed rate. */
/* Space between the longest field label and the value column. The widest
 * value ("Autobaud") still clears the right edge at this gap. */
#define DHEX_CONFIG_LABEL_GAP 20

static const uint32_t dhex_common_bauds[] = {
    0, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400,
    57600, 115200, 230400, 460800, 921600,
};
#define DHEX_COMMON_BAUD_COUNT (sizeof(dhex_common_bauds) / sizeof(dhex_common_bauds[0]))
#define DHEX_CONFIG_FIELD_COUNT 4U

/* Autobaud: passive edge-timing detection, same technique as
 * ESP32-Bit-Pirate's detectBaudByEdge -- listen for transitions on
 * the RX line (no probe byte sent, works only if the far end is
 * already talking), measure inter-edge intervals in short windows,
 * score each candidate rate by how well the intervals fit as integer
 * bit-time multiples, and require the winner to repeat across a
 * couple of windows before trusting it. */
#define DHEX_AUTOBAUD_MAX_INTERVALS 64U
#define DHEX_AUTOBAUD_WINDOW_MS 300U
#define DHEX_AUTOBAUD_TOTAL_MS 3000U
#define DHEX_AUTOBAUD_MIN_EDGES 30U

typedef struct {
    volatile uint32_t last_edge_us;
    volatile uint32_t intervals[DHEX_AUTOBAUD_MAX_INTERVALS];
    volatile uint8_t interval_count;
    volatile uint32_t edge_count;
} dhex_autobaud_isr_state_t;

/* PSRAM: only touched while autobaud is actively running (a rare,
 * user-triggered, few-seconds-long operation), so the extra access
 * latency on each GPIO edge is a good trade for not permanently
 * costing internal SRAM the other 99.9% of the time. Safe from the
 * ISR: gpio_install_isr_service() below is called without
 * ESP_INTR_FLAG_IRAM, so the handler always runs with the flash
 * cache enabled and PSRAM reachable. */
static EXT_RAM_BSS_ATTR dhex_autobaud_isr_state_t dhex_autobaud_isr;
static bool dhex_autobaud_isr_service_installed;

static void IRAM_ATTR dhex_autobaud_isr_handler(void *arg)
{
    (void)arg;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    if (dhex_autobaud_isr.last_edge_us != 0 && dhex_autobaud_isr.interval_count < DHEX_AUTOBAUD_MAX_INTERVALS) {
        dhex_autobaud_isr.intervals[dhex_autobaud_isr.interval_count++] = now - dhex_autobaud_isr.last_edge_us;
    }
    dhex_autobaud_isr.last_edge_us = now;
    dhex_autobaud_isr.edge_count++;
}

typedef struct {
    uint32_t edges;
    uint32_t approx_baud;
} dhex_baud_measurement_t;

static dhex_baud_measurement_t dhex_measure_baud_once(gpio_num_t pin, uint32_t window_ms)
{
    dhex_autobaud_isr.last_edge_us = 0;
    dhex_autobaud_isr.interval_count = 0;
    dhex_autobaud_isr.edge_count = 0;

    if (!dhex_autobaud_isr_service_installed) {
        gpio_install_isr_service(0);
        dhex_autobaud_isr_service_installed = true;
    }

    const gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << (unsigned)pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_config);
    gpio_isr_handler_add(pin, dhex_autobaud_isr_handler, NULL);

    vTaskDelay(pdMS_TO_TICKS(window_ms));

    gpio_isr_handler_remove(pin);

    dhex_baud_measurement_t result = {0};
    result.edges = dhex_autobaud_isr.edge_count;

    const uint8_t raw_count = dhex_autobaud_isr.interval_count;
    if (raw_count <= 8) {
        return result;
    }

    uint32_t intervals[DHEX_AUTOBAUD_MAX_INTERVALS];
    memcpy(intervals, (const void *)dhex_autobaud_isr.intervals, raw_count * sizeof(uint32_t));

    /* Small insertion sort -- at most 64 entries. */
    for (uint8_t i = 1; i < raw_count; i++) {
        const uint32_t key = intervals[i];
        int j = (int)i - 1;
        while (j >= 0 && intervals[j] > key) {
            intervals[j + 1] = intervals[j];
            j--;
        }
        intervals[j + 1] = key;
    }

    uint8_t count = 0;
    for (uint8_t i = 0; i < raw_count; i++) {
        if (intervals[i] >= 2U && intervals[i] <= 200000U) {
            intervals[count++] = intervals[i];
        }
    }
    if (count <= 8) {
        return result;
    }

    /*
     * The bit period is the SHORTEST real gap between two edges: a UART
     * line physically cannot transition faster than once per bit, so
     * the minimum inter-edge interval is exactly one bit time. Multi-bit
     * runs only ever produce longer (integer-multiple) intervals, never
     * shorter -- which is precisely why an interval-scoring approach
     * gets fooled into a sub-harmonic (it can "explain" a 9600 stream as
     * 4800 using the 2-bit intervals and ignoring the 1-bit ones). Using
     * the minimum sidesteps that: one genuine single-bit gap is enough.
     *
     * intervals[] is sorted ascending and already glitch-filtered
     * (>= 2us). Take a low percentile rather than the absolute minimum
     * so a lone sub-bit glitch that slipped the floor can't inflate the
     * rate; a handful of real single-bit gaps always cluster here. For
     * the ideal calibration signal (a run of 0x55 'U', pure alternating
     * bits) every interval equals one bit, so this lands dead-on.
     */
    const size_t bit_index = (size_t)count / 20U; /* ~5th percentile */
    const uint32_t bit_us = intervals[bit_index];
    if (bit_us > 0U) {
        const uint32_t guess = 1000000UL / bit_us;
        if (guess >= 1000U && guess <= 400000U) {
            result.approx_baud = guess;
        }
    }

    return result;
}

static uint32_t dhex_snap_to_standard_baud(uint32_t approx)
{
    uint32_t snapped = 0;
    uint32_t best_diff = UINT32_MAX;
    for (size_t i = 0; i < DHEX_COMMON_BAUD_COUNT; i++) {
        const uint32_t candidate = dhex_common_bauds[i];
        if (candidate == 0) {
            continue;
        }
        const uint32_t diff = approx > candidate ? approx - candidate : candidate - approx;
        if (diff < best_diff) {
            best_diff = diff;
            snapped = candidate;
        }
    }
    return snapped;
}

/* Returns a detected standard baud rate, or 0 if nothing converged
 * within DHEX_AUTOBAUD_TOTAL_MS (no traffic, or too irregular to
 * pin down). Blocking -- vTaskDelay inside each measurement window
 * yields normally, but the calling app is unresponsive for up to
 * DHEX_AUTOBAUD_TOTAL_MS, which is why the caller draws a "detecting"
 * screen first. */
static uint32_t dhex_detect_baud(gpio_num_t pin)
{
    uint32_t vote_baud[DHEX_COMMON_BAUD_COUNT] = {0};
    uint32_t vote_count[DHEX_COMMON_BAUD_COUNT] = {0};
    size_t vote_entries = 0;
    uint32_t best_baud = 0;
    uint32_t best_votes = 0;
    uint32_t consecutive_baud = 0;
    uint32_t consecutive_count = 0;
    const uint32_t burst_threshold = (DHEX_AUTOBAUD_MIN_EDGES * 6U) > 24U ?
        (DHEX_AUTOBAUD_MIN_EDGES * 6U) : 24U;

    const int64_t start_us = esp_timer_get_time();
    while ((esp_timer_get_time() - start_us) < ((int64_t)DHEX_AUTOBAUD_TOTAL_MS * 1000)) {
        const dhex_baud_measurement_t m = dhex_measure_baud_once(pin, DHEX_AUTOBAUD_WINDOW_MS);
        if (m.edges < DHEX_AUTOBAUD_MIN_EDGES || m.approx_baud == 0) {
            consecutive_baud = 0;
            consecutive_count = 0;
            continue;
        }

        const uint32_t snapped = dhex_snap_to_standard_baud(m.approx_baud);
        const uint32_t diff = m.approx_baud > snapped ? m.approx_baud - snapped : snapped - m.approx_baud;
        if (m.edges >= burst_threshold && diff == 0) {
            return snapped;
        }

        size_t idx = vote_entries;
        for (size_t i = 0; i < vote_entries; i++) {
            if (vote_baud[i] == snapped) {
                idx = i;
                break;
            }
        }
        if (idx == vote_entries && vote_entries < DHEX_COMMON_BAUD_COUNT) {
            vote_baud[vote_entries] = snapped;
            vote_count[vote_entries] = 0;
            vote_entries++;
        }
        if (idx < vote_entries) {
            vote_count[idx]++;
            if (vote_count[idx] > best_votes) {
                best_votes = vote_count[idx];
                best_baud = snapped;
            }
        }

        if (snapped == consecutive_baud) {
            consecutive_count++;
        } else {
            consecutive_baud = snapped;
            consecutive_count = 1;
        }

        if (consecutive_count >= 2U) {
            return consecutive_baud;
        }
        if (best_votes >= 3U) {
            return best_baud;
        }
    }

    return 0;
}

static void dhex_config_defaults(dhex_uart_config_t *cfg)
{
    cfg->port = (int)DHEX_BOARD_PM_UART_PORT;
    cfg->tx_pin = (int)DHEX_BOARD_PIN_PM_UART_TX;
    cfg->rx_pin = (int)DHEX_BOARD_PIN_PM_UART_RX;
    cfg->baud = 9600U;
    cfg->data_bits = 8U;
    cfg->parity = 'E';
    cfg->stop_bits = 1U;
}

static void dhex_config_load(dhex_uart_config_t *cfg)
{
    dhex_config_defaults(cfg);

    nvs_handle_t nvs;
    if (nvs_open(DHEX_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    int32_t pin_value = 0;
    if (nvs_get_i32(nvs, "port", &pin_value) == ESP_OK) {
        cfg->port = (int)pin_value;
    }
    if (nvs_get_i32(nvs, "tx", &pin_value) == ESP_OK) {
        cfg->tx_pin = (int)pin_value;
    }
    if (nvs_get_i32(nvs, "rx", &pin_value) == ESP_OK) {
        cfg->rx_pin = (int)pin_value;
    }
    uint32_t baud_value = 0;
    if (nvs_get_u32(nvs, "baud", &baud_value) == ESP_OK) {
        cfg->baud = baud_value;
    }
    uint8_t byte_value = 0;
    if (nvs_get_u8(nvs, "bits", &byte_value) == ESP_OK) {
        cfg->data_bits = byte_value;
    }
    if (nvs_get_u8(nvs, "parity", &byte_value) == ESP_OK) {
        cfg->parity = (char)byte_value;
    }
    if (nvs_get_u8(nvs, "stop", &byte_value) == ESP_OK) {
        cfg->stop_bits = byte_value;
    }

    nvs_close(nvs);
}

static esp_err_t dhex_config_save(const dhex_uart_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(DHEX_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_i32(nvs, "port", cfg->port);
    if (ret == ESP_OK) {
        ret = nvs_set_i32(nvs, "tx", cfg->tx_pin);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_i32(nvs, "rx", cfg->rx_pin);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u32(nvs, "baud", cfg->baud);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "bits", cfg->data_bits);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "parity", (uint8_t)cfg->parity);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "stop", cfg->stop_bits);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static size_t dhex_baud_index(uint32_t baud)
{
    for (size_t i = 0; i < DHEX_COMMON_BAUD_COUNT; i++) {
        if (dhex_common_bauds[i] == baud) {
            return i;
        }
    }

    /* A non-standard rate (set via a launch-arg run, say) -- land on
     * its nearest neighbor so the first turn moves somewhere sensible. */
    size_t nearest = 0;
    uint32_t best_diff = UINT32_MAX;
    for (size_t i = 0; i < DHEX_COMMON_BAUD_COUNT; i++) {
        const uint32_t diff = dhex_common_bauds[i] > baud ?
            dhex_common_bauds[i] - baud : baud - dhex_common_bauds[i];
        if (diff < best_diff) {
            best_diff = diff;
            nearest = i;
        }
    }
    return nearest;
}

/* delta is always +1 or -1 (one UP/DOWN key per encoder detent). */
static void dhex_config_adjust(dhex_uart_config_t *cfg, uint8_t field, int delta)
{
    switch (field) {
    case 0: {
        const size_t count = DHEX_COMMON_BAUD_COUNT;
        const size_t idx = (dhex_baud_index(cfg->baud) + (size_t)((int)count + delta)) % count;
        cfg->baud = dhex_common_bauds[idx];
        break;
    }
    case 1: {
        int bits = (int)cfg->data_bits + delta;
        if (bits < 5) {
            bits = 8;
        } else if (bits > 8) {
            bits = 5;
        }
        cfg->data_bits = (uint8_t)bits;
        break;
    }
    case 2: {
        static const char parities[] = {'N', 'E', 'O'};
        const size_t count = sizeof(parities);
        size_t idx = 0;
        for (size_t i = 0; i < count; i++) {
            if (parities[i] == cfg->parity) {
                idx = i;
                break;
            }
        }
        idx = (idx + (size_t)((int)count + delta)) % count;
        cfg->parity = parities[idx];
        break;
    }
    case 3:
        cfg->stop_bits = cfg->stop_bits >= 2 ? 1U : 2U;
        break;
    default:
        break;
    }
}

static bool dhex_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool dhex_parse_framing(const char *text, uint8_t *data_bits, char *parity, uint8_t *stop_bits)
{
    if (text == NULL || strlen(text) != 3) {
        return false;
    }
    if (text[0] < '5' || text[0] > '8') {
        return false;
    }
    const char p = (char)toupper((unsigned char)text[1]);
    if (p != 'N' && p != 'E' && p != 'O') {
        return false;
    }
    if (text[2] != '1' && text[2] != '2') {
        return false;
    }

    *data_bits = (uint8_t)(text[0] - '0');
    *parity = p;
    *stop_bits = (uint8_t)(text[2] - '0');
    return true;
}

/* dhex                              -- use the saved (or default) config
 * dhex <baud> <framing> <rx> <tx> [port] -- e.g. "dhex 9600 8E1 13 14",
 * mirroring Serial2.begin(baud, config, rxPin, txPin)'s argument order.
 * Applying new args always saves them, so a bare "dhex" next time
 * reuses whatever was last configured. */
static bool dhex_parse_args(dhex_context_t *ctx, dhex_uart_config_t *cfg)
{
    const int argc = dhex_context_argc(ctx);
    if (argc == 1) {
        dhex_config_load(cfg);
        return true;
    }

    if (argc != 5 && argc != 6) {
        return false;
    }

    dhex_uart_config_t parsed;
    dhex_config_defaults(&parsed);

    uint32_t baud = 0;
    if (!dhex_parse_u32(dhex_context_argv(ctx, 1),
                        DHEX_UART_MIN_BAUD_RATE,
                        DHEX_UART_MAX_BAUD_RATE,
                        &baud)) {
        return false;
    }
    parsed.baud = baud;

    if (!dhex_parse_framing(dhex_context_argv(ctx, 2),
                            &parsed.data_bits,
                            &parsed.parity,
                            &parsed.stop_bits)) {
        return false;
    }

    uint32_t rx_pin = 0;
    uint32_t tx_pin = 0;
    if (!dhex_parse_u32(dhex_context_argv(ctx, 3), 0, GPIO_NUM_MAX - 1, &rx_pin) ||
        !dhex_parse_u32(dhex_context_argv(ctx, 4), 0, GPIO_NUM_MAX - 1, &tx_pin)) {
        return false;
    }
    parsed.rx_pin = (int)rx_pin;
    parsed.tx_pin = (int)tx_pin;

    if (argc == 6) {
        uint32_t port = 0;
        if (!dhex_parse_u32(dhex_context_argv(ctx, 5), 0, SOC_UART_NUM - 1, &port)) {
            return false;
        }
        parsed.port = (int)port;
    }

    if (dhex_config_save(&parsed) != ESP_OK) {
        DHEX_LOGW(TAG, "failed to save config to NVS, using it for this run only");
    }
    *cfg = parsed;
    return true;
}

/* Placeholder content so the hex/ascii view has something to show
 * before any real bytes arrive (or right after a reconfigure clears
 * the ring buffer), instead of a screen full of "--". */
static void dhex_seed_buffer(void)
{
    static const char seed[] = " Apreciometru \r\n";
    const size_t seed_len = sizeof(seed) - 1U; /* exclude the NUL */

    memset(dhex_state.buffer, 0, sizeof(dhex_state.buffer));
    for (size_t i = 0; i < seed_len && i < DHEX_BUFFER_SIZE; i++) {
        dhex_state.buffer[i] = (uint8_t)seed[i];
    }
    dhex_state.total_received = seed_len < DHEX_BUFFER_SIZE ? seed_len : DHEX_BUFFER_SIZE;
    dhex_state.last_rendered_total = 0;
}

static uart_word_length_t dhex_data_bits_enum(uint8_t bits)
{
    switch (bits) {
        case 5: return UART_DATA_5_BITS;
        case 6: return UART_DATA_6_BITS;
        case 7: return UART_DATA_7_BITS;
        default: return UART_DATA_8_BITS;
    }
}

static uart_parity_t dhex_parity_enum(char parity)
{
    switch (parity) {
        case 'E': return UART_PARITY_EVEN;
        case 'O': return UART_PARITY_ODD;
        default: return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t dhex_stop_bits_enum(uint8_t bits)
{
    return bits >= 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static esp_err_t dhex_uart_start(const dhex_uart_config_t *cfg)
{
    const uart_port_t port = (uart_port_t)cfg->port;

    esp_err_t ret = uart_driver_install(port, DHEX_UART_DRIVER_RX_BYTES, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    const uart_config_t config = {
        .baud_rate = (int)cfg->baud,
        .data_bits = dhex_data_bits_enum(cfg->data_bits),
        .parity = dhex_parity_enum(cfg->parity),
        .stop_bits = dhex_stop_bits_enum(cfg->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ret = uart_param_config(port, &config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(port,
                           (gpio_num_t)cfg->tx_pin,
                           (gpio_num_t)cfg->rx_pin,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (ret != ESP_OK) {
        uart_driver_delete(port);
        return ret;
    }

    return ESP_OK;
}

static void dhex_uart_stop(void)
{
    if (dhex_state.uart_ready) {
        uart_driver_delete((uart_port_t)dhex_state.active_config.port);
    }
}

static void dhex_poll_uart(void)
{
    if (!dhex_state.uart_ready) {
        return;
    }

    uint8_t chunk[DHEX_POLL_CHUNK];
    int len = uart_read_bytes((uart_port_t)dhex_state.active_config.port, chunk, sizeof(chunk), 0);
    for (int i = 0; i < len; i++) {
        dhex_state.buffer[dhex_state.total_received % DHEX_BUFFER_SIZE] = chunk[i];
        dhex_state.total_received++;
    }
}

/* Oldest-to-newest logical index -> physical ring buffer slot. Index 0
 * is the oldest byte currently held (or the first not-yet-received
 * slot, if fewer than 16 bytes have arrived so far). */
static size_t dhex_ring_index(size_t logical_index)
{
    const size_t total = dhex_state.total_received;
    const size_t base = total >= DHEX_BUFFER_SIZE ? total : DHEX_BUFFER_SIZE;
    return (base - DHEX_BUFFER_SIZE + logical_index) % DHEX_BUFFER_SIZE;
}

static bool dhex_slot_filled(size_t logical_index)
{
    const size_t total = dhex_state.total_received;
    if (total >= DHEX_BUFFER_SIZE) {
        return true;
    }
    return logical_index < total;
}

/* The application canvas on a Cardputer ADV is 204x109, far short of what
 * the full layout needs. Below this size the column-index row, the ascii
 * column beside the hex dump and the ruler ticks are all dropped, and the
 * bottom row switches to a font narrow enough for 16 columns to fit. */
static bool dhex_layout_compact(dhex_gfx_t *gfx)
{
    return dhex_gfx_width(gfx) < 240U || dhex_gfx_height(gfx) < 140U;
}

static void dhex_draw_wireshark_row(dhex_gfx_t *gfx, size_t row, int y, int hex_x, int ascii_x, int hex_col_w, int ascii_col_w)
{
    char addr[8];
    const size_t base_offset = dhex_state.total_received >= DHEX_BUFFER_SIZE ?
        dhex_state.total_received - DHEX_BUFFER_SIZE :
        0;
    snprintf(addr, sizeof(addr), "%04X:", (unsigned)((base_offset + (row * DHEX_ROW_BYTES)) & 0xFFFFU));
    dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
    dhex_gfx_text(gfx, 0, y, addr);

    for (size_t col = 0; col < DHEX_ROW_BYTES; col++) {
        const size_t logical = (row * DHEX_ROW_BYTES) + col;
        const int x = hex_x + (int)(col * (size_t)hex_col_w);
        if (!dhex_slot_filled(logical)) {
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_DARK);
            dhex_gfx_text(gfx, x, y, "--");
            continue;
        }

        const uint8_t b = dhex_state.buffer[dhex_ring_index(logical)];
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", b);
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
        dhex_gfx_text(gfx, x, y, hex);
    }

    if (dhex_layout_compact(gfx)) {
        return;
    }

    for (size_t col = 0; col < DHEX_ROW_BYTES; col++) {
        const size_t logical = (row * DHEX_ROW_BYTES) + col;
        const int x = ascii_x + (int)(col * (size_t)ascii_col_w);
        if (!dhex_slot_filled(logical)) {
            continue;
        }

        const uint8_t b = dhex_state.buffer[dhex_ring_index(logical)];
        char sym[2] = {0, 0};
        if (b == 0x0D || b == 0x0A) {
            /* Inverse video so a control-character stand-in can never
             * be mistaken for a literal 'C' or 'L' byte. Both letters are
             * stacked here, as in the bottom row: a row is tall enough for
             * two lines of the small font, and one letter alone still read
             * as an ordinary character.
             *
             * ascii_col_w - 2: same visible gap as the bottom row, so
             * back-to-back CR/LF don't fuse into one solid block. */
            const int block_w = ascii_col_w - 2;
            const int line_h = DHEX_ROW_HEIGHT / 2;

            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
            dhex_gfx_fill_rect(gfx, x, y - DHEX_ROW_HEIGHT + 1, block_w, DHEX_ROW_HEIGHT);
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_BLACK);

            const char *top = b == 0x0D ? "C" : "L";
            const char *bottom = b == 0x0D ? "R" : "F";
            const int letter_w = (int)dhex_gfx_text_width(gfx, top);
            const int letter_x = x + ((block_w - letter_w) / 2);
            dhex_gfx_text(gfx, letter_x, y - line_h, top);
            dhex_gfx_text(gfx, letter_x, y, bottom);
            continue;
        } else if (b >= 0x20 && b <= 0x7E) {
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
            sym[0] = (char)b;
        } else {
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
            sym[0] = '.';
        }
        dhex_gfx_text(gfx, x, y, sym);
    }
}

static void dhex_draw_wireshark_section(dhex_gfx_t *gfx, int top_y, int hex_x, int ascii_x, int hex_col_w, int ascii_col_w)
{
    dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);

    /* Compact layout drops the column-index row and lifts the data rows
     * into the space it occupied. */
    int y = top_y;
    if (!dhex_layout_compact(gfx)) {
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
        dhex_gfx_text(gfx, 0, top_y, "Addr:");
        for (size_t col = 0; col < DHEX_ROW_BYTES; col++) {
            char header[3];
            snprintf(header, sizeof(header), "%02X", (unsigned)col);
            dhex_gfx_text(gfx, hex_x + (int)(col * (size_t)hex_col_w), top_y, header);
        }
        y = top_y + DHEX_ROW_HEIGHT;
    }

    for (size_t row = 0; row < (DHEX_BUFFER_SIZE / DHEX_ROW_BYTES); row++) {
        dhex_draw_wireshark_row(gfx, row, y, hex_x, ascii_x, hex_col_w, ascii_col_w);
        y += DHEX_ROW_HEIGHT;
    }
}

/* Gap between the tick and the top of the font's glyphs, and the
 * tick's own size. */
#define DHEX_BIG_TICK_GAP 5
#define DHEX_BIG_TICK_WIDTH 1
#define DHEX_BIG_TICK_HEIGHT 2

/* Same rule irriga's clock display uses: the bigger ProFont on tall
 * panels, a half-size one everywhere else. The PROFONT_* name is its
 * nominal pixel size (the 2x-doubled and 1x families share this
 * numbering), which doubles as a good-enough glyph height estimate --
 * dhex_gfx doesn't expose font ascent/height directly. */
static int dhex_font_big_size(dhex_gfx_t *gfx)
{
    if (dhex_layout_compact(gfx)) {
        /* 16 columns across a 204 px canvas leaves under 13 px each, so
         * anything larger overlaps its neighbour on the right. */
        return 17;
    }
    return dhex_gfx_height(gfx) >= 480 ? 58 : 29;
}

static dhex_gfx_font_t dhex_font_big(dhex_gfx_t *gfx)
{
    const int size = dhex_font_big_size(gfx);
    if (size >= 58) {
        return DHEX_GFX_FONT_PROFONT_58;
    }
    return size >= 29 ? DHEX_GFX_FONT_PROFONT_29 : DHEX_GFX_FONT_PROFONT_17;
}

static void dhex_draw_big_ascii_section(dhex_gfx_t *gfx, int y, int col_w)
{
    /* A tick above every column position -- a lightweight ruler so
     * columns are easy to count at a glance, independent of content.
     * BLACK, not DARK: DARK dithers on a 1bpp panel, which at just 1-2
     * pixels can drop one of the two stacked pixels entirely. */
    if (!dhex_layout_compact(gfx)) {
        const int tick_y = y - dhex_font_big_size(gfx) - DHEX_BIG_TICK_GAP;
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
        for (size_t i = 0; i < DHEX_BUFFER_SIZE; i++) {
            const int x = (int)(i * (size_t)col_w);
            dhex_gfx_fill_rect(gfx, x, tick_y, DHEX_BIG_TICK_WIDTH, DHEX_BIG_TICK_HEIGHT);
        }
    }

    for (size_t i = 0; i < DHEX_BUFFER_SIZE; i++) {
        const int x = (int)(i * (size_t)col_w);
        if (!dhex_slot_filled(i)) {
            continue;
        }

        const uint8_t b = dhex_state.buffer[dhex_ring_index(i)];
        if (b == 0x0D || b == 0x0A) {
            /* Inverse video, same reasoning as the wireshark section:
             * these are stand-ins for control characters, never real
             * letters, and must read as visually distinct at a glance. */
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
            /* col_w - 2: leaves a visible gap so back-to-back CR/LF
             * (as in a plain "\r\n") don't fuse into one solid block. */
            dhex_gfx_fill_rect(gfx,
                                   x,
                                   y - (2 * DHEX_BOTTOM_LINE_HEIGHT) + 4,
                                   col_w - 2,
                                   2 * DHEX_BOTTOM_LINE_HEIGHT);
            dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_BLACK);
            const char *top = b == 0x0D ? "C" : "L";
            const char *bottom = b == 0x0D ? "R" : "F";
            /* Centred inside the inverse block rather than flush against
             * its left edge, which read as if the letters had slipped. */
            const int letter_w = (int)dhex_gfx_text_width(gfx, top);
            const int letter_x = x + (((col_w - 2) - letter_w) / 2);
            dhex_gfx_text(gfx, letter_x, y - DHEX_BOTTOM_LINE_HEIGHT, top);
            dhex_gfx_text(gfx, letter_x, y, bottom);
        } else if (b >= 0x20 && b <= 0x7E) {
            dhex_gfx_set_font(gfx, dhex_font_big(gfx));
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
            char sym[2] = {(char)b, 0};
            dhex_gfx_text(gfx, x, y, sym);
        } else {
            dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
            dhex_gfx_text(gfx, x, y, ".");
        }
    }
}

/* Bytes shown by the quad view, side by side, each a quarter of the
 * panel's width and its full height. */
#define DHEX_QUAD_COUNT 4U

/* Baseline of the hex value printed above each character. Deep enough for
 * the title font, which is what it is drawn in. */
#define DHEX_QUAD_HEX_BASELINE 18

/* Nominal size the cell baseline is derived from. Fixed rather than taken
 * from whichever font a cell ends up using, so a two-letter CR or LF sits
 * on the same line as the full-size characters next to it instead of
 * floating above them. */
#define DHEX_QUAD_BASELINE_FONT 58

static void dhex_draw_quad_screen(dhex_gfx_t *gfx)
{
    const int screen_width = (int)dhex_gfx_width(gfx);
    const int screen_height = (int)dhex_gfx_height(gfx);
    const int cell_w = screen_width / (int)DHEX_QUAD_COUNT;
    const int cell_h = screen_height;

    dhex_gfx_clear(gfx, DHEX_GFX_COLOR_BLACK);

    /* Faint cell dividers, so an empty cell still reads as a cell. */
    dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_DARK);
    for (size_t i = 1; i < DHEX_QUAD_COUNT; i++) {
        const int x = (int)i * cell_w;
        dhex_gfx_line(gfx, x, 0, x, screen_height - 1);
    }

    for (size_t i = 0; i < DHEX_QUAD_COUNT; i++) {
        const size_t logical = DHEX_BUFFER_SIZE - DHEX_QUAD_COUNT + i;
        if (!dhex_slot_filled(logical)) {
            continue;
        }

        const int cell_x = (int)i * cell_w;
        const uint8_t b = dhex_state.buffer[dhex_ring_index(logical)];

        /* Hex value along the top, so the byte stays readable even when
         * its ascii form is a stand-in or a placeholder dot. */
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", b);
        dhex_gfx_set_font(gfx, DHEX_TITLE_FONT);
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
        const int hex_w = (int)dhex_gfx_text_width(gfx, hex);
        dhex_gfx_text(gfx, cell_x + ((cell_w - hex_w) / 2), DHEX_QUAD_HEX_BASELINE, hex);

        char text[3] = {0, 0, 0};
        if (b == 0x0D || b == 0x0A) {
            /* Two letters never fit at the single-glyph size, and these
             * are stand-ins rather than data, so they drop a tier. */
            dhex_gfx_set_font(gfx, DHEX_GFX_FONT_PROFONT_29);
            text[0] = b == 0x0D ? 'C' : 'L';
            text[1] = b == 0x0D ? 'R' : 'F';
        } else if (b >= 0x20 && b <= 0x7E) {
            dhex_gfx_set_font(gfx, DHEX_GFX_FONT_PROFONT_58);
            text[0] = (char)b;
        } else {
            dhex_gfx_set_font(gfx, DHEX_GFX_FONT_PROFONT_29);
            text[0] = '.';
        }

        const int text_w = (int)dhex_gfx_text_width(gfx, text);
        const int x = cell_x + ((cell_w - text_w) / 2);
        /* Centred in what is left of the cell under the hex line, on a
         * baseline shared by every cell so the smaller stand-ins sit level
         * with the full-size characters. */
        const int baseline =
            DHEX_QUAD_HEX_BASELINE + ((cell_h - DHEX_QUAD_HEX_BASELINE + DHEX_QUAD_BASELINE_FONT) / 2);
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
        dhex_gfx_text(gfx, x, baseline, text);
    }

    dhex_gfx_present(gfx);
}

static void dhex_draw_header(dhex_gfx_t *gfx)
{
    const int screen_width = (int)dhex_gfx_width(gfx);

    dhex_gfx_set_font(gfx, DHEX_TITLE_FONT);
    dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
    dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, DHEX_HEADER_TITLE_BASELINE, "dhex");

    const dhex_uart_config_t *cfg = &dhex_state.active_config;
    char params[24];
    snprintf(params,
            sizeof(params),
            "UART%d %u,%u%c%u",
            cfg->port,
            (unsigned)cfg->baud,
            (unsigned)cfg->data_bits,
            cfg->parity,
            (unsigned)cfg->stop_bits);
    char pins[24];
    snprintf(pins, sizeof(pins), "RX%d TX%d", cfg->rx_pin, cfg->tx_pin);

    dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);
    dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
    const int params_w = (int)dhex_gfx_text_width(gfx, params);
    const int pins_w = (int)dhex_gfx_text_width(gfx, pins);
    dhex_gfx_text(gfx,
                      screen_width - DHEX_HEADER_MARGIN - params_w,
                      DHEX_HEADER_LINE1_BASELINE,
                      params);
    dhex_gfx_text(gfx,
                      screen_width - DHEX_HEADER_MARGIN - pins_w,
                      DHEX_HEADER_LINE2_BASELINE,
                      pins);

    dhex_gfx_line(gfx, 0, DHEX_HEADER_HEIGHT, screen_width - 1, DHEX_HEADER_HEIGHT);
}

/* Full-width highlight bars rather than text-measured boxes: simpler,
 * and correct regardless of how narrow/short the panel is. */
static void dhex_draw_config_screen(dhex_gfx_t *gfx)
{
    const dhex_uart_config_t *cfg = &dhex_state.draft_config;
    const int screen_width = (int)dhex_gfx_width(gfx);
    const int screen_height = (int)dhex_gfx_height(gfx);

    dhex_gfx_clear(gfx, DHEX_GFX_COLOR_BLACK);

    dhex_gfx_set_font(gfx, DHEX_TITLE_FONT);
    dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
    dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, DHEX_HEADER_TITLE_BASELINE, "dhex config");
    dhex_gfx_line(gfx, 0, DHEX_HEADER_HEIGHT, screen_width - 1, DHEX_HEADER_HEIGHT);

    char baud_text[12];
    if (cfg->baud == 0) {
        strlcpy(baud_text, "Autobaud", sizeof(baud_text));
    } else {
        snprintf(baud_text, sizeof(baud_text), "%u", (unsigned)cfg->baud);
    }
    char bits_text[4];
    snprintf(bits_text, sizeof(bits_text), "%u", (unsigned)cfg->data_bits);
    const char *parity_text = cfg->parity == 'E' ? "Even" : cfg->parity == 'O' ? "Odd" : "None";
    char stop_text[4];
    snprintf(stop_text, sizeof(stop_text), "%u", (unsigned)cfg->stop_bits);

    static const char *labels[DHEX_CONFIG_FIELD_COUNT] = {"Baud", "Data bits", "Parity", "Stop bits"};
    const char *values[DHEX_CONFIG_FIELD_COUNT] = {baud_text, bits_text, parity_text, stop_text};

    /* Compact layout drops the key hint footer and gives the rows the
     * space it would have taken, down to the bottom of the panel. */
    const int footer_h = dhex_layout_compact(gfx) ? 0 : DHEX_ROW_HEIGHT;
    const int fields_top = DHEX_HEADER_HEIGHT + DHEX_TOP_MARGIN;
    const int fields_bottom = screen_height - footer_h;
    const int row_h = (fields_bottom - fields_top) / (int)DHEX_CONFIG_FIELD_COUNT;

    dhex_gfx_set_font(gfx, DHEX_TITLE_FONT);

    /* Values start on a common column past the widest label, so they line
     * up under each other instead of drifting with the label lengths. */
    int value_x = 0;
    for (uint8_t i = 0; i < DHEX_CONFIG_FIELD_COUNT; i++) {
        const int label_w = (int)dhex_gfx_text_width(gfx, labels[i]);
        if (label_w > value_x) {
            value_x = label_w;
        }
    }
    value_x += DHEX_HEADER_MARGIN + DHEX_CONFIG_LABEL_GAP;

    for (uint8_t i = 0; i < DHEX_CONFIG_FIELD_COUNT; i++) {
        const int row_top = fields_top + ((int)i * row_h);
        const int baseline = row_top + row_h - 4;

        if (i == dhex_state.config_field) {
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
            dhex_gfx_fill_rect(gfx, 0, row_top, screen_width, row_h);
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_BLACK);
        } else {
            dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
        }
        dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, baseline, labels[i]);
        dhex_gfx_text(gfx, value_x, baseline, values[i]);
    }

    if (footer_h > 0) {
        dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_LIGHT);
        dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, screen_height - 2, "Enter: apply  Esc: cancel");
    }

    dhex_gfx_present(gfx);
}

static void dhex_draw_detecting_screen(dhex_gfx_t *gfx)
{
    const int screen_width = (int)dhex_gfx_width(gfx);
    const int screen_height = (int)dhex_gfx_height(gfx);

    dhex_gfx_clear(gfx, DHEX_GFX_COLOR_BLACK);
    dhex_gfx_set_font(gfx, DHEX_TITLE_FONT);
    dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
    dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, DHEX_HEADER_TITLE_BASELINE, "dhex config");
    dhex_gfx_line(gfx, 0, DHEX_HEADER_HEIGHT, screen_width - 1, DHEX_HEADER_HEIGHT);
    dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, screen_height / 2, "Detecting baud...");
    dhex_gfx_present(gfx);
}

/* Autobaud selected: blocks for up to DHEX_AUTOBAUD_TOTAL_MS, so the
 * caller must already have the "detecting" screen on-panel before
 * this runs (the app is unresponsive to input for that stretch). */
static void dhex_apply_config(dhex_context_t *ctx)
{
    if (dhex_state.draft_config.baud == 0) {
        dhex_gfx_t *gfx = dhex_context_gfx(ctx);
        if (gfx != NULL) {
            dhex_draw_detecting_screen(gfx);
        }
        const uint32_t detected = dhex_detect_baud((gpio_num_t)dhex_state.draft_config.rx_pin);
        if (detected != 0) {
            DHEX_LOGI(TAG, "autobaud: detected %u", (unsigned)detected);
            dhex_state.draft_config.baud = detected;
        } else {
            DHEX_LOGW(TAG, "autobaud: no signal detected, kept %u",
                         (unsigned)dhex_state.active_config.baud);
            dhex_state.draft_config.baud = dhex_state.active_config.baud;
        }
    }

    dhex_uart_stop();
    const esp_err_t ret = dhex_uart_start(&dhex_state.draft_config);
    dhex_state.active_config = dhex_state.draft_config;
    dhex_state.uart_ready = ret == ESP_OK;
    if (ret != ESP_OK) {
        DHEX_LOGW(TAG, "UART reconfigure failed: %s", esp_err_to_name(ret));
    }
    if (dhex_config_save(&dhex_state.draft_config) != ESP_OK) {
        DHEX_LOGW(TAG, "failed to save config to NVS");
    }
    dhex_seed_buffer();
}

static void dhex_render(dhex_context_t *ctx)
{
    dhex_gfx_t *gfx = dhex_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    if (dhex_state.config_mode) {
        dhex_draw_config_screen(gfx);
        return;
    }

    if (dhex_state.quad_mode) {
        dhex_draw_quad_screen(gfx);
        dhex_state.last_rendered_total = dhex_state.total_received;
        return;
    }

    dhex_gfx_clear(gfx, DHEX_GFX_COLOR_BLACK);
    dhex_draw_header(gfx);

    if (!dhex_state.uart_ready) {
        dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);
        dhex_gfx_set_color(gfx, DHEX_GFX_COLOR_WHITE);
        dhex_gfx_text(gfx, DHEX_HEADER_MARGIN, DHEX_HEADER_HEIGHT + DHEX_ROW_HEIGHT, "uart unavailable");
        dhex_gfx_present(gfx);
        return;
    }

    const int screen_width = (int)dhex_gfx_width(gfx);
    const int screen_height = (int)dhex_gfx_height(gfx);

    dhex_gfx_set_font(gfx, DHEX_SMALL_FONT);
    const int addr_w = (int)dhex_gfx_text_width(gfx, "0000:") + DHEX_ADDR_GAP;
    const int hex_col_w = (int)dhex_gfx_text_width(gfx, "00 ");
    const int hex_x = addr_w;
    const int ascii_x = hex_x + (int)(DHEX_ROW_BYTES * (size_t)hex_col_w) + DHEX_HEX_ASCII_GAP;
    const int ascii_col_w = (int)dhex_gfx_text_width(gfx, "X ");

    const int top_y = DHEX_HEADER_HEIGHT + DHEX_TOP_MARGIN + DHEX_ROW_HEIGHT;
    dhex_draw_wireshark_section(gfx, top_y, hex_x, ascii_x, hex_col_w, ascii_col_w);

    const int big_col_w = screen_width / (int)DHEX_BUFFER_SIZE;
    const int big_y = screen_height - (DHEX_SECTION_GAP / 2);
    dhex_draw_big_ascii_section(gfx, big_y, big_col_w);

    dhex_gfx_present(gfx);
    dhex_state.last_rendered_total = dhex_state.total_received;
}

static esp_err_t dhex_start(dhex_context_t *ctx)
{
    dhex_gfx_t *gfx = dhex_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    dhex_uart_config_t cfg;
    if (!dhex_parse_args(ctx, &cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&dhex_state, 0, sizeof(dhex_state));
    dhex_state.active_config = cfg;
    dhex_seed_buffer();

    const esp_err_t ret = dhex_uart_start(&cfg);
    if (ret != ESP_OK) {
        DHEX_LOGW(TAG, "UART start failed: %s", esp_err_to_name(ret));
    } else {
        dhex_state.uart_ready = true;
        DHEX_LOGI(TAG,
                     "dhex ready: UART%d TX=%d RX=%d %u,%u%c%u",
                     cfg.port,
                     cfg.tx_pin,
                     cfg.rx_pin,
                     (unsigned)cfg.baud,
                     (unsigned)cfg.data_bits,
                     cfg.parity,
                     (unsigned)cfg.stop_bits);
    }

    dhex_context_set_graphics_active(ctx, true);
    dhex_render(ctx);
    return ESP_OK;
}

static void dhex_stop(dhex_context_t *ctx)
{
    dhex_uart_stop();
    dhex_context_set_graphics_active(ctx, false);
    memset(&dhex_state, 0, sizeof(dhex_state));
}

static void dhex_suspend(dhex_context_t *ctx)
{
    /* Leave the UART driver running so bytes keep arriving in its own
     * ring buffer while this app isn't the active session. */
    dhex_context_set_graphics_active(ctx, false);
}

static void dhex_resume(dhex_context_t *ctx)
{
    dhex_context_set_graphics_active(ctx, true);
    dhex_render(ctx);
}

static void dhex_title(dhex_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    strlcpy(buffer, "dhex", buffer_len);
}

static bool dhex_event(dhex_context_t *ctx, const dhex_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == DHEX_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == DHEX_KEY_APP_EXIT) {
            dhex_context_request_exit(ctx);
            return true;
        }

        if (!dhex_state.config_mode) {
            switch (ch) {
            case DHEX_KEY_ESCAPE:
                dhex_context_request_exit(ctx);
                break;
            case '\r':
            case '\n':
                dhex_state.draft_config = dhex_state.active_config;
                dhex_state.config_field = 0;
                dhex_state.config_mode = true;
                dhex_render(ctx);
                break;
            case ' ':
                dhex_state.quad_mode = !dhex_state.quad_mode;
                dhex_render(ctx);
                break;
            default:
                break;
            }
            return true;
        }

        /* The fields are drawn as a vertical list with the values in a
         * column, so up/down walks the list and left/right moves along the
         * selected value. The original mapping is the other way round,
         * following the rotary encoder convention on the boards that have
         * one; with a keyboard the axes should match the layout. */
        switch (ch) {
        case DHEX_KEY_UP:
            dhex_state.config_field = dhex_state.config_field == 0 ?
                (uint8_t)(DHEX_CONFIG_FIELD_COUNT - 1U) : (uint8_t)(dhex_state.config_field - 1U);
            dhex_render(ctx);
            break;
        case DHEX_KEY_DOWN:
            dhex_state.config_field = (uint8_t)((dhex_state.config_field + 1U) % DHEX_CONFIG_FIELD_COUNT);
            dhex_render(ctx);
            break;
        case DHEX_KEY_RIGHT:
            dhex_config_adjust(&dhex_state.draft_config, dhex_state.config_field, 1);
            dhex_render(ctx);
            break;
        case DHEX_KEY_LEFT:
            dhex_config_adjust(&dhex_state.draft_config, dhex_state.config_field, -1);
            dhex_render(ctx);
            break;
        case '\r':
        case '\n':
            dhex_apply_config(ctx);
            dhex_state.config_mode = false;
            dhex_render(ctx);
            break;
        case DHEX_KEY_ESCAPE:
            dhex_state.config_mode = false;
            dhex_render(ctx);
            break;
        default:
            break;
        }
        return true;
    }

    if (event->type == DHEX_EVENT_TICK) {
        dhex_poll_uart();
        if (!dhex_state.config_mode && dhex_state.total_received != dhex_state.last_rendered_total) {
            dhex_render(ctx);
        }
        return true;
    }

    if (event->type == DHEX_EVENT_RESUME) {
        dhex_resume(ctx);
        return true;
    }

    return false;
}

const dhex_app_t dhex_app = {
    .name = "dhex",
    .summary = "hex/ascii UART dump; usage: dhex [baud framing rx tx [port]] e.g. dhex 9600 8E1 13 14; Enter opens a settings screen (baud/bits/parity/stop) that reconfigures and saves on exit",
    .flags = DHEX_APP_FLAG_RESUMABLE,
    .start = dhex_start,
    .suspend = dhex_suspend,
    .resume = dhex_resume,
    .stop = dhex_stop,
    .event = dhex_event,
    .title = dhex_title,
};
