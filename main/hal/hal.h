/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "keyboard/keyboard.h"
#include "utils/settings/settings.h"
#include <M5Unified.hpp>
#include <M5GFX.h>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

class Hal {
public:
    void init();
    void update();

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms)
    {
        m5gfx::delay(ms);
    }
    std::uint32_t millis()
    {
        return m5gfx::millis();
    }
    void feedTheDog();
    std::vector<uint8_t> getDeviceMac();
    std::string getDeviceMacString();

    /* --------------------------------- Display -------------------------------- */
    /* No status bar. It carried a WiFi icon, a battery and a clock across
     * the top of every screen, and on a 240 row panel those 24 rows were
     * worth more to the application under them: the icons on the desktop
     * are a finger's target rather than a fingernail's because of it.
     * What it showed is still reachable -- the battery in about, the
     * address and the clock in jobs and Clock. */
    M5GFX& display                = M5.Display;
    LGFX_Sprite canvas            = LGFX_Sprite(&M5.Display);
    LGFX_Sprite canvasKeyboardBar = LGFX_Sprite(&M5.Display);

    /* The on-screen keyboard band, along the bottom. Zero-sized, and so
     * never pushed, whenever a CardKB is doing the typing instead. */
    LGFX_Sprite canvasOsk = LGFX_Sprite(&M5.Display);

    inline void pushCanvasKeyboardBar()
    {
        // Not created unless the bar is on screen, and pushing a sprite
        // that has no buffer is not something to rely on.
        if (canvasKeyboardBar.width() > 0) {
            canvasKeyboardBar.pushSprite(0, 0);
        }
    }
    inline void pushCanvasOsk()
    {
        if (canvasOsk.width() > 0) {
            canvasOsk.pushSprite(canvasKeyboardBar.width(), display.height() - canvasOsk.height());
        }
    }

    inline void pushCanvas()
    {
        canvas.pushSprite(canvasKeyboardBar.width(), 0);
    }

    /**
     * @brief Claim the glass, rather than the application canvas.
     *
     * Anything that draws into the canvas is pushed to the panel a frame
     * at a time and costs a full-screen copy each way. An application
     * that wants the panel itself -- an emulator, a game -- sets this on
     * open and clears it on close.
     *
     * Claiming the panel also hands back the 150 KB the canvas holds,
     * since nothing is drawing into it, and clearing it builds the
     * canvas again empty for the launcher to redraw.
     */
    void setFullScreenApp(bool enabled);

    inline bool isFullScreenApp() const
    {
        return _is_full_screen_app;
    }

    /* ---------------------------------- Audio --------------------------------- */
    m5::Speaker_Class& speaker = M5.Speaker;
    m5::Mic_Class& mic         = M5.Mic;

    /* ---------------------------------- Input --------------------------------- */
    /* The Cardputer's home key was a button of its own. This board has
     * none: M5.BtnA here is a touch zone along the bottom of the screen,
     * which the nine-cell touch layer is already using. The power
     * button's short click is free, physical, and where a thumb already
     * rests, so that is home. */
    m5::Button_Class& homeButton = M5.BtnPWR;
    Keyboard keyboard;

    /* ---------------------------------- Power --------------------------------- */
    inline uint8_t getBatLevel()
    {
        return M5.Power.getBatteryLevel();
    }

    /* ---------------------------------- WiFi ---------------------------------- */
    using ScanResult_t = std::pair<int, std::string>;
    void wifiInit();
    void wifiDeinit();
    void wifiScan(std::vector<ScanResult_t>& scanResult);
    bool wifiConnect(const std::string& ssid, const std::string& password);
    bool isWifiConnected() const
    {
        return _is_wifi_connected;
    }
    void wifiDisconnect();

    /** @brief Address the router handed out, e.g. "192.168.1.20", empty when offline. */
    std::string getIpAddress() const;

    /* ---------------------------------- Time ---------------------------------- */
    /*
     * How often the clock is fetched. A day: the chip keeps time on its
     * own between syncs, and the drift over that span is smaller than
     * anything this device does anything with.
     */
    static constexpr std::uint32_t kTimeSyncIntervalMs = 24U * 60U * 60U * 1000U;

    /**
     * @brief Start and stop the time client.
     *
     * Public because the timed job owns the decision: stopping that job
     * should stop the fetching, and it does not otherwise get a say --
     * connecting to WiFi starts this on its own.
     */
    void timeSyncStart();
    void timeSyncStop();

    bool isTimeSynced() const
    {
        // Time code in HAL because the clock is technically in hardware. And the
        // WiFi components set it. Don't check esp_sntp_get_sync_status() because
        // we're not continuously syncing.
        constexpr time_t t2026 = 1767225600;  // "2026-01-01T00:00Z"
        return time(NULL) > t2026;
    }


    /* ----------------------------------- IR ----------------------------------- */
    void irInit();
    void irSend(uint8_t addr, uint8_t cmd);

    /* ----------------------------------- BLE ---------------------------------- */
    void bleKeyboardInit();
    bool bleKeyboardIsConnected() const;

    /* ----------------------------------- USB ---------------------------------- */
    void usbKeyboardInit();
    bool usbKeyboardIsConnected() const;

    /* -------------------------------- Settings -------------------------------- */
    Settings& getSettings()
    {
        return *_settings;
    }

    /* ----------------------------------- IMU ---------------------------------- */
    m5::IMU_Class& imu = M5.Imu;

    /* --------------------------------- SD Card -------------------------------- */
    struct SdCardProbeResult_t {
        bool is_mounted = false;
        std::string size;
        std::string type;
        std::string name;

        bool operator==(const SdCardProbeResult_t& other) const
        {
            return is_mounted == other.is_mounted && size == other.size && type == other.type && name == other.name;
        }
    };

    SdCardProbeResult_t sdCardProbe();

private:
    Settings* _settings             = nullptr;
    bool _is_wifi_inited            = false;
    bool _is_wifi_connected         = false;
    bool _is_ir_inited              = false;
    bool _is_ble_keyboard_inited    = false;
    bool _is_usb_keyboard_inited    = false;
    bool _is_sd_card_mounted        = false;
    bool _is_full_screen_app        = false;
    int _ble_keyboard_event_slot_id = -1;
    int _usb_keyboard_event_slot_id = -1;

    void display_init();
    void createCanvases();
    void i2c_scan();
    void keyboard_init();
    void start_sntp();
    void stop_sntp();
    void setting_init();
    void spi_init();
    void sd_card_init();
    void handle_ble_keyboard_event(const Keyboard::KeyEvent_t& keyEvent);
    void handle_usb_keyboard_event(const Keyboard::KeyEvent_t& keyEvent);
};

Hal& GetHAL();
