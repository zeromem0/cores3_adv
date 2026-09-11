/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/cardkb/cardkb.h"
#include "utils/osk/osk.h"
#include "utils/timed/timed.h"
#include "utils/touch_keys/touch_keys.h"
#include "hal_config.h"
#include <apps/utils/audio/audio.h>
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <memory>

static std::unique_ptr<Hal> _hal_instance;
static const std::string _tag = "HAL";

Hal& GetHAL()
{
    if (!_hal_instance) {
        mclog::tagInfo(_tag, "creating hal instance");
        _hal_instance = std::make_unique<Hal>();
    }
    return *_hal_instance.get();
}

void Hal::init()
{
    mclog::tagInfo(_tag, "init");

    M5.begin();
    M5.Display.setBrightness(255);
    M5.Speaker.begin();  // Codec takes some time to initialize

    /* The keyboard is probed before the canvases are built, not after.
     * Whether a CardKB answers on port A decides how tall the on-screen
     * band is, and the canvas is sized around that -- asking afterwards
     * meant the band was always built at full height and only then found
     * to be unwanted, so it stayed on the glass with the application
     * canvas cut short underneath it. */
    i2c_scan();
    keyboard_init();
    display_init();
    setting_init();
    spi_init();
}

void Hal::update()
{
    M5.update();
    cardkb::update();
    osk::update();
    touch_keys::update();
}

void Hal::feedTheDog()
{
    vTaskDelay(1);
}

std::vector<uint8_t> Hal::getDeviceMac()
{
    std::vector<uint8_t> mac(6);
    esp_read_mac(mac.data(), ESP_MAC_EFUSE_FACTORY);
    return mac;
}

std::string Hal::getDeviceMacString()
{
    auto mac = getDeviceMac();
    return fmt::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* -------------------------------------------------------------------------- */
/*                                  Dispplay                                  */
/* -------------------------------------------------------------------------- */
void Hal::display_init()
{
    mclog::tagInfo(_tag, "display init");


    createCanvases();
}

/*
 * The surface the launcher and its applications draw into.
 *
 * There is no keyboard bar and no status bar: the first stood down the
 * left of every screen showing which modifiers were held, the second
 * across the top with a clock and a battery. Both sprites are simply
 * never created -- everything that draws or reads them asks their size
 * first, which is then zero -- and the rows and columns they cost went
 * to the application canvas.
 */
void Hal::createCanvases()
{
    /*
     * Sprites go to PSRAM. Left to itself LovyanGFX asks for internal
     * DMA-capable memory, and on a 320x240 panel the application canvas
     * alone wants 138 KB of it -- which an application that had just
     * released and reclaimed it could not always get, leaving the screen
     * black with only the status bar drawn. The board has 8 MB of PSRAM
     * and the panel is SPI, so the slower pushes do not show.
     */
    canvas.setPsram(true);
    canvasOsk.setPsram(true);

    /* The canvas starts at the top row of the panel -- there is no status
     * bar above it any more -- and stops above the keyboard band, which
     * is zero high when a CardKB is plugged in. Applications lay
     * themselves out from canvas.height(), so they reflow into what is
     * left. */
    canvas.createSprite(display.width(), display.height() - osk::height());
    osk::init();

    /* Said out loud, because the way this fails is silent: pushing a
     * sprite that was never built draws nothing, so the screen stays
     * black while the keys go on working and nothing looks wrong. */
    if (canvas.width() <= 0) {
        mclog::tagError(_tag, "canvas allocation failed, internal {} free, largest {}",
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
}

/*
 * The canvas, given back.
 *
 * It is 150 KB of PSRAM, and an application that has taken the whole
 * panel draws onto the glass and never touches it, so for as long as one
 * is open it is only holding memory. The launcher redraws it from
 * scratch when it gets the screen back, so nothing is lost by throwing
 * the pixels away.
 */
void Hal::setFullScreenApp(bool enabled)
{
    if (_is_full_screen_app == enabled) {
        return;
    }
    _is_full_screen_app = enabled;

    if (enabled) {
        canvas.deleteSprite();
    } else {
        createCanvases();
    }
}

/* -------------------------------------------------------------------------- */
/*                                     I2C                                    */
/* -------------------------------------------------------------------------- */
void Hal::i2c_scan()
{
    mclog::tagInfo(_tag, "i2c scan");

    bool ret[128] = {false};
    M5.In_I2C.scanID(ret);

    uint8_t address;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
    for (int i = 0; i < 128; i += 16) {
        printf("%02x: ", i);
        for (int j = 0; j < 16; j++) {
            fflush(stdout);
            address = i + j;
            if (ret[address]) {
                printf("%02x ", address);
            } else {
                printf("-- ");
            }
        }
        printf("\r\n");
    }
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */
// https://github.com/78/xiaozhi-esp32/blob/main/main/main.cc

void Hal::setting_init()
{
    mclog::tagInfo(_tag, "setting init");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        mclog::tagWarn(_tag, "erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    _settings = new Settings("cardputer", true);
}

/* -------------------------------------------------------------------------- */
/*                                  Keyboard                                  */
/* -------------------------------------------------------------------------- */
void Hal::keyboard_init()
{
    mclog::tagInfo(_tag, "keyboard init");

    cardkb::init();

    if (!keyboard.init()) {
        mclog::tagError(_tag, "keyboard init failed");
        return;
    }
}

/* -------------------------------------------------------------------------- */
/*                                    WiFI                                    */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.3.3/examples/wifi/scan/main/scan.c
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/wifi/getting_started/station/main/station_example_main.c
// http://github.com/espressif/esp-idf/blob/v5.4.2/examples/protocols/sntp/main/sntp_example_main.c
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_event.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>

#define DEFAULT_SCAN_LIST_SIZE 6
static wifi_ap_record_t _ap_info[DEFAULT_SCAN_LIST_SIZE];

void Hal::wifiScan(std::vector<ScanResult_t>& scanResult)
{
    mclog::tagInfo(_tag, "wifi scan");

    scanResult.clear();

    uint16_t number   = DEFAULT_SCAN_LIST_SIZE;
    uint16_t ap_count = 0;
    memset(_ap_info, 0, sizeof(_ap_info));

    // Start WiFi scan
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to start wifi scan: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to get AP number: {}", esp_err_to_name(ret));
        return;
    }

    ret = esp_wifi_scan_get_ap_records(&number, _ap_info);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to get AP records: {}", esp_err_to_name(ret));
        return;
    }

    // Process scan results
    for (int i = 0; i < number; i++) {
        std::string ssid = (char*)_ap_info[i].ssid;
        int rssi         = _ap_info[i].rssi;

        // Skip empty SSID
        if (ssid.empty()) {
            continue;
        }

        // Add to ap_list
        scanResult.push_back(std::make_pair(rssi, ssid));
    }

    // Sort ap_list by RSSI (from strongest to weakest)
    std::sort(scanResult.begin(), scanResult.end(),
              [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                  return a.first > b.first;  // Higher RSSI first
              });

    mclog::tagInfo(_tag, "wifi scan completed, found {} APs", scanResult.size());
}

static EventGroupHandle_t s_wifi_event_group = NULL;
static const int WIFI_CONNECTED_BIT          = BIT0;
static const int WIFI_DISCONNECTED_BIT       = BIT1;
static const int WIFI_FAIL_BIT               = BIT2;
static const int WIFI_STARTED_BIT            = BIT3;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    const char* TAG = "wifi";

    // Wifi started
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_STARTED_BIT);
    }

    // Disconnected
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    // Connected
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void Hal::wifiInit()
{
    mclog::tagInfo(_tag, "wifi init");

    if (_is_wifi_inited) {
        return;
    }

    // The netif, the event loop and the handlers outlive a deinit -- the
    // wifi driver does not own them -- and creating any of them a second
    // time fails, so they are set up once and kept.
    static bool netif_ready = false;
    if (!netif_ready) {
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);

        if (!s_wifi_event_group) {
            s_wifi_event_group = xEventGroupCreate();
        }

        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

        netif_ready = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
    _is_wifi_inited = true;
}

void Hal::wifiDeinit()
{
    mclog::tagInfo(_tag, "wifi deinit");

    if (!_is_wifi_inited) {
        return;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    _is_wifi_inited    = false;
    // Stopping the driver does not always raise a disconnect event, and
    // everything that asks whether the link is up has to see it go down.
    _is_wifi_connected = false;
}

bool Hal::wifiConnect(const std::string& ssid, const std::string& password)
{
    mclog::tagInfo(_tag, "wifi connect to ssid: {} password: {}", ssid, password);

    if (!_is_wifi_inited) {
        wifiInit();
    }

    wifiDisconnect();

    // Hold until wifi started
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));

    // Reset event status
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

    // Set Wi-Fi config
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Wait for connection result
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        mclog::tagInfo(_tag, "connected to SSID: {}", ssid);
        _is_wifi_connected = true;
        start_sntp();
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        mclog::tagError(_tag, "failed to connect to SSID: {}", ssid);
        return false;
    } else {
        mclog::tagError(_tag, "wifi connect timeout");
        return false;
    }
}

std::string Hal::getIpAddress() const
{
    if (!_is_wifi_connected) {
        return "";
    }

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return "";
    }

    char text[16];
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    return text;
}

void Hal::wifiDisconnect()
{
    mclog::tagInfo(_tag, "wifi disconnect");

    if (!_is_wifi_inited) {
        return;
    }

    if (!_is_wifi_connected) {
        return;
    }

    // Hold until wifi started
    xEventGroupWaitBits(s_wifi_event_group, WIFI_STARTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));

    // Disconnect old connection
    ESP_ERROR_CHECK(esp_wifi_disconnect());

    // Wait for disconnect result
    xEventGroupWaitBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

    // Reset event status
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_DISCONNECTED_BIT);

    stop_sntp();

    _is_wifi_connected = false;
}

void Hal::start_sntp()
{
    mclog::tagInfo(_tag, "start sntp");

    if (!_is_wifi_connected) {
        mclog::tagError(_tag, "wifi not connected");
        return;
    }

    setenv("TZ", timed::timezone_rule(), 1);
    tzset();

    /*
     * A client that is already running is asked for a fresh answer, not
     * configured again. Setting the operating mode on a running client
     * is an assert inside lwIP, and this is reached on every reconnect:
     * the board rejoined its network and panicked, over and over.
     */
    if (esp_sntp_enabled()) {
        esp_sntp_restart();
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    /* The servers, and the rule above, belong to the job that uses them
     * -- it is what puts them on screen in the clock application, and
     * one list read from one place cannot disagree with itself. */
    for (int i = 0; i < timed::server_count(); i++) {
        esp_sntp_setservername(i, timed::server_name(i));
    }

    /* Once a day. The chip keeps its own time between syncs and drifts
     * by seconds, not minutes; asking more often would be traffic spent
     * to correct an error nothing here can measure. */
    esp_sntp_set_sync_interval(kTimeSyncIntervalMs);

    esp_sntp_init();
}

void Hal::timeSyncStart()
{
    start_sntp();
}

void Hal::timeSyncStop()
{
    /* Not guarded on the link being up, unlike stop_sntp: stopping the
     * job while the network happens to be down still has to stop it. */
    mclog::tagInfo(_tag, "stop sntp");
    esp_sntp_stop();
}

void Hal::stop_sntp()
{
    mclog::tagInfo(_tag, "stop sntp");

    if (!_is_wifi_connected) {
        mclog::tagError(_tag, "wifi not connected");
        return;
    }

    esp_sntp_stop();
}


/* -------------------------------------------------------------------------- */
/*                                     IR                                     */
/* -------------------------------------------------------------------------- */
#include "utils/ir_nec/ir_helper.h"

void Hal::irInit()
{
    mclog::tagInfo(_tag, "ir init");

    if (_is_ir_inited) {
        mclog::tagInfo(_tag, "ir already inited");
        return;
    }

    ir_helper_init((gpio_num_t)HAL_PIN_IR_TX);

    _is_ir_inited = true;
}

void Hal::irSend(uint8_t addr, uint8_t cmd)
{
    mclog::tagInfo(_tag, "ir send: addr: {:02X}, cmd: {:02X}", addr, cmd);

    if (!_is_ir_inited) {
        mclog::tagError(_tag, "ir not inited");
        return;
    }

    ir_helper_send(addr, cmd);
}

/* -------------------------------------------------------------------------- */
/*                                     BLE                                    */
/* -------------------------------------------------------------------------- */
#include "utils/ble_hid_device/ble_hid_device_helper.h"

void Hal::bleKeyboardInit()
{
    if (_is_ble_keyboard_inited) {
        mclog::tagWarn(_tag, "ble keyboard already initialized");
        return;
    }

    mclog::tagInfo(_tag, "ble keyboard init");

    // Initialize BLE HID device
    ble_hid_device_helper_init();

    // Register keyboard event callback to automatically forward keys
    _ble_keyboard_event_slot_id = keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_ble_keyboard_event(keyEvent); });

    _is_ble_keyboard_inited = true;
    mclog::tagInfo(_tag, "ble keyboard init done, auto-forwarding enabled");
}

bool Hal::bleKeyboardIsConnected() const
{
    if (!_is_ble_keyboard_inited) {
        return false;
    }

    auto state = ble_hid_device_helper_get_state();
    return (state == BLE_HID_DEVICE_STATE_CONNECTED);
}

void Hal::handle_ble_keyboard_event(const Keyboard::KeyEvent_t& keyEvent)
{
    // Only forward if BLE keyboard is connected
    if (!bleKeyboardIsConnected()) {
        return;
    }

    // Create HID buffer (8 bytes: modifier, reserved, keycode1-6)
    uint8_t buffer[8] = {0};

    // Handle key press/release
    if (keyEvent.state) {
        // Get current modifier state from keyboard
        uint8_t modifierMask = keyboard.getModifierMask();

        // Set modifier byte (physical modifiers + any firmware-injected ones, e.g. Fn+alpha -> LSHIFT)
        buffer[0] = modifierMask | keyEvent.extraModifiers;

        // For modifier keys themselves, don't set keycode
        if (keyEvent.isModifier) {
            buffer[2] = 0;  // No keycode for pure modifier keys
        } else {
            buffer[2] = keyEvent.keyCode;
        }

        // Send key press
        ble_hid_device_helper_send(buffer);
        mclog::tagDebug(_tag, "ble keyboard sent key: {} (code: {}, modifier: {:08b})",
                        keyEvent.keyName ? keyEvent.keyName : "special", (int)keyEvent.keyCode, modifierMask);
    } else {
        // Key released - always preserve current modifier state so held modifiers (e.g. ALT) stay active
        buffer[0] = keyboard.getModifierMask();
        buffer[2] = 0;
        ble_hid_device_helper_send(buffer);
        mclog::tagDebug(_tag, "ble keyboard key released (modifier: {:08b})", buffer[0]);
    }
}

/* -------------------------------------------------------------------------- */
/*                                     USB                                    */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.4.2/examples/peripherals/usb/device/tusb_hid
#include "utils/tusb_hid_device/tusb_hid_device_helper.h"

void Hal::usbKeyboardInit()
{
    if (_is_usb_keyboard_inited) {
        mclog::tagWarn(_tag, "usb keyboard already initialized");
        return;
    }

    mclog::tagInfo(_tag, "usb keyboard init");

    delay(200);

    tusb_hid_device_helper_init();

    _usb_keyboard_event_slot_id = keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_usb_keyboard_event(keyEvent); });

    _is_usb_keyboard_inited = true;
}

bool Hal::usbKeyboardIsConnected() const
{
    if (!_is_usb_keyboard_inited) {
        return false;
    }

    return tusb_hid_device_helper_is_mounted();
}

void Hal::handle_usb_keyboard_event(const Keyboard::KeyEvent_t& keyEvent)
{
    // Only forward if USB keyboard is connected
    if (!usbKeyboardIsConnected()) {
        return;
    }

    // Handle key press/release
    if (keyEvent.state) {
        uint8_t mod = GetHAL().keyboard.getModifierMask() | keyEvent.extraModifiers;
        if (keyEvent.isModifier) {
            tusb_hid_device_helper_report(mod, NULL);
        } else {
            uint8_t keycode[6] = {keyEvent.keyCode};
            tusb_hid_device_helper_report(mod, keycode);
        }
        mclog::tagDebug(_tag, "usb keyboard sent key: {} (code: {}, modifier: {:08b})",
                        keyEvent.keyName ? keyEvent.keyName : "special", (int)keyEvent.keyCode, mod);
    } else {
        tusb_hid_device_helper_report(GetHAL().keyboard.getModifierMask(), NULL);
        mclog::tagDebug(_tag, "usb keyboard key released");
    }
}

/* -------------------------------------------------------------------------- */
/*                                     SPI                                    */
/* -------------------------------------------------------------------------- */
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>

static bool _spi_bus_initialized = false;

void Hal::spi_init()
{
    mclog::tagInfo(_tag, "spi init");

    esp_err_t ret;

    // spi_host_device_t host_id = SPI2_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = HAL_PIN_SPI_MOSI,
        .miso_io_num     = HAL_PIN_SPI_MISO,
        .sclk_io_num     = HAL_PIN_SPI_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    // Initialize SPI bus only if not already initialized
    if (!_spi_bus_initialized) {
        ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            mclog::tagError(_tag, "failed to initialize SPI bus");
            return;
        }
        _spi_bus_initialized = true;
        mclog::tagInfo(_tag, "spi bus initialized");
    } else {
        mclog::tagWarn(_tag, "spi bus already initialized, reusing");
    }
}

/* -------------------------------------------------------------------------- */
/*                                   SD Card                                  */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.3.3/examples/storage/sd_card/sdspi
// https://github.com/m5stack/M5PaperS3-UserDemo/blob/main/main/hal/hal.h
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>

#define MOUNT_POINT "/sdcard"

static sdmmc_card_t* _sd_card = nullptr;

void Hal::sd_card_init()
{
    mclog::tagInfo(_tag, "sd card init");

    if (!_spi_bus_initialized) {
        spi_init();
    }

    // If already mounted successfully, return
    if (_is_sd_card_mounted) {
        mclog::tagInfo(_tag, "sd card already mounted");
        return;
    }

    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false, .use_one_fat = false};

    const char mount_point[] = MOUNT_POINT;
    mclog::tagInfo(_tag, "initializing SD card");

    // Initialize SD card slot
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = HAL_PIN_SD_CARD_CS;
    slot_config.host_id               = (spi_host_device_t)host.slot;

    mclog::tagInfo(_tag, "mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &_sd_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            mclog::tagError(_tag, "failed to mount filesystem");
        } else {
            mclog::tagError(_tag, "failed to initialize the card, make sure SD card lines have pull-up resistors");
        }

        // Don't clean up SPI bus on failure - leave it for retry
        mclog::tagInfo(_tag, "sd card init failed, but spi bus remains initialized for retry");
        return;
    }

    mclog::tagInfo(_tag, "filesystem mounted successfully");

    sdmmc_card_print_info(stdout, _sd_card);

    _is_sd_card_mounted = true;
}

Hal::SdCardProbeResult_t Hal::sdCardProbe()
{
    SdCardProbeResult_t result;

    if (!_is_sd_card_mounted) {
        sd_card_init();
        if (!_is_sd_card_mounted) {
            result.is_mounted = false;
            result.size       = "Not Found";
            return result;
        }
    }

    result.is_mounted = true;

    // Try write to sd card
    FILE* fp = fopen(MOUNT_POINT "/test.txt", "w");
    if (fp) {
        fwrite("Hello, World!", 1, 13, fp);
        fclose(fp);

        result.size =
            fmt::format("Size: {:.1f} GB",
                        ((float)((uint64_t)_sd_card->csd.capacity) * _sd_card->csd.sector_size) / (1024 * 1024 * 1024));
    } else {
        result.size = "Write Failed";
    }

    result.type = "Type: ";
    if (_sd_card->is_sdio) {
        result.type += "SDIO";
    } else if (_sd_card->is_mmc) {
        result.type += "MMC";
    } else {
        result.type += (_sd_card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC";
    }

    result.name = fmt::format("Name: {}", std::string(_sd_card->cid.name));

    return result;
}
