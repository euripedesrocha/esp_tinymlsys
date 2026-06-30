idf_component_register(
    SRCS "esp_tensor.cpp" "esp_tensor_dot_s16_esp32s3.S"
    INCLUDE_DIRS "include"
    REQUIRES esp-nn esp-dsp
)

set(esp_tensor_contract_mode enforce)
if(CONFIG_ESP_TENSOR_CONTRACT_MODE_IGNORE)
    set(esp_tensor_contract_mode ignore)
endif()

target_compile_options(${COMPONENT_LIB} PUBLIC
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++26>
    $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>
    $<$<COMPILE_LANGUAGE:CXX>:-freflection>
    $<$<COMPILE_LANGUAGE:CXX>:-fcontract-evaluation-semantic=${esp_tensor_contract_mode}>
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/cpm.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/mdspan.cmake)
    target_link_libraries(${COMPONENT_LIB} PUBLIC esp_tensor::mdspan)
endif()
