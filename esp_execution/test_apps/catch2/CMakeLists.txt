cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../common/esp_execution_test_support"
    "${CMAKE_CURRENT_LIST_DIR}/../../../esp_freertos_cpp/test_apps/common/esp_freertos_cpp_test_support"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_execution_catch2_tests)
