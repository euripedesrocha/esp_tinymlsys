set(test_dependencies)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND test_dependencies esp_driver_gptimer)
endif()
if(NOT IDF_TARGET STREQUAL "linux")
    list(APPEND test_dependencies esp_timer)
endif()

idf_component_register(
    SRCS
        "test_main.cpp"
        "test_freertos_chrono.cpp"
        "test_freertos_semaphore.cpp"
        "test_freertos_event_group.cpp"
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
    PRIV_REQUIRES
        console
        esp_freertos_cpp
        esp_freertos_cpp_test_support
        freertos
        ${test_dependencies}
)
