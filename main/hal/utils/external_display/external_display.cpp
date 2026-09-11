/*
 * SPDX-License-Identifier: MIT
 *
 * Panel settings taken from
 * github.com/AndyAiCardputer/zx-spectrum-cardputer-ili9341.
 */
#include "external_display.h"

#include <driver/gpio.h>
#include <hal/hal.h>
#include <mooncake_log.h>

#include <lgfx/v1/panel/Panel_LCD.hpp>

namespace external_display {

namespace {

const std::string _tag = "ext_display";

/* The header pins this panel is wired to. The SPI lines are the ones the
 * card is already on, which is why the bus is marked shared. */
constexpr int kPinSck = 40;
constexpr int kPinMosi = 14;
constexpr int kPinCs = 5;
constexpr int kPinDc = 6;
constexpr int kPinRst = 3;

/*
 * This build of M5GFX carries no ILI9341 panel class -- it ships the
 * ILI9342 that M5Stack's own devices use -- so the controller's start-up
 * sequence is written out here, as the reference project had to do.
 */
struct Panel_ILI9341 : public lgfx::v1::Panel_LCD {
    Panel_ILI9341()
    {
        _cfg.memory_width = _cfg.panel_width = 240;
        _cfg.memory_height = _cfg.panel_height = 320;
    }

protected:
    static constexpr uint8_t CMD_PWCTR1  = 0xC0;
    static constexpr uint8_t CMD_PWCTR2  = 0xC1;
    static constexpr uint8_t CMD_VMCTR1  = 0xC5;
    static constexpr uint8_t CMD_VMCTR2  = 0xC7;
    static constexpr uint8_t CMD_FRMCTR1 = 0xB1;
    static constexpr uint8_t CMD_DFUNCTR = 0xB6;
    static constexpr uint8_t CMD_GMCTRP1 = 0xE0;
    static constexpr uint8_t CMD_GMCTRN1 = 0xE1;
    static constexpr uint8_t CMD_PIXFMT  = 0x3A;

    const uint8_t* getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t list0[] = {
            CMD_PWCTR1,  1, 0x23,
            CMD_PWCTR2,  1, 0x10,
            CMD_VMCTR1,  2, 0x3E, 0x28,
            CMD_VMCTR2,  1, 0x86,
            CMD_PIXFMT,  1, 0x55,        // 16 bit, RGB565
            CMD_FRMCTR1, 2, 0x00, 0x18,  // 79 Hz
            CMD_DFUNCTR, 3, 0x08, 0x82, 0x27,
            CMD_GMCTRP1, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03,
            0x0E, 0x09, 0x00,
            CMD_GMCTRN1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C,
            0x31, 0x36, 0x0F,
            CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
            CMD_IDMOFF, 0,
            CMD_DISPON, 0 + CMD_INIT_DELAY, 100,
            0xFF, 0xFF,
        };
        return (listno == 0) ? list0 : nullptr;
    }
};

class Panel : public lgfx::LGFX_Device {
public:
    Panel()
    {
        auto bus_cfg = _bus.config();
        bus_cfg.spi_host   = SPI3_HOST;  // The host the card is on.
        bus_cfg.spi_mode   = 0;
        bus_cfg.freq_write = 40000000;
        bus_cfg.freq_read  = 16000000;
        bus_cfg.spi_3wire  = true;
        bus_cfg.use_lock   = true;
        bus_cfg.dma_channel = SPI_DMA_CH_AUTO;
        bus_cfg.pin_sclk   = kPinSck;
        bus_cfg.pin_mosi   = kPinMosi;
        bus_cfg.pin_miso   = -1;
        bus_cfg.pin_dc     = kPinDc;
        _bus.config(bus_cfg);
        _lcd.setBus(&_bus);

        auto panel_cfg = _lcd.config();
        panel_cfg.pin_cs   = kPinCs;
        panel_cfg.pin_rst  = kPinRst;
        panel_cfg.pin_busy = -1;
        /* Shared with the card, so every transfer takes the bus rather
         * than assuming it still holds it. */
        panel_cfg.bus_shared = true;
        panel_cfg.readable   = false;
        panel_cfg.invert     = false;
        panel_cfg.rgb_order  = false;
        panel_cfg.dlen_16bit = false;
        panel_cfg.memory_width  = 240;
        panel_cfg.memory_height = 320;
        panel_cfg.panel_width   = 240;
        panel_cfg.panel_height  = 320;
        panel_cfg.offset_x = 0;
        panel_cfg.offset_y = 0;
        panel_cfg.offset_rotation = 4;
        _lcd.config(panel_cfg);

        setPanel(&_lcd);
    }

private:
    Panel_ILI9341 _lcd;
    lgfx::Bus_SPI _bus;
};

Panel* _panel;
bool _present;

/*
 * Whether a panel is there at all.
 *
 * It cannot be read back, so it cannot be asked. What can be told apart
 * is a floating reset line from one tied to a panel: with the pin pulled
 * up and then down, a connected panel loads the line and the level
 * follows what is driven, while an open pin floats. This is weak
 * evidence, so it is only used to decide whether to keep the panel, and
 * a panel wrongly missed costs nothing but the external screen.
 */
bool probe()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask  = 1ULL << kPinRst;
    cfg.mode          = GPIO_MODE_INPUT;
    cfg.pull_up_en    = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(2));
    const int pulled_up = gpio_get_level((gpio_num_t)kPinRst);

    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(2));
    const int pulled_down = gpio_get_level((gpio_num_t)kPinRst);

    gpio_reset_pin((gpio_num_t)kPinRst);

    /* An open pin follows both pulls; a pin tied to a panel's reset
     * input does too, so this cannot tell them apart on its own. What it
     * does catch is a pin held at one level by something attached. */
    return !(pulled_up == 1 && pulled_down == 0);
}

}  // namespace

bool init()
{
    if (_present) {
        return true;
    }

    if (!probe()) {
        mclog::tagInfo(_tag, "no external panel");
        return false;
    }

    _panel = new Panel();
    if (!_panel->init()) {
        mclog::tagWarn(_tag, "external panel did not start");
        delete _panel;
        _panel = nullptr;
        return false;
    }

    _panel->setRotation(3);  // Landscape, 320x240.
    _panel->fillScreen(TFT_BLACK);
    _present = true;

    mclog::tagInfo(_tag, "external panel ready, {}x{}", _panel->width(), _panel->height());
    return true;
}

bool is_present()
{
    return _present;
}

lgfx::LGFX_Device& screen()
{
    if (_present && _panel != nullptr) {
        return *_panel;
    }
    return GetHAL().display;
}

lgfx::LGFX_Device* external()
{
    return _present ? _panel : nullptr;
}

}  // namespace external_display
