idf_component_register(
    SRCS "esp_tensor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-nn esp-dsp
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/cpm.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/mdspan.cmake)
    target_link_libraries(${COMPONENT_LIB} PUBLIC esp_tensor::mdspan)
endif()
