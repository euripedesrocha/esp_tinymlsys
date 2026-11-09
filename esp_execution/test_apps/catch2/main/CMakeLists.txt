set(portable_sources
    "test_execution.cpp"
    "test_exception_handling.cpp"
    "test_value_types.cpp"
    "test_control_flow.cpp"
    "test_repeat.cpp"
    "test_repeat_n.cpp"
    "test_repeat_until.cpp"
    "test_let_value.cpp"
    "test_scheduler.cpp"
    "test_continues_on.cpp"
    "test_nonthrowing.cpp"
    "test_io_concepts.cpp"
    "test_io_primitives.cpp"
    "test_io_environment.cpp"
    "test_io_operation_environment.cpp"
    "test_io_nonthrowing.cpp"
    "test_io_operations.cpp"
    "test_io_composition.cpp"
    "test_io_transport.cpp"
)
set(freertos_sources
    "test_freertos_wait.cpp"
    "test_freertos_task_context.cpp"
    "test_freertos_task_context_integration.cpp"
)
set(peripheral_sources)
set(peripheral_dependencies)
if(CONFIG_SOC_GPTIMER_SUPPORTED AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND peripheral_sources "test_idf_gptimer.cpp" "test_idf_gptimer_context.cpp")
    if(NOT CONFIG_FREERTOS_UNICORE)
        list(APPEND peripheral_sources "test_idf_gptimer_join.cpp")
    endif()
endif()
if(NOT IDF_TARGET STREQUAL "linux")
    list(APPEND peripheral_dependencies esp_timer)
endif()
if(IDF_TARGET STREQUAL "esp32s3" AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND peripheral_sources "test_idf_gptimer_wait.cpp" "test_interrupt_handoff.cpp")
    list(APPEND peripheral_dependencies esp_driver_gptimer esp_timer esp_system)
endif()
idf_component_register(
    SRCS
        "test_main.cpp"
        ${portable_sources}
        ${freertos_sources}
        ${peripheral_sources}
    INCLUDE_DIRS
        "."
    WHOLE_ARCHIVE
    PRIV_REQUIRES
        console
        esp_execution
        esp_freertos_cpp
        esp_execution_test_support
        esp_freertos_cpp_test_support
        freertos
        ${peripheral_dependencies}
)

set_source_files_properties(
    "test_nonthrowing.cpp" "test_io_nonthrowing.cpp"
    PROPERTIES COMPILE_OPTIONS "-fno-exceptions"
)
