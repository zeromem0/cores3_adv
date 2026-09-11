/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <smooth_ui_toolkit.h>
#include <apps/utils/common.h>
#include <functional>

class LauncherMenu : public smooth_ui_toolkit::SmoothSelectorMenu {
public:
    struct OptionInfo_t {
        int appId = -1;
        std::string name;
    };

    std::function<void(int index, int appId)> onAppOpen;
    bool pushCanvas = true;

    void init(int launcherAppId);
    void onReadInput() override;
    void onRender() override;

    /** @brief The installed application id carrying this name, or -1. */
    int appIdByName(const std::string& name) const;

    /** @brief The name of an installed application id, "" if unknown. */
    std::string appNameById(int appId) const;

    /** @brief Every application on the desktop, in the order it is shown. */
    std::vector<std::string> appNames() const;

    /** @brief The name under the selection right now. */
    std::string selectedAppName();

    /*
     * Which application the board opens by itself at power-up, "" for
     * none. Held here as well as in the settings because the desktop
     * marks it, and reading NVS once a frame to draw a dot would be
     * silly.
     */
    void setStartupApp(const std::string& name);
    inline const std::string& startupApp() const
    {
        return _startup_app;
    }

    /** @brief Draw again on the next update, whether or not anything moved. */
    inline void invalidate()
    {
        _data.is_changed = true;
        _grid_dirty      = true;
    }
    void onClick() override;

    /** @brief Take a tap on the desktop. True when it landed on an icon. */
    bool handleTouch(int x, int y);

    /** @brief How many icons fit across, and how many rows of them fit down. */
    static int columns();
    static int visibleRows();

private:
    std::vector<OptionInfo_t> _option_infos;
    std::string _startup_app;

    /*
     * A page of icons changes only when something is chosen, which is a
     * great deal rarer than once a frame -- and repainting a 320x240
     * sprite and pushing it over SPI for nothing is the most expensive
     * thing the launcher could do.
     */
    bool _grid_dirty = true;
    int _top_row     = 0;

    void ensureVisible();
    void drawGrid();
    void renderNow();
};
