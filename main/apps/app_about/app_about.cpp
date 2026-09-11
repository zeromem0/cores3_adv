/*
 * SPDX-License-Identifier: MIT
 *
 * One implementation for both panels. Everything that would otherwise
 * be a fixed coordinate is worked out from the canvas, because the two
 * boards this runs on differ by more than three times in each
 * direction: 240x111 on the Cardputer, 800x456 on the LCD-5. Text size
 * follows the width, the rows follow the font, and the labels shorten
 * when there is no room for the long ones.
 */
#include "app_about.h"

#include <apps/utils/audio/audio.h>
#include <apps/utils/theme.h>
#include <hal.h>
#include <mooncake_log.h>

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

/* The mascot lives in boot_screen.h. logo.h next to it is the M5Stack
 * splash -- the wordmark and "Any Key to start" -- which is not what
 * anyone means by the mascot. */
#include "../app_launcher/view/boot_anim/assets/boot_screen.h"
#include "assets/about_big.h"
#include "assets/about_small.h"
#include <apps/utils/common.h>

#include <cstdio>

using namespace mooncake;

namespace {

constexpr std::uint32_t kRefreshMs = 1000;

/* Who made this, on two lines, because both halves are true and neither
 * is the whole of it. */
constexpr const char* kAuthorHuman = "zeromem0 - boards, wiring, every test on real glass";
constexpr const char* kAuthorModel = "Claude Opus 5 - the porting, one flash at a time";

std::size_t app_partition_size()
{
    const esp_partition_t* part = esp_ota_get_running_partition();
    return part != nullptr ? part->size : 0;
}

/*
 * What the memory bus was built to run at.
 *
 * These are the configured values, not measured ones -- but read on a
 * screen they answer the question that matters, because a board showing
 * you this line is a board that got far enough to draw it. On the LCD-5
 * that is the whole test: 120MHz PSRAM stops during start-up, well
 * before any application, so if this says 120 then 120 works. It saved
 * having to watch a serial port to find out.
 */
int psram_mhz()
{
#if defined(CONFIG_SPIRAM_SPEED_120M)
    return 120;
#elif defined(CONFIG_SPIRAM_SPEED_80M)
    return 80;
#elif defined(CONFIG_SPIRAM_SPEED_40M)
    return 40;
#else
    return 0;
#endif
}

int flash_mhz()
{
#if defined(CONFIG_ESPTOOLPY_FLASHFREQ_120M)
    return 120;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_80M)
    return 80;
#elif defined(CONFIG_ESPTOOLPY_FLASHFREQ_40M)
    return 40;
#else
    return 0;
#endif
}

}  // namespace

AppAbout::AppAbout()
{
    setAppInfo().name = "about";
    setAppInfo().userData = new AppIcon_t(image_data_about_big, image_data_about_small);
}

AppAbout::~AppAbout()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppAbout::draw_colour_bar(int y, int height)
{
    auto& canvas = GetHAL().canvas;
    const int width = canvas.width();

    /* Eight primaries: every combination of full red, green and blue. If
     * the byte order were wrong these would not be primaries. */
    const int band = height / 2;
    for (int i = 0; i < 8; i++) {
        const std::uint16_t rgb = (std::uint16_t)((i & 1 ? 0x001F : 0) | (i & 2 ? 0x07E0 : 0) |
                                                  (i & 4 ? 0xF800 : 0));
        canvas.fillRect(i * width / 8, y, width / 8 + 1, band, rgb);
    }

    /* A grey ramp under them: banding here means the panel is not
     * getting the six bits of green it should. */
    for (int x = 0; x < width; x++) {
        const int v = x * 255 / (width - 1);
        canvas.drawFastVLine(x, y + band, height - band, canvas.color565(v, v, v));
    }
}

void AppAbout::draw()
{
    auto& canvas = GetHAL().canvas;
    const int width = canvas.width();
    const int height = canvas.height();

    /* Two sizes, chosen from the width. The font is set here rather than
     * assumed: the canvas keeps whatever the last application left on
     * it, and every measurement below comes from fontHeight(). */
    const int body_size = width >= 480 ? 2 : 1;
    const bool roomy    = width >= 480;

    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(top_left);

    const esp_app_desc_t* app = esp_app_get_description();

    /*
     * The mascot's head, from the boot screen's own artwork.
     *
     * Only the top of it: the image is 240x135 stored row by row, so
     * asking for the first rows of it is asking for the top of the
     * picture -- no separate asset, no cropping, just a shorter height
     * than the data holds.
     *
     * Wide panels only. On the Cardputer the mascot is as wide as the
     * whole screen, so there is nowhere to put it that is not on top of
     * the text it would be introducing.
     */
    if (roomy) {
        constexpr int kLogoW = 240;
        constexpr int kLogoRows = 74;
        canvas.pushImage(width - kLogoW - 8, 4, kLogoW, kLogoRows, image_data_boot);
    }

    canvas.setTextSize(body_size + 1);
    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString(roomy ? "Waveshare ESP32-S3-Touch-LCD-5" : "Cardputer ADV", 4, 2);
    int y = 2 + canvas.fontHeight() + 2;

    canvas.setTextSize(body_size);
    const int row = canvas.fontHeight() + (roomy ? 8 : 2);

    canvas.setTextColor(TFT_LIGHTGREY, THEME_COLOR_BG);
    char line[128];
    std::snprintf(line, sizeof(line), "%s %s", app->version, app->date);
    canvas.drawString(line, 4, y);
    y += row;

    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    canvas.setTextColor(TFT_WHITE, THEME_COLOR_BG);

    std::snprintf(line, sizeof(line), roomy ? "CPU: ESP32-S3 rev %d, %d cores, %d MHz, IDF %s"
                                            : "CPU: S3 rev %d, %dc, %dMHz",
                  chip.revision, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_get_idf_version());
    canvas.drawString(line, 4, y);
    y += row;

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    std::snprintf(line, sizeof(line), roomy ? "FLASH: %uM at %dMHz, app partition %uk"
                                            : "FLASH: %uM %dMHz, app %uk",
                  (unsigned)(flash_size / (1024U * 1024U)), flash_mhz(),
                  (unsigned)(app_partition_size() / 1024U));
    canvas.drawString(line, 4, y);
    y += row;

    /* Largest block, not just the total: on both boards the thing that
     * fails is an allocation that needs its bytes in one piece. */
    std::snprintf(line, sizeof(line), roomy ? "RAM: %uk free of %uk, largest block %uk"
                                            : "RAM: %uk/%uk, max %uk",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U),
                  (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024U),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024U));
    canvas.drawString(line, 4, y);
    y += row;

    const std::size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 0) {
        std::snprintf(line, sizeof(line), roomy ? "PSRAM: %uk free of %uk at %dMHz"
                                                : "PSRAM: %uk/%uk %dMHz",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U),
                      (unsigned)(psram / 1024U), psram_mhz());
    } else {
        /* Said rather than left out. On this board it is the single fact
         * that shapes every other decision. */
        std::snprintf(line, sizeof(line), "PSRAM: none");
    }
    canvas.drawString(line, 4, y);
    y += row;

    std::snprintf(line, sizeof(line), "PANEL: %dx%d", GetHAL().display.width(),
                  GetHAL().display.height());
    canvas.drawString(line, 4, y);
    y += row;

    const std::string ip = GetHAL().getIpAddress();
    std::snprintf(line, sizeof(line), "MAC: %s", GetHAL().getDeviceMacString().c_str());
    canvas.drawString(line, 4, y);
    y += row;

    std::snprintf(line, sizeof(line), "IP: %s", ip.empty() ? "offline" : ip.c_str());
    canvas.drawString(line, 4, y);
    y += row;

    std::snprintf(line, sizeof(line), "UP: %lu s", (unsigned long)(GetHAL().millis() / 1000U));
    canvas.drawString(line, 4, y);
    y += row + (roomy ? row / 2 : 2);

    canvas.setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    canvas.drawString(kAuthorHuman, 4, y);
    y += row;
    canvas.drawString(kAuthorModel, 4, y);

    /* The strip sits on the bottom edge, out of the way of the text. */
    const int bar_h = roomy ? height / 8 : 8;
    draw_colour_bar(height - bar_h, bar_h);

    GetHAL().pushCanvas();
}

void AppAbout::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& event) {
        if (!event.state || event.isModifier) {
            return;
        }
        if (event.keyCode == KEY_ESC) {
            _close_requested = true;
        }
    });

    draw();
    _last_draw_ms = GetHAL().millis();
}

void AppAbout::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        audio::play_random_tone();
        close();
        return;
    }

    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    /* Once a second, for the uptime and whatever the network has been
     * doing; everything else on the page is settled by then. */
    const std::uint32_t now = GetHAL().millis();
    if (now - _last_draw_ms >= kRefreshMs) {
        _last_draw_ms = now;
        draw();
    }
}

void AppAbout::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
}
