/*
 * ZX Spectrum 48K emulator.
 *
 * The Z80 and Spectrum sources under zx/ come from
 * github.com/AndyAiCardputer/zx-spectrum-cardputer-adv (MIT) and are
 * carried in unmodified; zx_host.cpp binds them to this firmware, and
 * this wrapper drives their lifecycle from mooncake's.
 *
 * Takes the whole panel while it runs, rather than the application
 * canvas: the ZX screen is 256x192 and every pixel of the glass counts.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>
#include <string>
#include <vector>

class ZXSpectrum;

class AppZX : public mooncake::AppAbility {
public:
    AppZX();
    ~AppZX();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum State_t {
        STATE_BROWSER,
        STATE_RUNNING,
    };

    /* One line of the list. The bare machine and the tape built into the
     * firmware sit alongside whatever the card holds, so the list reads
     * the same whether or not a card is in. */
    struct Entry_t {
        std::string label;
        std::string path;      // Relative to the mount point; empty if not a file
        bool builtin = false;  // The tape carried in flash
    };

    ZXSpectrum* _spectrum = nullptr;
    std::uint8_t _state   = STATE_BROWSER;

    std::vector<Entry_t> _entries;
    int _selected = 0;
    int _scroll   = 0;

    bool _audio_ready = false;
    std::uint32_t _next_frame_ms = 0;

    int _key_slot         = -1;
    int _key_raw_slot     = -1;
    bool _fn_held         = false;
    bool _close_requested = false;

    /* Why the last attempt to start did not, shown on the list. */
    const char* _last_error = "load failed";

    /* Which Spectrum key each arrow-marked key is currently holding down,
     * so a release lets go of exactly what the press took. */
    std::uint8_t _arrow_key[4] = {};

    void build_list();
    void draw_browser();
    /**
     * @brief Start the machine, optionally with the tape short-circuited.
     *
     * @param instant hand the blocks to the ROM's loader whole instead of
     *                emulating the tape they would have come off
     */
    bool start_machine(const Entry_t& entry, bool instant);

    void handle_browser_key(std::uint8_t ch);
};
