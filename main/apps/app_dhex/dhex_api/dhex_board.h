/*
 * Board macros the application expects, filled in for the
 * Cardputer ADV.
 *
 * The API names the general-purpose serial port a device exposes to
 * applications PM_UART (the aqm particulate-matter sensor app was the
 * first consumer). Here it maps onto the Grove port, already wired as
 * UART2 on GPIO13/GPIO15 by the GPS helper -- see hal/hal_config.h.
 * These are the never-configured-yet defaults only; the settings screen
 * overrides them and persists the result in NVS.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

#include "hal_config.h"

#define DHEX_BOARD_PM_UART_PORT UART_NUM_2
#define DHEX_BOARD_PIN_PM_UART_TX HAL_PIN_GPS_TX
#define DHEX_BOARD_PIN_PM_UART_RX HAL_PIN_GPS_RX
