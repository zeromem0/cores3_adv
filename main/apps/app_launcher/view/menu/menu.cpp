/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "menu.h"
#include "../../app_launcher.h"
#include "../../remote_launch.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/theme.h>
#include <apps/utils/common.h>
#include <mooncake_log.h>
#include <mooncake.h>
#include <hal/hal.h>
#include <string>

using namespace mooncake;
using namespace smooth_ui_toolkit;

static const std::string _tag = "LauncherMenu";

void LauncherMenu::init(int launcherAppId)
{
    mclog::tagInfo(_tag, "init");

    /*
     * The desktop, in the order it is read: five to a row, so the first
     * five are the top row and the next five the one under it.
     *
     * The order applications happen to be installed in is the order they
     * were written over months, which is history rather than use. On a
     * carousel that only decided what came next; on a grid it decides
     * where things live, and a place is worth setting down by hand.
     * Anything installed but not named here is put after the list rather
     * than dropped, so an application appears on the desktop the day it
     * is written and is given its place afterwards.
     */
    static const char* const kDesktop[] = {
        "SetWiFi", "Clock",   "TaskMan", "SDCard", "Jobs",
        "Dhex",    "Aprecio", "Irriga",  "zx",     "Wilma",
        "About",   "Bclock",  "IMU",     "Record",
    };

    auto installed_apps = GetMooncake().getAppAbilityManager()->getAllAbilityInstance();
    std::vector<AppAbility*> ordered;

    for (const char* wanted : kDesktop) {
        for (auto& app_raw : installed_apps) {
            auto app = static_cast<AppAbility*>(app_raw);
            if (app->getId() == launcherAppId || app->getAppInfo().name != wanted) {
                continue;
            }
            ordered.push_back(app);
            break;
        }
    }

    for (auto& app_raw : installed_apps) {
        auto app = static_cast<AppAbility*>(app_raw);
        if (app->getId() == launcherAppId) {
            continue;
        }
        bool already = false;
        for (auto* placed : ordered) {
            if (placed->getId() == app->getId()) {
                already = true;
                break;
            }
        }
        if (!already) {
            mclog::tagInfo(_tag, "{} has no place on the desktop, appended", app->getAppInfo().name);
            ordered.push_back(app);
        }
    }

    int i = 0;
    for (auto* app : ordered) {
        mclog::tagInfo(_tag, "desktop: {} id: {}", app->getAppInfo().name, app->getId());
        _option_infos.push_back({app->getId(), app->getAppInfo().name});
        addOption({Vector4i{ICON_GAP + i * (ICON_WIDTH + ICON_GAP), ICON_MARGIN_TOP, ICON_WIDTH, ICON_WIDTH},
                   app->getAppInfo().userData});
        i++;
    }

    // Setup animation
    getSelectorPostion().x.springOptions().visualDuration = 0.4;
    getSelectorShape().x.springOptions().visualDuration   = 0.4;
    getSelectorShape().y.springOptions().visualDuration   = 0.4;

    moveTo(0);

    // Readinput in every update
    setConfig().readInputInterval = 0;

    _startup_app = GetHAL().getSettings().GetString("startup_app", "");
    if (!_startup_app.empty()) {
        mclog::tagInfo(_tag, "startup app: {}", _startup_app);
    }
}

/*
 * The geometry of the desktop, in one place, because the drawing and the
 * hit test have to agree exactly or an icon opens its neighbour.
 *
 * It is laid out from the canvas rather than from the panel. With no
 * status bar above it the canvas is the whole 320x240 -- but with no
 * CardKB plugged in the on-screen keyboard takes the bottom of it, and
 * the grid then has fewer rows and scrolls rather than being drawn off
 * the edge.
 */
namespace {

/* Wide enough for the 56 pixel icon and the longest name under it. */
constexpr int kIconSize = 56;
constexpr int kCellW    = 64;
/* The icon, the margins around it, and a 16 pixel line of text. */
constexpr int kCellMinH = 72;

int cell_w()
{
    return GetHAL().canvas.width() / LauncherMenu::columns();
}

int cell_h()
{
    return GetHAL().canvas.height() / LauncherMenu::visibleRows();
}

}  // namespace

int LauncherMenu::columns()
{
    const int cols = GetHAL().canvas.width() / kCellW;
    return cols > 0 ? cols : 1;
}

int LauncherMenu::visibleRows()
{
    const int rows = GetHAL().canvas.height() / kCellMinH;
    return rows > 0 ? rows : 1;
}

/* Keep the selection on screen after it has been moved. */
void LauncherMenu::ensureVisible()
{
    const int row = getSelectedOptionIndex() / columns();
    if (row < _top_row) {
        _top_row = row;
    } else if (row >= _top_row + visibleRows()) {
        _top_row = row - visibleRows() + 1;
    }
}

bool LauncherMenu::handleTouch(int x, int y)
{
    const int col = x / cell_w();
    const int row = y / cell_h() + _top_row;
    if (col < 0 || col >= columns() || row < 0) {
        return false;
    }

    const int index = row * columns() + col;
    if (index < 0 || index >= (int)_option_infos.size()) {
        return false;
    }

    /* Selected first, then opened, so the icon is seen to be chosen even
     * though the application takes the screen a moment later. */
    moveTo(index);
    ensureVisible();
    renderNow();
    onClick();
    return true;
}

void LauncherMenu::onReadInput()
{
    auto event = GetHAL().keyboard.getLatestKeyEventRaw();
    if (event.state != true) {
        return;
    }

    const int count = (int)_option_infos.size();
    if (count == 0) {
        return;
    }
    const int index = getSelectedOptionIndex();

    /*
     * Up and down move by a row rather than by one, which is the whole
     * point of a grid: on the carousel they were another way of saying
     * left and right.
     */
    if (event.row == 3 && event.col == 10) {
        moveTo((index + count - 1) % count);
    } else if (event.row == 3 && event.col == 12) {
        moveTo((index + 1) % count);
    } else if (event.row == 2 && event.col == 11) {
        moveTo(index - columns() >= 0 ? index - columns() : index);
    } else if (event.row == 3 && event.col == 11) {
        moveTo(index + columns() < count ? index + columns() : index);
    } else if (event.row == 2 && event.col == 13) {
        onClick();
        return;
    }
    // Q -- toggle quiet mode
    else if (event.row == 1 && event.col == 1) {
        bool new_quiet = !audio::is_quiet_mode();
        audio::set_quiet_mode(new_quiet);
        GetHAL().getSettings().SetBool("quiet_mode", new_quiet);
    }
    // S -- make the selected application the one that opens at power-up,
    // or take that away from it if it already is.
    else if (event.row == 2 && event.col == 3) {
        const std::string name = selectedAppName();
        if (!name.empty()) {
            setStartupApp(name == _startup_app ? "" : name);
        }
    } else {
        return;
    }

    ensureVisible();
    invalidate();
}

void LauncherMenu::drawGrid()
{
    auto& canvas = GetHAL().canvas;

    const int count = (int)_option_infos.size();
    const int cols  = columns();
    const int cw    = cell_w();
    const int ch    = cell_h();
    const int rows  = visibleRows();

    canvas.fillScreen(THEME_COLOR_BG);
    canvas.setFont(FONT_BASIC);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);

    for (int r = 0; r < rows; r++) {
        for (int col = 0; col < cols; col++) {
            const int index = (_top_row + r) * cols + col;
            if (index < 0 || index >= count) {
                continue;
            }

            const bool selected = (index == getSelectedOptionIndex());
            const int ix        = col * cw + (cw - kIconSize) / 2;
            const int iy        = r * ch + 3;

            /* The face under every icon is the same. What marks the
             * selection is a ring drawn inside the bounds of the tile:
             * one drawn around it would abut the neighbouring cell, the
             * cells being exactly a tile and its margins wide. */
            canvas.fillSmoothRoundRect(ix - 3, iy - 3, kIconSize + 6, kIconSize + 6, 10, THEME_COLOR_ICON);
            if (selected) {
                canvas.drawRoundRect(ix - 3, iy - 3, kIconSize + 6, kIconSize + 6, 10, THEME_COLOR_SELECTED);
                canvas.drawRoundRect(ix - 2, iy - 2, kIconSize + 4, kIconSize + 4, 9, THEME_COLOR_SELECTED);
            }

            auto* icon = (AppIcon_t*)(getOptionList()[index].userData);
            if (icon != nullptr) {
                canvas.pushImage(ix, iy, kIconSize, kIconSize, icon->iconBig);
            }

            /* A dot in front of the name marks the application the board
             * opens by itself. */
            std::string label = _option_infos[index].name;
            if (!_startup_app.empty() && label == _startup_app) {
                label.insert(0, "\xE2\x97\x8F");
            }

            canvas.setTextColor(selected ? THEME_COLOR_SELECTED : THEME_COLOR_ICON, THEME_COLOR_BG);
            canvas.drawString(label.c_str(), col * cw + cw / 2, iy + kIconSize + 4);
        }
    }

    canvas.setTextDatum(top_left);
}

void LauncherMenu::renderNow()
{
    _grid_dirty = false;
    drawGrid();
    if (pushCanvas) {
        GetHAL().pushCanvas();
    }
}

void LauncherMenu::onRender()
{
    /* Drawn only when something moved: see the note on _grid_dirty. */
    if (!_grid_dirty) {
        return;
    }
    renderNow();
}

int LauncherMenu::appIdByName(const std::string& name) const
{
    for (const auto& info : _option_infos) {
        if (info.name == name) {
            return info.appId;
        }
    }
    return -1;
}

std::string LauncherMenu::appNameById(int appId) const
{
    for (const auto& info : _option_infos) {
        if (info.appId == appId) {
            return info.name;
        }
    }
    return "";
}

std::vector<std::string> LauncherMenu::appNames() const
{
    std::vector<std::string> names;
    names.reserve(_option_infos.size());
    for (const auto& info : _option_infos) {
        names.push_back(info.name);
    }
    return names;
}

std::string LauncherMenu::selectedAppName()
{
    const int index = getSelectedOptionIndex();
    if (index < 0 || index >= (int)_option_infos.size()) {
        return "";
    }
    return _option_infos[index].name;
}

void LauncherMenu::setStartupApp(const std::string& name)
{
    _startup_app = name;
    GetHAL().getSettings().SetString("startup_app", name);
    remote_launch::publish(appNames(), _startup_app);
    if (name.empty()) {
        mclog::tagInfo(_tag, "startup app cleared");
    } else {
        mclog::tagInfo(_tag, "startup app set to {}", name);
    }
    invalidate();
}

void LauncherMenu::onClick()
{
    if (onAppOpen) {
        const int index = getSelectedOptionIndex();
        if (index >= 0 && index < (int)_option_infos.size()) {
            onAppOpen(index, _option_infos[index].appId);
        }
    }
}

static LauncherMenu* _launcher_menu = nullptr;

void Launcher::start_menu()
{
    _launcher_menu = new LauncherMenu();
    _launcher_menu->init(getId());
    _launcher_menu->onAppOpen = [this](int index, int appId) { handle_app_open(index, appId); };

    remote_launch::init();
    remote_launch::publish(_launcher_menu->appNames(), _launcher_menu->startupApp());
}

void Launcher::update_menu(bool pushCanvas)
{
    _launcher_menu->pushCanvas = pushCanvas;
    _launcher_menu->update();
}

/*
 * Ask for the desktop to be drawn again even though nothing about it
 * changed. It renders only when it thinks it has to, which is right
 * until the canvas underneath it is thrown away and rebuilt empty.
 */
void Launcher::invalidate_menu()
{
    _launcher_menu->invalidate();
}

void Launcher::handle_desktop_touch()
{
    _launcher_menu->handleTouch(_touch_x, _touch_y);
}

void Launcher::handle_app_open(int index, int appId)
{
    // mclog::tagInfo(_tag, "handle app open: index: {} appId: {} running app id: {}", index, appId,
    // _data.running_app_id);

    GetMooncake().openApp(appId);
    _data.running_app_id = appId;
    remote_launch::set_running(_launcher_menu->appNameById(appId));

    /* No opening animation: it drew a circle growing over the canvas and
     * pushed it fifteen times, which on a 320x240 sprite is fifteen full
     * screen copies to say nothing the application is not about to say
     * for itself a moment later. */
}

/*
 * The application the board opens by itself, if one was chosen.
 *
 * Held down keys mean "not this time". Without that way out, an
 * application that hangs the moment it opens would take the board with
 * it: the launcher never gets the screen back, and the setting could
 * only be cleared over the network the application may never have let
 * start.
 */
void Launcher::start_startup_app()
{
    const std::string name = _launcher_menu->startupApp();
    if (name.empty()) {
        return;
    }

    const uint32_t deadline = GetHAL().millis() + 500;
    while (GetHAL().millis() < deadline) {
        GetHAL().update();
        if (GetHAL().keyboard.getLatestKeyEventRaw().state) {
            mclog::tagInfo(_tag, "key held, not opening {}", name);
            return;
        }
        GetHAL().delay(5);
    }

    const int app_id = _launcher_menu->appIdByName(name);
    if (app_id < 0) {
        mclog::tagWarn(_tag, "startup app \"{}\" is not installed", name);
        return;
    }

    mclog::tagInfo(_tag, "opening startup app {}", name);
    handle_app_open(-1, app_id);
}

void Launcher::handle_remote_request()
{
    const auto request = remote_launch::take();

    if (request.action == remote_launch::Action::Open) {
        /* Asking for what is already on screen does nothing. Taking the
         * request at face value meant closing the application and
         * opening it again, and the launcher showing through the gap --
         * which reads as the board having crashed rather than as having
         * obeyed. */
        if (_data.running_app_id >= 0 &&
            _launcher_menu->appNameById(_data.running_app_id) == request.name) {
            mclog::tagInfo(_tag, "{} is already open", request.name);
        } else {
            _pending_open = request.name;
        }
    } else if (request.action == remote_launch::Action::Close) {
        _pending_open.clear();
        if (_data.running_app_id >= 0) {
            GetMooncake().closeApp(_data.running_app_id);
        }
    }

    if (_pending_open.empty()) {
        return;
    }

    /* Whatever is on screen goes first, and the open waits for a later
     * pass: the launcher learns an application has finished by finding
     * it asleep, which cannot happen inside this call. */
    if (_data.running_app_id >= 0) {
        GetMooncake().closeApp(_data.running_app_id);
        return;
    }

    const int app_id = _launcher_menu->appIdByName(_pending_open);
    if (app_id < 0) {
        mclog::tagWarn(_tag, "no application named \"{}\"", _pending_open);
        _pending_open.clear();
        return;
    }

    mclog::tagInfo(_tag, "opening {} on request", _pending_open);
    _pending_open.clear();
    handle_app_open(-1, app_id);
}
