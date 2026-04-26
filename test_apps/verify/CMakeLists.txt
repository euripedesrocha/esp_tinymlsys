cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_memory_verify)

if(TARGET __idf_esp_memory AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        __idf_esp_memory
        PRIVATE
            -Wall
            -Wextra
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wformat=2
            -Wnull-dereference
            -Wdouble-promotion
            -Wimplicit-fallthrough
            -Werror
    )
endif()

if(TARGET __idf_esp_memory AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(
        __idf_esp_memory
        PRIVATE
            -Wcast-align=strict
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
            -Wzero-as-null-pointer-constant
    )
endif()
