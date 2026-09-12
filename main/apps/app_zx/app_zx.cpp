/*
 * SPDX-License-Identifier: MIT
 */
#include "app_zx.h"

#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <hal.h>
#include <hal/utils/jobs/jobs.h>
#include <mooncake_log.h>

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <strings.h>
#include <algorithm>
#include <cstring>

/* A tape built into the firmware is optional, and none is carried here:
 * the Spectrum games worth playing are still under copyright. Drop a
 * header next to this one, generated from a .tap by tools/tap_to_header.py,
 * and it appears in the list. See assets/builtin_tape.h.example. */
#if __has_include("assets/builtin_tape.h")
#include "assets/builtin_tape.h"
#define ZX_HAS_BUILTIN_TAPE 1
#endif

#include "assets/zx_big.h"
#include "assets/zx_small.h"
#include "zx_host.h"
#include "zx/spectrum/spectrum_mini.h"
#include "zx/spectrum/tap_loader.h"
#include "zx/spectrum/z80_loader.h"

using namespace mooncake;

namespace {

constexpr char kMountPoint[] = "/sdcard";

/* One emulated frame is 1/50 s, and the emulator is written to be run at
 * that rate: the border and beeper both assume it. */
constexpr std::uint32_t kFrameIntervalMs = 20;

/* Browser layout, drawn straight onto the panel like the emulator is.
 * How many tapes are listed comes from the panel rather than from a
 * number: this list was written for 135 rows and would show six of them
 * on a screen with room for twelve. */
constexpr int kRowHeight = 16;

/* Under the band across the top, which the browser carries like every
 * other screen. The emulator itself does not: what it draws is the
 * Spectrum's own screen and its border, and a band over that would be a
 * band over the machine. */
int list_top()
{
    return app_header::height() + 8;
}

int visible_rows()
{
    /* The hint line along the bottom keeps the last row for itself. */
    const int rows = (GetHAL().display.height() - list_top() - kRowHeight) / kRowHeight;
    return rows > 1 ? rows : 1;
}

/* Each key can hold down two Spectrum keys at once, since the symbols the
 * Spectrum only reaches with symbol shift need both. Indexed by HID scan
 * code, so a release puts down exactly what the press picked up. */
struct KeyBinding_t {
    std::uint8_t first;
    std::uint8_t second;
};
KeyBinding_t _held[128];

bool has_extension(const std::string& name, const char* extension)
{
    const size_t length = std::strlen(extension);
    if (name.size() <= length) {
        return false;
    }
    return strcasecmp(name.c_str() + name.size() - length, extension) == 0;
}

}  // namespace

AppZX::AppZX()
{
    setAppInfo().name     = "ZX";
    setAppInfo().userData = new AppIcon_t(image_data_zx_big, image_data_zx_small);
}

AppZX::~AppZX()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppZX::build_list()
{
    _entries.clear();

    /* The bare machine first, then whatever tape the firmware carries:
     * both work with no card in the slot, which is most of the time. */
    _entries.push_back({"BASIC (no tape)", "", false});
#ifdef ZX_HAS_BUILTIN_TAPE
    _entries.push_back({std::string(builtin_tape_title) + " (flash)", "", true});
#endif

    const size_t built_in = _entries.size();

    /* Probing mounts the card if it is not up yet, which is the only
     * public way in. */
    if (!GetHAL().sdCardProbe().is_mounted) {
        mclog::tagWarn(getAppInfo().name, "no sd card");
        return;
    }

    /* The card root and the folder the standalone emulator reads, so a
     * card prepared for either one works as it is. Names are kept
     * relative to the mount point so loading knows where to look. */
    static const char* const kSearchDirs[] = {"", "/ZXgames"};

    for (const char* subdir : kSearchDirs) {
        const std::string base = std::string(kMountPoint) + subdir;

        DIR* dir = opendir(base.c_str());
        if (dir == nullptr) {
            continue;
        }

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            const std::string name = entry->d_name;
            if (has_extension(name, ".tap") || has_extension(name, ".z80")) {
                const std::string path =
                    std::string(subdir).empty() ? name : std::string(subdir + 1) + "/" + name;
                _entries.push_back({path, path, false});
            }
        }
        closedir(dir);
    }

    std::sort(_entries.begin() + built_in, _entries.end(),
              [](const Entry_t& a, const Entry_t& b) { return a.label < b.label; });
    mclog::tagInfo(getAppInfo().name, "{} tapes on card", _entries.size() - built_in);
}

void AppZX::draw_browser()
{
    auto& display = GetHAL().display;

    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(1);
    display.setFont(&fonts::Font0);

    app_header::draw(display, "ZX");
    display.setFont(&fonts::Font0);
    display.setTextSize(1);
    display.setTextDatum(top_left);

    const int total = (int)_entries.size();
    for (int i = 0; i < visible_rows() && (_scroll + i) < total; i++) {
        const int index    = _scroll + i;
        const int y        = list_top() + i * kRowHeight;
        const bool current = (index == _selected);

        if (current) {
            display.fillRect(0, y - 2, display.width(), kRowHeight, TFT_DARKGREEN);
        }
        display.setTextColor(current ? TFT_WHITE : TFT_LIGHTGREY, current ? TFT_DARKGREEN : TFT_BLACK);

        std::string label = _entries[index].label;
        if (label.size() > 36) {
            label.resize(36);
        }
        display.drawString(label.c_str(), 8, y);
    }

    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("Enter: tape   Tab: instant   Esc: exit", 8, display.height() - 12);
}

bool AppZX::start_machine(const Entry_t& entry, bool instant)
{
    mclog::tagInfo(getAppInfo().name, "free heap {} largest block {}", esp_get_free_heap_size(),
                   heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    _spectrum = new ZXSpectrum();
    if (!_spectrum->init_48k()) {
        mclog::tagError(getAppInfo().name, "48k init failed");
        _last_error = jobs::is_running("wifid") ? "no memory: stop wifid in jobs" : "not enough memory";
        delete _spectrum;
        _spectrum = nullptr;
        return false;
    }
    _spectrum->reset();

    if (!zx_host::renderer_begin()) {
        delete _spectrum;
        _spectrum = nullptr;
        return false;
    }
    _audio_ready = zx_host::beeper_begin();

    /* The tape loader emulates the tape rather than dropping the blocks
     * into memory, so it is handed a way to draw and the loading stripes
     * appear as they did off a real one. Loading runs to completion here
     * rather than a frame at a time, which is long enough that the
     * watchdog has to be fed along the way. */
    auto on_tape_frame = [this]() {
        zx_host::renderer_draw(_spectrum->mem.getScreenData(), _spectrum->borderColor);
        GetHAL().feedTheDog();
    };

    if (entry.builtin || !entry.path.empty()) {
        bool loaded = false;

        TAPLoader loader;
        const std::string full = std::string(kMountPoint) + "/" + entry.path;

        if (entry.builtin) {
#ifdef ZX_HAS_BUILTIN_TAPE
            loaded = instant
                         ? loader.loadTAPInstantFromMemory(builtin_tape, builtin_tape_size, _spectrum, on_tape_frame)
                         : loader.loadTAPFromMemory(builtin_tape, builtin_tape_size, _spectrum, on_tape_frame);
#endif
        } else if (has_extension(entry.path, ".z80")) {
            /* A snapshot is already the machine's memory, so there is
             * nothing to load slowly and nothing to speed up. */
            Z80Loader snapshot;
            loaded = snapshot.loadZ80(full.c_str(), _spectrum);
            if (!loaded) {
                mclog::tagError(getAppInfo().name, "z80 load failed: {}", snapshot.getLastError());
            }
        } else {
            loaded = instant ? loader.loadTAPInstant(full.c_str(), _spectrum, on_tape_frame)
                             : loader.loadTAP(full.c_str(), _spectrum, on_tape_frame);
        }

        if (!loaded && !has_extension(entry.path, ".z80")) {
            mclog::tagError(getAppInfo().name, "tape load failed: {}", loader.getLastError());
            _last_error = "tape load failed";
        }

        if (!loaded) {
            zx_host::renderer_end();
            delete _spectrum;
            _spectrum = nullptr;
            return false;
        }
    }

    std::memset(_held, 0, sizeof(_held));
    _state         = STATE_RUNNING;
    _next_frame_ms = GetHAL().millis();
    return true;
}

void AppZX::handle_browser_key(std::uint8_t ch)
{
    const int total = (int)_entries.size();

    switch (ch) {
        case 'w':  // up
            if (_selected > 0) {
                _selected--;
            }
            break;
        case 's':  // down
            if (_selected < total - 1) {
                _selected++;
            }
            break;
        case '\r':
        case '\t':
            /* Loading a tape takes as long as it did off a real one, so
             * the list says what is happening before it starts. */
            GetHAL().display.setTextColor(TFT_YELLOW, TFT_BLACK);
            GetHAL().display.drawString(ch == '\t' ? "instant..." : "loading...", 150,
                                        GetHAL().display.height() - 12);

            if (!start_machine(_entries[_selected], ch == '\t')) {
                /* Nothing started, so the list stays up and says why.
                 * Nearly always it is the heap: the machine wants 48 KB
                 * in one piece and the WiFi driver is holding it. */
                draw_browser();
                GetHAL().display.setTextColor(TFT_RED, TFT_BLACK);
                GetHAL().display.drawString(_last_error, 8, GetHAL().display.height() - 12);
                return;
            }
            return;
        default:
            return;
    }

    if (_selected < _scroll) {
        _scroll = _selected;
    } else if (_selected >= _scroll + visible_rows()) {
        _scroll = _selected - visible_rows() + 1;
    }
    draw_browser();
}

void AppZX::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    /* The panel is the machine's, bars included. */
    GetHAL().setFullScreenApp(true);

    /* The background jobs are left exactly as they are. The Spectrum's
     * 48K have to come out of a heap the WiFi driver holds most of, so
     * with the network up the machine will not start -- but which of the
     * two is wanted at a given moment is not this application's to
     * decide, and the jobs application is where that choice is made. */

    app_header::reset();

    _state    = STATE_BROWSER;
    _selected = 0;
    _scroll   = 0;
    build_list();
    draw_browser();

    /* Both the list and the emulator read the arrow keys, which only
     * report as arrows while Fn is held; taking them by matrix position
     * means they work bare, the same way the launcher reads them. */
    _key_raw_slot = GetHAL().keyboard.onKeyEventRaw.connect([this](const Keyboard::KeyEventRaw_t& key) {
        /* The Spectrum's two shifts, on modifiers this application has no
         * other use for. They are what the machine's own keyboard is
         * built around: symbol shift reaches the punctuation and the
         * keywords printed under the keys, and the two held together put
         * BASIC in extended mode, where BEEP, INK, PAPER and every
         * function live. Without them half of BASIC is unreachable. */
        if (_state == STATE_RUNNING && _spectrum != nullptr) {
            if (key.row == 3 && key.col == 0) {  // Ctrl
                _spectrum->updateKey(SPECKEY_SYMB, key.state ? 1 : 0);
                return;
            }
            if (key.row == 3 && key.col == 1) {  // Opt
                _spectrum->updateKey(SPECKEY_SHIFT, key.state ? 1 : 0);
                return;
            }
        }

        if (key.row == 2 && key.col == 0) {  // Fn
            _fn_held = key.state;
            return;
        }

        /* The four arrow-marked keys, in matrix order up, down, left,
         * right. Bare they are ';' '.' ',' '/' and BASIC needs every one
         * of them, so they only steer while Fn is held -- the same rule
         * the rest of this firmware uses. */
        int arrow = -1;
        if (key.row == 2 && key.col == 11) {
            arrow = 0;
        } else if (key.row == 3 && key.col == 11) {
            arrow = 1;
        } else if (key.row == 3 && key.col == 10) {
            arrow = 2;
        } else if (key.row == 3 && key.col == 12) {
            arrow = 3;
        }
        if (arrow < 0) {
            return;
        }

        if (_state == STATE_BROWSER) {
            if (key.state && arrow < 2) {
                handle_browser_key(arrow == 0 ? 'w' : 's');
            }
            return;
        }

        if (_spectrum == nullptr) {
            return;
        }

        /* A release is honoured whatever Fn is doing by then: letting go
         * of Fn first would otherwise leave the key held down forever. */
        if (!key.state) {
            if (_arrow_key[arrow] != SPECKEY_NONE) {
                _spectrum->updateKey((SpecKeys)_arrow_key[arrow], 0);
                _spectrum->updateKey(SPECKEY_SHIFT, 0);
                _arrow_key[arrow] = SPECKEY_NONE;
            }
            return;
        }

        if (!_fn_held) {
            return;  // Bare: the character handler sends the punctuation.
        }

        /* The Spectrum has no arrow keys: its cursor is caps shift with
         * 5-8, which is what BASIC and cursor-key games both read. */
        static const SpecKeys kCursor[4] = {SPECKEY_7, SPECKEY_6, SPECKEY_5, SPECKEY_8};
        _arrow_key[arrow] = kCursor[arrow];
        _spectrum->updateKey(SPECKEY_SHIFT, 1);
        _spectrum->updateKey(kCursor[arrow], 1);
    });

    _key_slot = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& key) {
        if (key.isModifier) {
            return;
        }

        if (_state == STATE_BROWSER) {
            if (!key.state) {
                return;
            }
            if (key.keyCode == KEY_ENTER) {
                handle_browser_key('\r');
            } else if (key.keyCode == KEY_TAB) {
                handle_browser_key('\t');
            } else if (key.keyCode == KEY_ESC) {
                _close_requested = true;
            }
            return;
        }

        if (_spectrum == nullptr || key.keyCode >= 128) {
            return;
        }

        /* Already delivered as cursor keys by the raw handler above. */
        if (key.keyCode == KEY_UP || key.keyCode == KEY_DOWN || key.keyCode == KEY_LEFT ||
            key.keyCode == KEY_RIGHT) {
            return;
        }

        if (!key.state) {
            KeyBinding_t& binding = _held[key.keyCode];
            if (binding.first != SPECKEY_NONE) {
                _spectrum->updateKey((SpecKeys)binding.first, 0);
            }
            if (binding.second != SPECKEY_NONE) {
                _spectrum->updateKey((SpecKeys)binding.second, 0);
            }
            binding = {(std::uint8_t)SPECKEY_NONE, (std::uint8_t)SPECKEY_NONE};
            return;
        }

        /* Backspace is the Spectrum's DELETE, which is caps shift with 0. */
        if (key.keyCode == KEY_BACKSPACE) {
            _held[key.keyCode] = {(std::uint8_t)SPECKEY_SHIFT, (std::uint8_t)SPECKEY_0};
        } else if (key.keyCode == KEY_ENTER) {
            _held[key.keyCode] = {(std::uint8_t)SPECKEY_ENTER, (std::uint8_t)SPECKEY_NONE};
        } else if (key.keyName != nullptr && key.keyName[0] != '\0' && key.keyName[1] == '\0') {
            bool needs_symbol = false;
            const SpecKeys mapped = zx_host::map_char(key.keyName[0], &needs_symbol);
            if (mapped == SPECKEY_NONE) {
                return;
            }
            _held[key.keyCode] = {(std::uint8_t)mapped,
                                  (std::uint8_t)(needs_symbol ? SPECKEY_SYMB : SPECKEY_NONE)};
        } else {
            return;
        }

        const KeyBinding_t& binding = _held[key.keyCode];
        if (binding.first != SPECKEY_NONE) {
            _spectrum->updateKey((SpecKeys)binding.first, 1);
        }
        if (binding.second != SPECKEY_NONE) {
            _spectrum->updateKey((SpecKeys)binding.second, 1);
        }
    });
}

void AppZX::onRunning()
{
    if (_close_requested) {
        _close_requested = false;
        audio::play_random_tone();
        close();
        return;
    }

    /* The home button leaves the emulator, the same as every other
     * application here; nothing inside the machine can claim it. */
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    /* Only on the list. Once the machine is running the panel is its
     * screen and its border, and there is no band up there to press. */
    if (_state == STATE_BROWSER &&
        app_header::back_pressed(GetHAL().display.width(), 0, 0)) {
        audio::play_random_tone();
        close();
        return;
    }

    if (_state != STATE_RUNNING || _spectrum == nullptr) {
        return;
    }

    const std::uint32_t now = GetHAL().millis();
    if ((std::int32_t)(now - _next_frame_ms) < 0) {
        return;
    }
    /* Late frames are dropped rather than chased: catching up would only
     * make the beeper stutter further behind. */
    _next_frame_ms = now + kFrameIntervalMs;

    uint16_t accumulator[312] = {0};
    _spectrum->runForFrame(accumulator);

    zx_host::renderer_draw(_spectrum->mem.getScreenData(), _spectrum->borderColor);
    if (_audio_ready) {
        zx_host::beeper_submit_frame(accumulator);
    }
}

void AppZX::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_key_slot >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot);
        _key_slot = -1;
    }
    if (_key_raw_slot >= 0) {
        GetHAL().keyboard.onKeyEventRaw.disconnect(_key_raw_slot);
        _key_raw_slot = -1;
    }

    if (_audio_ready) {
        zx_host::beeper_end();
        _audio_ready = false;
    }
    zx_host::renderer_end();

    delete _spectrum;
    _spectrum = nullptr;

    /* Giving the panel back also builds the launcher's sprites again,
     * empty; it notices and redraws them itself. */
    GetHAL().setFullScreenApp(false);
}
