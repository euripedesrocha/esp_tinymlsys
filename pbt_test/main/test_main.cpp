/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "commands.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_log_level.h"
#include "rpc_transport.hpp"

#include "driver/usb_serial_jtag.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "pbt_test RPC transport requires USB Serial/JTAG console"
#endif

namespace {
constexpr auto* tag = "pbt_rpc";
}

extern "C" void app_main(void) {
    ESP_EARLY_LOGI(tag, "installing USB Serial/JTAG RPC driver");
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
        .tx_buffer_size = rpc::usb_serial_tx_buffer_size,
        .rx_buffer_size = rpc::usb_serial_rx_buffer_size,
    };
    const auto install_result = usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    ESP_EARLY_LOGI(tag, "USB Serial/JTAG RPC driver install result: %s",
                   esp_err_to_name(install_result));
    ESP_ERROR_CHECK(install_result);

    ESP_EARLY_LOGI(tag, "entering binary RPC server");
    esp_log_level_set("*", ESP_LOG_NONE);
    rpc::run_server(get_commands());
}
