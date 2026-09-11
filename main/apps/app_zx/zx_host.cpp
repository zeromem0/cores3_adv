/*
 * SPDX-License-Identifier: MIT
 *
 * Emulator core: Copyright (c) 2025 AndyAiCardputer, MIT licensed.
 * See zx/ for the sources carried in from that project.
 */
#include "zx_host.h"

#include <mooncake_log.h>

#include "zx/spectrum/spectrum_mini.h"

#include <cstdlib>
#include <cstring>

/* Spelled with the directory: a component in this build also ships a
 * hal.h, and the bare name reaches that one instead. */
#include <hal/hal.h>

namespace zx_host {

namespace {

const std::string _tag = "zx";

/* Rendered a band at a time rather than into a full frame. A whole one
 * would be 96 KB, and while this board has PSRAM to hold it, a band of
 * 8 KB in internal memory is what the SPI bus can send without a
 * round trip through the slower RAM on every push. */
constexpr int kBandRows = 16;

constexpr int kZxWidth = kZxScreenWidth;
constexpr int kZxHeight = kZxScreenHeight;

uint16_t* _band;
uint16_t _flash_counter;
bool _flash_state;

/* A copy of the screen as it was last drawn. Pushing all 256x192 pixels
 * costs 96 KB over a bus that needs 10 ms for them, half of what a frame
 * has; most frames change a line or two, so the bands that did not change
 * are neither drawn nor sent. */
constexpr int kScreenBytes = 6912;
uint8_t* _shadow;
bool _force_redraw = true;

/* Where the screen sits on the panel. The 32 columns and 24 rows left
 * over on a 320x240 board are the border, which is what the machine
 * would have put there. */
int _origin_x;
int _origin_y;

/* The border is a flat colour that changes rarely -- a game sets it once,
 * a tape loader stripes it -- so it is painted when it changes and left
 * alone otherwise. Eight is not a colour, which makes the first frame
 * paint it whatever it turns out to be. */
uint8_t _border_drawn = 0xff;

/*
 * The bus is left at the speed the board was brought up at.
 *
 * The port this came from doubled it to 80 MHz while the emulator held
 * the screen, which the Cardputer's ST7789 takes. This panel is an
 * ILI9342C and does not: the pixels arrived, but shifted and interleaved,
 * so a screen of text came out as a diagonal smear of half-glyphs while
 * everything else on the board -- drawn at 40 MHz -- was sharp. What paid
 * for the overclock was one full frame per frame; here most frames send
 * the two or three bands that changed, and 96 KB at 40 MHz is 20 ms only
 * on the frames that redraw everything.
 */

/* The four rectangles around the screen, in whatever colour the machine
 * last wrote to port 0xFE. */
void paint_border(uint8_t colour)
{
    auto& display = GetHAL().display;

    /* The palette is held the way the screen is pushed: 16 bits with the
     * two bytes the other way round, which is what pushImage reads and
     * what fillRect does not -- handed one straight, it painted a white
     * border the dark navy that 0x18C6 is when read the near way round.
     * The same swap the screenshot does on the way back off the panel. */
    const uint16_t raw = specpal565[colour & 0x07U];
    const uint16_t rgb = (uint16_t)((raw >> 8) | (raw << 8));

    const int right = _origin_x + kZxWidth;
    const int below = _origin_y + kZxHeight;

    if (_origin_y > 0) {
        display.fillRect(0, 0, display.width(), _origin_y, rgb);
        display.fillRect(0, below, display.width(), display.height() - below, rgb);
    }
    if (_origin_x > 0) {
        display.fillRect(0, _origin_y, _origin_x, kZxHeight, rgb);
        display.fillRect(right, _origin_y, display.width() - right, kZxHeight, rgb);
    }
}

/* Beeper: the emulator hands over one accumulator entry per scan line,
 * 312 of them per frame at 50 Hz. The speaker streams straight out of the
 * buffer it was handed rather than copying it, so frames alternate
 * between two of them and the one still playing is left alone. */
constexpr int kSamplesPerFrame = 312;
constexpr int kSampleRate = kSamplesPerFrame * 50;
constexpr int32_t kTStatesPerLine = 224;
int16_t _pcm[2][kSamplesPerFrame];
int _pcm_slot;

}  // namespace

bool renderer_begin()
{
    if (_band == nullptr) {
        _band = (uint16_t*)malloc((size_t)kZxWidth * kBandRows * sizeof(uint16_t));
    }
    if (_band == nullptr) {
        mclog::tagError(_tag, "band buffer allocation failed");
        return false;
    }

    if (_shadow == nullptr) {
        _shadow = (uint8_t*)malloc(kScreenBytes);
    }
    if (_shadow == nullptr) {
        mclog::tagError(_tag, "shadow screen allocation failed");
        free(_band);
        _band = nullptr;
        return false;
    }
    _force_redraw = true;
    _border_drawn = 0xff;

    /* Centred, so the border is the same width on both sides. A panel
     * smaller than the screen would give a negative origin; there is none
     * such here, and clamping to zero draws the top left corner of the
     * screen rather than nothing at all. */
    auto& display = GetHAL().display;
    _origin_x     = (display.width() - kZxWidth) / 2;
    _origin_y     = (display.height() - kZxHeight) / 2;
    if (_origin_x < 0) {
        _origin_x = 0;
    }
    if (_origin_y < 0) {
        _origin_y = 0;
    }

    display.fillScreen(TFT_BLACK);
    return true;
}

void renderer_end()
{
    free(_band);
    _band = nullptr;
    free(_shadow);
    _shadow = nullptr;
}


void renderer_draw(const uint8_t* screen, uint8_t border_colour)
{
    if (_band == nullptr || screen == nullptr) {
        return;
    }

    border_colour &= 0x07U;
    if (border_colour != _border_drawn) {
        _border_drawn = border_colour;
        paint_border(border_colour);
    }

    /* Flashing cells swap ink and paper twice a second, and every band
     * holding one has to be redrawn when they do. Which bands those are
     * is not worth tracking for something that happens every 16 frames:
     * the whole screen goes out instead. */
    bool redraw_all = _force_redraw;
    _force_redraw = false;
    if (++_flash_counter >= 16U) {
        _flash_counter = 0U;
        _flash_state = !_flash_state;
        redraw_all = true;
    }

    const uint8_t* bitmap = screen;
    const uint8_t* attrs = screen + 0x1800;
    const uint8_t* shadow_bitmap = _shadow;
    const uint8_t* shadow_attrs = _shadow + 0x1800;

    for (int band_y = 0; band_y < kZxHeight; band_y += kBandRows) {
        const int rows = (band_y + kBandRows > kZxHeight) ? (kZxHeight - band_y) : kBandRows;

        /* Nothing this band draws from has moved since it was last sent,
         * so neither the pixels nor the bus transfer are worth paying. */
        if (!redraw_all) {
            bool changed = false;
            for (int row = 0; row < rows && !changed; row++) {
                const int src_y = band_y + row;
                const int offset = ((src_y / 64) * 2048) + ((src_y % 8) * 256) + (((src_y % 64) / 8) * 32);
                const int attr_offset = (src_y / 8) * 32;

                changed = memcmp(bitmap + offset, shadow_bitmap + offset, 32) != 0 ||
                          memcmp(attrs + attr_offset, shadow_attrs + attr_offset, 32) != 0;
            }
            if (!changed) {
                continue;
            }
        }

        for (int row = 0; row < rows; row++) {
            const int src_y = band_y + row;

            /* The ZX screen is stored in thirds, then by pixel row within
             * a character, then by character row. */
            const int third = src_y / 64;
            const int line = src_y % 8;
            const int chunk = (src_y % 64) / 8;
            const uint8_t* bitmap_line = bitmap + (third * 2048) + (line * 256) + (chunk * 32);
            const uint8_t* attr_line = attrs + ((src_y / 8) * 32);

            /* Eight source pixels share one attribute, so it is unpacked
             * once per character cell rather than once per pixel: what is
             * left in the inner loop is a compare, a bit test and a
             * store. */
            uint16_t* out = _band + (row * kZxWidth);
            int current_char = -1;
            uint16_t ink_colour = 0;
            uint16_t paper_colour = 0;
            uint8_t bits = 0;

            for (int x = 0; x < kZxWidth; x++) {
                const int char_x = x >> 3;
                if (char_x != current_char) {
                    current_char = char_x;
                    bits = bitmap_line[char_x];

                    const uint8_t attr = attr_line[char_x];
                    uint8_t ink = attr & 0x07U;
                    uint8_t paper = (attr >> 3) & 0x07U;

                    if ((attr & 0x80U) && _flash_state) {
                        const uint8_t swap = ink;
                        ink = paper;
                        paper = swap;
                    }
                    if (attr & 0x40U) {
                        ink |= 0x08U;
                        paper |= 0x08U;
                    }
                    ink_colour = specpal565[ink];
                    paper_colour = specpal565[paper];
                }

                out[x] = (bits & (uint8_t)(0x80U >> (x & 7))) ? ink_colour : paper_colour;
            }
        }

        GetHAL().display.pushImage(_origin_x, _origin_y + band_y, kZxWidth, rows, _band);
    }

    memcpy(_shadow, screen, kScreenBytes);
}

SpecKeys map_char(char ch, bool* needs_symbol_shift)
{
    if (needs_symbol_shift != nullptr) {
        *needs_symbol_shift = false;
    }

    if (ch == '\n') {
        ch = '\r';
    }
    if (ch >= 'A' && ch <= 'Z') {
        ch = (char)(ch - 'A' + 'a');
    }

    /* Letters and digits sit next to each other in the enum, so they cost
     * nothing to work out. */
    if (ch >= '1' && ch <= '9') {
        return (SpecKeys)(SPECKEY_1 + (ch - '1'));
    }
    if (ch == '0') {
        return SPECKEY_0;
    }

    static const struct {
        char ch;
        uint8_t key;
        bool symbol;
    } kCharMap[] = {
        {'q', SPECKEY_Q, false}, {'w', SPECKEY_W, false}, {'e', SPECKEY_E, false},
        {'r', SPECKEY_R, false}, {'t', SPECKEY_T, false}, {'y', SPECKEY_Y, false},
        {'u', SPECKEY_U, false}, {'i', SPECKEY_I, false}, {'o', SPECKEY_O, false},
        {'p', SPECKEY_P, false}, {'a', SPECKEY_A, false}, {'s', SPECKEY_S, false},
        {'d', SPECKEY_D, false}, {'f', SPECKEY_F, false}, {'g', SPECKEY_G, false},
        {'h', SPECKEY_H, false}, {'j', SPECKEY_J, false}, {'k', SPECKEY_K, false},
        {'l', SPECKEY_L, false}, {'z', SPECKEY_Z, false}, {'x', SPECKEY_X, false},
        {'c', SPECKEY_C, false}, {'v', SPECKEY_V, false}, {'b', SPECKEY_B, false},
        {'n', SPECKEY_N, false}, {'m', SPECKEY_M, false},
        {' ', SPECKEY_SPACE, false}, {'\r', SPECKEY_ENTER, false},

        /* Everything the Spectrum only reaches with symbol shift held. */
        {'!', SPECKEY_1, true},  {'@', SPECKEY_2, true},  {'#', SPECKEY_3, true},
        {'$', SPECKEY_4, true},  {'%', SPECKEY_5, true},  {'&', SPECKEY_6, true},
        {'\'', SPECKEY_7, true}, {'(', SPECKEY_8, true},  {')', SPECKEY_9, true},
        {'_', SPECKEY_0, true},  {'<', SPECKEY_R, true},  {'>', SPECKEY_T, true},
        {';', SPECKEY_O, true},  {'"', SPECKEY_P, true},  {'-', SPECKEY_J, true},
        {'+', SPECKEY_K, true},  {'=', SPECKEY_L, true},  {':', SPECKEY_Z, true},
        {'?', SPECKEY_C, true},  {'/', SPECKEY_V, true},  {'*', SPECKEY_B, true},
        {',', SPECKEY_N, true},  {'.', SPECKEY_M, true},
    };

    for (const auto& entry : kCharMap) {
        if (entry.ch == ch) {
            if (needs_symbol_shift != nullptr) {
                *needs_symbol_shift = entry.symbol;
            }
            return (SpecKeys)entry.key;
        }
    }
    return SPECKEY_NONE;
}

bool beeper_begin()
{
    /* The speaker is already running -- the firmware starts it during HAL
     * init and every other application shares it -- so nothing here
     * reconfigures it. The frame rate is passed to playRaw() as the source
     * rate instead, and the speaker resamples. */
    _pcm_slot = 0;
    return GetHAL().speaker.isEnabled();
}

void beeper_end()
{
    GetHAL().speaker.stop();
}

void beeper_submit_frame(const uint16_t* accumulator)
{
    if (accumulator == nullptr) {
        return;
    }

    /* The speaker waits for a free slot rather than refusing one, so a
     * caller that runs ahead of playback gets held back to real time --
     * which is what made loading a tape take as long as the tape. A
     * frame that has nowhere to go is dropped instead. */
    if (GetHAL().speaker.isPlaying(0) >= 2) {
        return;
    }

    /* Each entry is how many of that scan line's t-states the beeper bit
     * was high, so it doubles as the line's amplitude. */
    int32_t lowest  = INT32_MAX;
    int32_t highest = 0;
    int32_t total   = 0;
    for (int i = 0; i < kSamplesPerFrame; i++) {
        int32_t level = (int32_t)accumulator[i];
        if (level > kTStatesPerLine) {
            level = kTStatesPerLine;
        }
        if (level < lowest) lowest = level;
        if (level > highest) highest = level;
        total += level;
    }

    /* A frame the beeper never moved in carries no sound, only a constant
     * level. Playing that would put a step at the start of every frame
     * and turn silence into a 50 Hz buzz, so it is simply not played. */
    if (highest - lowest < 4) {
        return;
    }

    /* Centred on the frame's own average, so a waveform sitting high or
     * low does not arrive as a jump from the previous frame. */
    const int32_t mean = total / kSamplesPerFrame;

    int16_t* pcm = _pcm[_pcm_slot];
    _pcm_slot ^= 1;

    for (int i = 0; i < kSamplesPerFrame; i++) {
        int32_t level = (int32_t)accumulator[i];
        if (level > kTStatesPerLine) {
            level = kTStatesPerLine;
        }
        pcm[i] = (int16_t)(((level - mean) * 24000) / kTStatesPerLine);
    }

    GetHAL().speaker.playRaw(pcm, kSamplesPerFrame, kSampleRate, false, 1, 0);
}

}  // namespace zx_host

/* -------------------------------------------------------------------------- */
/*                          What the core reaches out for                      */
/* -------------------------------------------------------------------------- */

/* The Z80 knows nothing about what it is plugged into and calls these four
 * for every memory and port access; userInfo is the machine. */
extern "C" {

byte Z80MemRead(uint16_t address, void* userInfo)
{
    return ((ZXSpectrum*)userInfo)->z80_peek(address);
}

void Z80MemWrite(uint16_t address, byte data, void* userInfo)
{
    ((ZXSpectrum*)userInfo)->z80_poke(address, data);
}

byte Z80InPort(uint16_t port, void* userInfo)
{
    return ((ZXSpectrum*)userInfo)->z80_in(port);
}

void Z80OutPort(uint16_t port, byte data, void* userInfo)
{
    ((ZXSpectrum*)userInfo)->z80_out(port, data);
}

}  // extern "C"

/* The tape loader plays the loading tone through the same beeper, and
 * scales it by the volume these two carry. */
int soundVolume  = 8;
bool soundEnabled = true;

void ZX_BeeperSubmitFrame(const uint16_t* accum312)
{
    zx_host::beeper_submit_frame(accum312);
}
