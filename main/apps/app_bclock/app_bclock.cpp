/*
 * SPDX-License-Identifier: MIT
 */
#include "app_bclock.h"

#include "assets/bclock_big.h"
#include "assets/bclock_small.h"
#include "assets/digits65.h"
#include "assets/lovyan_icons.h"
#include "assets/molecules.h"
#include "assets/snowflakes.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <hal.h>
#include <mooncake_log.h>

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>

using namespace mooncake;

namespace {

/* Two digits, a separator, two digits: 191 of the panel's 240 columns.
 * The sketch showed hours, minutes and seconds on one row and the whole
 * date on another, which needed 296 columns and a 240-row panel to put
 * them on. Here each row holds one pair. */
constexpr int kFaceWidth = kDigitWidth * 4 + kNarrowWidth;

/* Transparent colour for every push here, as in the sketch: the glyphs
 * and the molecules are cut out against black. */
constexpr std::uint16_t kTransparent = 0x0000;

}  // namespace

void AppBClock::Object_t::move(int width, int height)
{
    r += dr;
    x += dx;
    if (x < 0) {
        x = 0;
        if (dx < 0) dx = -dx;
    } else if (x >= width) {
        x = width - 1;
        if (dx > 0) dx = -dx;
    }
    y += dy;
    if (y < 0) {
        y = 0;
        if (dy < 0) dy = -dy;
    } else if (y >= height) {
        y = height - 1;
        if (dy > 0) dy = -dy;
    }
    z += dz;
    if (z < .5f) {
        z = .5f;
        if (dz < .0f) dz = -dz;
    } else if (z >= 2.0f) {
        z = 2.0f;
        if (dz > .0f) dz = -dz;
    }
}

AppBClock::AppBClock()
{
    setAppInfo().name     = "bClock";
    setAppInfo().userData = new AppIcon_t(image_data_bclock_big, image_data_bclock_small);
}

AppBClock::~AppBClock()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

/*
 * The band pair, and the face they are drawn over.
 *
 * Half the panel first, then a third, then a quarter: the sketch's own
 * search, kept because on this board the largest block that can be had
 * depends on what the network has been doing. A band that is too tall to
 * allocate is not a failure, it is a hint to ask for a shorter one.
 */
bool AppBClock::build_sprites()
{
    auto& display = GetHAL().display;
    const int width = display.width();
    const int height = display.height();

    for (int div = 2; div <= 6; div++) {
        _band_height = (height + div - 1) / div;

        bool failed = false;
        for (int i = 0; i < 2 && !failed; i++) {
            _bands[i].setColorDepth(display.getColorDepth());
            _bands[i].setPsram(false);
            failed = !_bands[i].createSprite(width, _band_height);
        }
        if (!failed) {
            mclog::tagInfo(getAppInfo().name, "{} bands of {} rows, largest block {}", div, _band_height,
                           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            break;
        }

        for (int i = 0; i < 2; i++) {
            _bands[i].deleteSprite();
        }
        _band_height = 0;
    }

    if (_band_height == 0) {
        mclog::tagError(getAppInfo().name, "no room for the bands, largest block {}",
                        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return false;
    }

    /* Three sprites, each holding one drifting thing so it can be
     * rotated and scaled on the way onto the band. Which three they hold
     * is decided separately, and can change while the application runs. */
    for (int i = 0; i < 3; i++) {
        if (!_icons[i].createSprite(kMoleculeSize, kMoleculeSize)) {
            mclog::tagError(getAppInfo().name, "no room for the icons");
            return false;
        }
        _icons[i].setSwapBytes(true);
    }
    load_icons();

    return true;
}

/* Whichever set is wanted, straight from flash into the three sprites.
 * Cheap enough to do on a keypress, which is why no set is ever held in
 * memory beside the one being drawn. */
void AppBClock::load_icons()
{
    static const uint16_t* const kSets[DRIFT_COUNT][3] = {
        {image_data_dioxide, image_data_nitrogen, image_data_oxygen},
        {image_data_fulg1, image_data_fulg2, image_data_fulg3},
        {image_data_info, image_data_alert, image_data_closeX},
    };

    const std::uint8_t set = (_drifting < DRIFT_COUNT) ? _drifting : 0;
    for (int i = 0; i < 3; i++) {
        _icons[i].pushImage(0, 0, kMoleculeSize, kMoleculeSize, kSets[set][i]);
    }
}

void AppBClock::release_sprites()
{
    for (int i = 0; i < 3; i++) {
        _icons[i].deleteSprite();
    }
    for (int i = 0; i < 2; i++) {
        _bands[i].deleteSprite();
    }
    _band_height = 0;
}

/*
 * Two numbers with something between them -- 10:47, or 25.08 -- straight
 * onto one band, at the position it has on the panel with the band's own
 * offset already taken out. Whatever falls outside is clipped by the
 * sprite, which is what lets a row of glyphs 65 tall cross bands of 45
 * without anyone working out which piece belongs where.
 */
void AppBClock::draw_group(LGFX_Sprite& band, int x, int y, int first, int second, int separator)
{
    /* A row of glyphs that misses this band entirely is worth finding
     * out cheaply: each one is 43x65 of transparency tests, and with
     * three bands most rows miss two of them. */
    if (y + kGlyphHeight <= 0 || y >= band.height()) {
        return;
    }

    const int glyphs[5] = {first / 10, first % 10, separator, second / 10, second % 10};
    const int widths[5] = {kDigitWidth, kDigitWidth, kNarrowWidth, kDigitWidth, kDigitWidth};

    band.setSwapBytes(true);
    int at = x;
    for (int i = 0; i < 5; i++) {
        band.pushImage(at, y, widths[i], kGlyphHeight, kGlyphs[glyphs[i]], kTransparent);
        at += widths[i];
    }
    band.setSwapBytes(false);
}

void AppBClock::draw_frame()
{
    auto& display = GetHAL().display;
    const int width = display.width();
    const int height = display.height();

    for (int i = 0; i < kObjectCount; i++) {
        _objects[i].move(width, height);
    }

    /*
     * Two rows of glyphs, and they only just fit: 65 apiece is 130 of
     * the panel's 135, which leaves one row of margin top and bottom and
     * three between them. The year and the seconds are not here because
     * they cannot be -- a third group would need a third 65 rows, and
     * squeezing them in small next to numbers this size looked like an
     * apology for the numbers.
     */
    const int gap    = 3;
    const int face_x = (width - kFaceWidth) / 2;
    const int time_y = (height - (kGlyphHeight * 2 + gap)) / 2;
    const int date_y = time_y + kGlyphHeight + gap;

    std::time_t now = 0;
    std::time(&now);
    std::tm local = {};
    localtime_r(&now, &local);

    char fps[16];
    std::snprintf(fps, sizeof(fps), "%u fps", (unsigned)_fps);

    int flip = 0;
    display.startWrite();
    for (int y = 0; y < height; y += _band_height) {
        flip = flip ? 0 : 1;
        _bands[flip].clear();

        for (int i = 0; i < kObjectCount; i++) {
            const Object_t& a = _objects[i];
            _icons[a.img].pushRotateZoom(&_bands[flip], a.x, a.y - y, a.r, a.z, a.z, kTransparent);
        }

        /* Only in the band that owns the top left corner. Drawn with a
         * foreground colour and no background, so the molecules keep
         * drifting behind the digits. */
        if (y == 0) {
            _bands[flip].setFont(&fonts::Font0);
            _bands[flip].setTextSize(1);
            _bands[flip].setTextColor((std::uint16_t)0xFFFF);
            _bands[flip].drawString(fps, 2, 2);
        }

        draw_group(_bands[flip], face_x, time_y - y, local.tm_hour, local.tm_min, kGlyphColon);
        draw_group(_bands[flip], face_x, date_y - y, local.tm_mday, local.tm_mon + 1, kGlyphDot);

        _bands[flip].pushSprite(&display, 0, y);
    }
    display.endWrite();

    /* Last, and on every frame: the molecules cross the whole panel, so
     * a band drawn once at the top would be walked over within
     * seconds. */
    app_header::draw(display, "Bclock");

    _frames++;
    const std::uint32_t now_ms = GetHAL().millis();
    if (now_ms - _fps_ms >= 1000) {
        _fps    = _frames;
        _frames = 0;
        _fps_ms = now_ms;
    }
}

void AppBClock::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    /* The panel whole, and taken before anything is allocated: the two
     * launcher sprites are 64 KB, and the bands are looking for the room
     * they leave behind. */
    GetHAL().setFullScreenApp(true);
    GetHAL().display.fillScreen(TFT_BLACK);
    app_header::reset();

    _ready = false;

    _objects = new (std::nothrow) Object_t[kObjectCount];
    if (_objects == nullptr) {
        mclog::tagError(getAppInfo().name, "no room for the molecules");
        return;
    }

    const int width  = GetHAL().display.width();
    const int height = GetHAL().display.height();
    for (int i = 0; i < kObjectCount; i++) {
        Object_t& a = _objects[i];
        a.img = i % 3;
        a.x   = rand() % width;
        a.y   = rand() % height;
        a.dx  = ((rand() & 3) + 1) * (i & 1 ? 1 : -1);
        a.dy  = ((rand() & 3) + 1) * (i & 2 ? 1 : -1);
        a.dr  = ((rand() & 3) + 1) * (i & 2 ? 1 : -1);
        a.r   = 0;
        a.z   = (float)((rand() % 10) + 10) / 10;
        a.dz  = (float)((rand() % 10) + 1) / 100;
    }

    _ready = build_sprites();
    if (!_ready) {
        release_sprites();
    }

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& key) {
        if (key.isModifier || !key.state) {
            return;
        }
        if (key.keyCode == KEY_ESC) {
            _close_requested = true;
        } else if (key.keyCode == KEY_SPACE) {
            _drifting = (std::uint8_t)((_drifting + 1) % DRIFT_COUNT);
            load_icons();
            static const char* const kNames[DRIFT_COUNT] = {"molecules", "snowflakes", "icons"};
            mclog::tagInfo(getAppInfo().name, "drifting: {}", kNames[_drifting]);
        }
    });
}

void AppBClock::onRunning()
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

    if (app_header::back_pressed(GetHAL().display.width(), 0, 0)) {
        audio::play_random_tone();
        close();
        return;
    }

    if (!_ready) {
        return;
    }

    draw_frame();
}

void AppBClock::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }

    release_sprites();

    delete[] _objects;
    _objects = nullptr;

    GetHAL().setFullScreenApp(false);
}
