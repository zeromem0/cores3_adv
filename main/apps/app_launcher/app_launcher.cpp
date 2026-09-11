/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include "remote_launch.h"
#include <hal/utils/touch_keys/touch_keys.h>
#include <apps/utils/theme.h>
#include <apps/utils/common.h>
#include <mooncake_log.h>
#include <hal/hal.h>
#include <hal.h>

using namespace mooncake;

void Launcher::onCreate()
{
    setAppInfo().name = "Launcher";
    mclog::tagInfo(getAppInfo().name, "on create");

    // Init
    boot_anim();
    start_menu();
    // The keyboard bar is no longer on screen; its columns went to the
    // application canvas. Its drawing code stays, and starts working
    // again the moment its sprite is created.
    start_keyboard_bar();

    open();

    start_startup_app();
}

void Launcher::onRunning()
{
    // mclog::tagInfo(getAppInfo().name, "on running");

    /* An application that took the panel over also had the canvas taken
     * from under it, so when it gives the screen back it is empty and
     * everything has to be drawn again. */
    static bool was_full_screen = false;
    const bool full_screen      = GetHAL().isFullScreenApp();
    if (was_full_screen && !full_screen) {
        /* No blanking. The desktop covers the whole canvas and the canvas
         * covers the whole panel, so clearing the glass first only put a
         * black frame between the application and the desktop. */
        invalidate_menu();
    }
    was_full_screen = full_screen;

    // If app is opened and running
    if (_data.running_app_id >= 0) {
        // If running app is closed
        if (GetMooncake().getAppCurrentState(_data.running_app_id) == AppAbility::StateSleeping) {
            _data.running_app_id = -1;
            remote_launch::set_running("");
            /*
             * No closing animation.
             *
             * It drew a shrinking circle into the canvas and pushed it
             * every step, which worked while the menu was a carousel
             * redrawn on every frame. The desktop draws only when
             * something about it changed, so the circle ate the icons
             * and nothing was left marked as needing them back.
             */
            invalidate_menu();
        }
    } else {
        update_menu();
    }

    /*
     * After the check above, not before it.
     *
     * Asking for another application closes the one on screen and waits
     * for a later pass, and the launcher learns that it has finished by
     * finding it asleep. Run first, this closed it again on every pass:
     * the state went back to closing before the line that watches for
     * sleeping ever saw it, so the request was never taken up and the
     * application ran its onClose forty times a second for ever.
     */
    handle_remote_request();

    /*
     * The desktop owns the finger while it is up.
     *
     * touch_keys divides the panel into nine invisible cells and plays
     * the chord a Cardputer user would press; on a screen of icons those
     * cells would answer the same tap that opened an application. It is
     * suspended while the desktop is showing and resumed when something
     * else takes the screen. Done on the change only, so that an
     * application which suspends it for its own reasons is not undone
     * every frame.
     */
    static bool was_menu = false;
    const bool is_menu   = (_data.running_app_id < 0);
    if (is_menu != was_menu) {
        was_menu = is_menu;
        if (is_menu) {
            touch_keys::suspend();
            invalidate_menu();

            /*
             * Whatever finger is on the glass right now is the one that
             * just left an application, not one choosing the next.
             *
             * Escape comes from touch_keys on the press, so the sequence
             * was: press in the top left corner, application closes,
             * desktop appears, finger lifts -- and the lift landed on
             * the icon under it, which is the top left one, which is the
             * application just left. It reopened itself.
             */
            _swallow_touch = true;
        } else {
            touch_keys::resume();
        }
    }

    if (!is_menu) {
        return;
    }

    std::int32_t tx     = 0;
    std::int32_t ty     = 0;
    const bool touching = GetHAL().display.getTouch(&tx, &ty) > 0;

    /* On release, so a finger dragged off an icon does not open it. */
    static bool was_touching = false;
    if (_swallow_touch) {
        /* Held until the glass is clear again. */
        if (!touching) {
            _swallow_touch = false;
        }
        was_touching = touching;
        return;
    }
    if (was_touching && !touching) {
        handle_desktop_touch();
    }
    if (touching) {
        _touch_x = (int)tx;
        _touch_y = (int)ty;
    }
    was_touching = touching;
}
