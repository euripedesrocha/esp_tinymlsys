set(native_sources)
if(CONFIG_SOC_GPTIMER_SUPPORTED AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND native_sources "test_gptimer.cpp")
endif()
idf_component_register(SRCS "test_main.cpp" ${native_sources} INCLUDE_DIRS "." WHOLE_ARCHIVE
    PRIV_REQUIRES console esp_platform_cpp esp_freertos_cpp freertos esp_timer)
