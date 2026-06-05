cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

project(esp_tensor_test)

if(CONFIG_COMPILER_STATIC_ANALYZER AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target_name Catch2 Catch2WithMain __idf_espressif__catch2)
        if(TARGET ${target_name})
            target_compile_options(${target_name} PRIVATE -fno-analyzer)
        endif()
    endforeach()
endif()
