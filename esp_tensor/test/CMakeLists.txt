cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

project(esp_tensor_test)

set(verified_components main esp_tensor)
idf_build_get_property(build_components BUILD_COMPONENTS)

if(IDF_TOOLCHAIN STREQUAL "clang" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(component_name ${build_components})
        if(NOT component_name IN_LIST verified_components)
            idf_component_get_property(component_lib ${component_name} COMPONENT_LIB)
            if(TARGET ${component_lib})
                get_target_property(component_type ${component_lib} TYPE)
                if(NOT component_type STREQUAL "INTERFACE_LIBRARY")
                    target_compile_options(${component_lib} PRIVATE -Wno-unused-command-line-argument)
                endif()
            endif()
        endif()
    endforeach()
endif()
