set(execution_sources "task_context.cpp")
set(execution_dependencies freertos esp_freertos_cpp)
idf_component_register(
    SRCS
        ${execution_sources}
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${execution_dependencies}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_26)
target_compile_options(${COMPONENT_LIB} PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>
)
