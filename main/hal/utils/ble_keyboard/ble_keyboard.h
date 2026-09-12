/*
 * A keyboard over the air.
 *
 * The firmware already pretends to be one -- the BLE HID device role it
 * inherited from the Cardputer -- and this is the other direction: the
 * board as the host, so a real Bluetooth keyboard can drive it.
 *
 * It arrives as HID usages rather than as matrix positions, which sounds
 * like a problem and is not: this firmware's key map is written in HID
 * usages already, so a usage can be looked up and injected as the
 * position that carries it. Everything downstream -- the applications
 * that read key codes, the ones that read raw positions, the modifier
 * mask -- then sees something it cannot tell from a finger on the matrix.
 *
 * A job rather than something started at boot. The radio costs internal
 * RAM, which is the scarce thing on this board, so it is spent only when
 * asked for; and the choice survives a reboot, like every other job.
 */
#pragma once

namespace ble_keyboard {

/** @brief Add the keyboard host to the job list. Does not start it. */
void register_ble_keyboard_job();

}  // namespace ble_keyboard
