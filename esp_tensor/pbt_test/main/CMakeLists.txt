idf_component_register(SRCS "test_main.cpp" "rpc_transport.cpp" "commands.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_psram esp_tensor esp_memory)
