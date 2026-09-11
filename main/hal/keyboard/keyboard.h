/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "keymap.h"
#include "../utils/adafruit_tca8418/Adafruit_TCA8418.h"
#include <mooncake_log_signal.h>

class Keyboard {
public:
    struct KeyEventRaw_t {
        bool state  = false;
        uint8_t row = 0;
        uint8_t col = 0;
    };

    struct KeyEvent_t {
        bool state              = false;
        bool isModifier         = false;
        KeScanCode_t keyCode    = KEY_NONE;
        const char* keyName     = "";
        uint8_t extraModifiers  = 0;  // HID modifier bits to OR into the report in addition to the
                                      // physical modifier keys (used when Fn injects LSHIFT for A-Z)
    };

    mclog::Signal<const KeyEventRaw_t&> onKeyEventRaw;
    mclog::Signal<const KeyEvent_t&> onKeyEvent;

    bool init();
    void update();

    /* The Cardputer's matrix scan, kept for reference; this board has
     * no matrix and never calls it. */
    void updateFromMatrix();
    inline uint8_t getModifierMask()
    {
        return _modifier_mask;
    }
    inline const KeyEvent_t& getLatestKeyEvent()
    {
        return _key_event_buffer;
    }
    inline const KeyEventRaw_t& getLatestKeyEventRaw()
    {
        return _key_event_raw_buffer;
    }
    void clearKeyEvent();
    KeyEvent_t convertToKeyEvent(const KeyEventRaw_t& key);

    /**
     * @brief Feed a key event as if the matrix had produced it.
     *
     * Coordinates are post-remap, the same ones onKeyEventRaw reports.
     * Emits both signals and keeps the modifier mask in step, so callers
     * cannot tell an injected key from a pressed one. Must be called from
     * the task that owns the display, since app handlers draw.
     */
    void injectKeyEventRaw(const KeyEventRaw_t& key);

    /**
     * @brief Find where a printable character sits on the matrix.
     *
     * @param needsShift set when the character is the shifted legend of
     *                   the key, so the caller can wrap it in shift
     * @return false when no key carries the character
     */
    bool findKeyPosition(char value, uint8_t& row, uint8_t& col, bool& needsShift);

    /**
     * @brief Where a key code sits on the matrix.
     *
     * For input that arrives as a code rather than as a position.
     * Turning codes back into positions is what lets such a source
     * reach every application, including the ones that read the
     * matrix directly.
     *
     * @param needsShift set when the code is the key's shifted legend
     * @param needsFn    set when it is the Fn one
     * @return false when no key carries the code
     */
    bool findPositionByKeyCode(uint8_t keyCode, uint8_t& row, uint8_t& col, bool& needsShift,
                               bool& needsFn);

    /**
     * @brief What is printed on a key, for something that draws one.
     *
     * @param fn     the Fn layer is selected
     * @param shift  the shifted legend is wanted, when Fn is not
     */
    const char* legendAt(uint8_t row, uint8_t col, bool fn, bool shift) const;

    /** @brief Whether that position is a modifier rather than a character. */
    bool isModifierAt(uint8_t row, uint8_t col) const;

private:
    Adafruit_TCA8418* _tca8418 = nullptr;
    uint8_t _modifier_mask     = 0;
    bool _fn_state             = false;
    KeyEventRaw_t _key_event_raw_buffer;
    KeyEvent_t _key_event_buffer;

    KeyEventRaw_t get_key_event_raw(const uint8_t& eventRaw);
    void remap(KeyEventRaw_t& key);
    void update_modifier_mask(const KeyEventRaw_t& key);
};
