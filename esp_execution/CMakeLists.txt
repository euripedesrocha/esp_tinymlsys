set(execution_sources "task_context.cpp")
set(execution_dependencies freertos esp_freertos_cpp)
set(execution_private_dependencies)
idf_build_get_property(execution_target IDF_TARGET)
if(NOT execution_target STREQUAL "linux")
    list(APPEND execution_dependencies esp_driver_gptimer esp_system)
    list(APPEND execution_private_dependencies esp_hal_timg)
endif()
if(CONFIG_SOC_GPTIMER_SUPPORTED AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND execution_sources "gptimer.cpp" "gptimer_context.cpp")
endif()
idf_component_register(
    SRCS
        ${execution_sources}
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${execution_dependencies}
    PRIV_REQUIRES
        ${execution_private_dependencies}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_26)
target_compile_options(${COMPONENT_LIB} PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>
    $<$<COMPILE_LANGUAGE:CXX>:-freflection>
)
