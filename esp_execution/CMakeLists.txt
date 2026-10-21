idf_component_register(
    SRCS
        "task_context.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_26)
target_compile_options(${COMPONENT_LIB} PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>
)
