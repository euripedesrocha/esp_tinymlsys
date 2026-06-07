idf_component_register(
    SRCS "esp_memory_resource.cpp"
    INCLUDE_DIRS "include"
    REQUIRES heap
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(
        ${COMPONENT_LIB}
        PRIVATE
            -Wconversion
            -Wsign-conversion
            -Warith-conversion
            -Wshadow
            -Wformat=2
            -Wnull-dereference
            -Wdouble-promotion
            -Wimplicit-fallthrough
            -Wcast-qual
    )
endif()
