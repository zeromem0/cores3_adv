/*
 * UART service.
 *
 * On the application's usual host this declares the shared console
 * UART service. Ported
 * applications drive their serial port through the ESP-IDF UART driver
 * directly, and only use the accepted baud rate range from here.
 *
 * Values match DHEX_BUS_UART_{MIN,MAX}_BAUD_RATE upstream.
 */
#pragma once

#define DHEX_UART_MIN_BAUD_RATE 300U
#define DHEX_UART_MAX_BAUD_RATE 921600U
