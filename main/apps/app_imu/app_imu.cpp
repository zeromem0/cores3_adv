/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_imu.h"
#include "assets/imu_big.h"
#include "assets/imu_small.h"
#include "assets/imu_panel.h"
#include <apps/utils/app_header/app_header.h>
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <assets.h>
#include <hal.h>

using namespace mooncake;

AppImu::AppImu()
{
    setAppInfo().name     = "IMU";
    setAppInfo().userData = new AppIcon_t(image_data_imu_big, image_data_imu_small);
}

AppImu::~AppImu()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppImu::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Clear screen
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    app_header::reset();

    GetHAL().imu.begin();
}

void AppImu::onRunning()
{
    auto imu_update = GetHAL().imu.update();
    if (imu_update) {
        // Obtain data on the current value of the IMU.
        _imu_data = GetHAL().imu.getImuData();

        // mclog::tagInfo(getAppInfo().name, "{:.1f} {:.1f} {:.1f}, {:.1f} {:.1f} {:.1f}", _imu_data.accel.x,
        //                _imu_data.accel.y, _imu_data.accel.z, _imu_data.gyro.x, _imu_data.gyro.y, _imu_data.gyro.z);

        update_panel_angle();

        // Render
        GetHAL().canvas.fillScreen(THEME_COLOR_BG);
        app_header::draw(GetHAL().canvas, "IMU");
        GetHAL().canvas.setFont(FONT_REPL);
        GetHAL().canvas.setTextSize(1);
        render_imu_data_label();
        render_imu_panel();
        GetHAL().pushCanvas();
    }

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
        return;
    }

    if (app_header::back_pressed(GetHAL().canvas.width(), GetHAL().canvasKeyboardBar.width(), 0)) {
        audio::play_random_tone();
        close();
    }
}

void AppImu::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

void AppImu::render_imu_data_label()
{
    /* The six readings start under the band and keep the spacing they
     * were written with. */
    const int top = app_header::height() + 6;

    GetHAL().canvas.setTextColor((uint32_t)0x8FC8AA);
    _str_buffer = fmt::format("AX: {: .1f}", _imu_data.accel.x);
    GetHAL().canvas.drawString(_str_buffer.c_str(), 6, top);
    _str_buffer = fmt::format("AY: {: .1f}", _imu_data.accel.y);
    GetHAL().canvas.drawString(_str_buffer.c_str(), 6, top + 16);
    _str_buffer = fmt::format("AZ: {: .1f}", _imu_data.accel.z);
    GetHAL().canvas.drawString(_str_buffer.c_str(), 6, top + 32);

    GetHAL().canvas.setTextColor((uint32_t)0x88AED9);
    _str_buffer = fmt::format("GX: {: .1f}", _imu_data.gyro.x);
    GetHAL().canvas.drawString(_str_buffer.c_str(), 6, top + 52);
    _str_buffer = fmt::format("GY: {: .1f}", _imu_data.gyro.y);
    GetHAL().canvas.drawString(_str_buffer.c_str(), 6, top + 68);
    _str_buffer = fmt::format("GZ: {: .1f}", _imu_data.gyro.z);
    GetHAL().canvas.drawString(_str_buffer.c_str(), 6, top + 84);

    _str_buffer.clear();
}

void AppImu::render_imu_panel()
{
    /*
     * In the middle of what is left under the band, rather than at the
     * 141,53 it was given on a 240x135 panel -- where it was the middle,
     * and here was neither. The readings down the left end at about
     * seventy columns, so a dial a hundred wide centred on 320 clears
     * them with room to spare.
     */
    const int panel_center_x = GetHAL().canvas.width() / 2;
    const int panel_center_y =
        app_header::height() + (GetHAL().canvas.height() - app_header::height()) / 2;

    // Panel
    int w = 100;
    int h = 100;
    GetHAL().canvas.pushImageRotateZoomWithAA(panel_center_x, panel_center_y, w / 2, h / 2, _panel_angle, 1.0f, 1.0f, w,
                                              h, image_data_imu_panel);

    // Tilt ball
    int ball_offset_x = std::clamp((int)(_imu_data.accel.x * 15), -15, 15);
    int ball_offset_y = std::clamp((int)(_imu_data.accel.y * 15), -15, 15);
    ball_offset_x     = -ball_offset_x;
    // Ball
    GetHAL().canvas.fillSmoothCircle(panel_center_x + ball_offset_x, panel_center_y + ball_offset_y, 16,
                                     (uint32_t)0x778595);
    // Ball cross mark
    GetHAL().canvas.fillRect(panel_center_x - 7 / 2 + ball_offset_x, panel_center_y + ball_offset_y, 7, 1, TFT_WHITE);
    GetHAL().canvas.fillRect(panel_center_x + ball_offset_x, panel_center_y - 7 / 2 + ball_offset_y, 1, 7, TFT_WHITE);

    // Cross mark
    GetHAL().canvas.fillRect(panel_center_x, panel_center_y - 52 / 2, 1, 52, TFT_WHITE);
    GetHAL().canvas.fillRect(panel_center_x - 52 / 2, panel_center_y, 52, 1, TFT_WHITE);
}

static void _calculate_attitude_yaw(float gyroZ, float deltaTime, float& yaw)
{
    yaw += gyroZ * deltaTime;
}

void AppImu::update_panel_angle()
{
    static uint32_t time_count = GetHAL().millis();
    static float yaw           = 0.0f;

    if (GetHAL().millis() - time_count > 200) {
        time_count = GetHAL().millis();
        yaw        = 0.0f;
        return;
    }

    _calculate_attitude_yaw(_imu_data.gyro.z, GetHAL().millis() - time_count, yaw);
    // mclog::tagInfo(getAppInfo().name, "get yaw: {}", yaw);

    _panel_angle = yaw / 1000;

    time_count = GetHAL().millis();
}
