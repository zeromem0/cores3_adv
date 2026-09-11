/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal.h>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mooncake.h>
#include <stdint.h>
#include <string>

/**
 * @brief
 *
 */
class Launcher : public mooncake::AppAbility {
private:
    struct KeyboardBarState_t {
        bool shift = false;
        bool fn    = false;
        bool ctrl  = false;
        bool opt   = false;
        bool alt   = false;

        void reset()
        {
            shift = false;
            fn    = false;
            ctrl  = false;
            opt   = false;
            alt   = false;
        }
    };

    struct Data_t {
        // Keyboard bar
        KeyboardBarState_t keyboard_state;

        char string_buffer[100];

        int running_app_id = -1;
    };
    Data_t _data;

    /* An application the remote page asked for, kept until the screen is
     * free to give it. */
    std::string _pending_open;

    void boot_anim();

    void start_menu();
    void update_menu(bool pushCanvas = true);

    void invalidate_menu();
    void start_keyboard_bar();
    void render_keyboard_bar();
    void handle_app_open(int index, int appId);

    /** @brief Open the application named in the settings, if there is one. */
    void start_startup_app();

    /** @brief Carry out whatever the remote page asked for. */
    void handle_remote_request();

    /** @brief A tap on the desktop, at the last place it was seen. */
    void handle_desktop_touch();
    bool _swallow_touch = false;
    int _touch_x        = 0;
    int _touch_y        = 0;

public:
    void onCreate() override;
    void onRunning() override;
};
