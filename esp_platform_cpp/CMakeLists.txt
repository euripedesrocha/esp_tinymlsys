set(platform_sources)
if(CONFIG_SOC_GPTIMER_SUPPORTED AND NOT CONFIG_GPTIMER_ISR_CACHE_SAFE)
    list(APPEND platform_sources "gptimer.cpp")
endif()
# Dependencies are discovered before CONFIG_SOC_* values are loaded.
idf_component_register(SRCS ${platform_sources} INCLUDE_DIRS "include"
    REQUIRES freertos esp_freertos_cpp esp_driver_gptimer esp_system
    PRIV_REQUIRES esp_hal_timg)
if(platform_sources)
    set(platform_compile_scope PUBLIC)
else()
    set(platform_compile_scope INTERFACE)
endif()
target_compile_features(${COMPONENT_LIB} ${platform_compile_scope} cxx_std_26)
target_compile_options(${COMPONENT_LIB} ${platform_compile_scope} $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>)
