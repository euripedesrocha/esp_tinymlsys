idf_component_register(INCLUDE_DIRS "include")

target_compile_features(${COMPONENT_LIB} INTERFACE cxx_std_26)
target_compile_options(${COMPONENT_LIB} INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>
)
