set(hardware_sources)
set(hardware_dependencies)
if(CONFIG_SOC_GPTIMER_SUPPORTED AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND hardware_sources "test_idf_gptimer.cpp")
    list(APPEND hardware_sources "test_idf_gptimer_context.cpp")
    list(APPEND hardware_sources "test_idf_gptimer_join.cpp")
endif()
if(NOT IDF_TARGET STREQUAL "linux")
    list(APPEND hardware_dependencies esp_timer)
endif()
if(IDF_TARGET STREQUAL "esp32s3" AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND hardware_sources "test_idf_gptimer_wait.cpp")
    list(APPEND hardware_sources "test_interrupt_handoff.cpp")
    list(APPEND hardware_dependencies esp_driver_gptimer esp_timer esp_system)
endif()
idf_component_register(
    SRCS
        "test_main.cpp"
        "test_execution.cpp"
        "test_value_types.cpp"
        "test_control_flow.cpp"
        "test_repeat.cpp"
        "test_repeat_n.cpp"
        "test_repeat_until.cpp"
        "test_let_value.cpp"
        "test_scheduler.cpp"
        "test_continues_on.cpp"
        "test_freertos_wait.cpp"
        "test_freertos_task_context.cpp"
        "test_freertos_task_context_integration.cpp"
        ${hardware_sources}
    INCLUDE_DIRS
        "."
    WHOLE_ARCHIVE
    PRIV_REQUIRES
        console
        esp_execution
        esp_freertos_cpp
        esp_execution_test_support
        freertos
        ${hardware_dependencies}
)
