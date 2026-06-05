include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/cpm.cmake)

CPMAddPackage(
    NAME kokkos_mdspan_source
    GIT_REPOSITORY https://github.com/kokkos/mdspan.git
    GIT_TAG mdspan-0.6.0
    DOWNLOAD_ONLY YES
)

if(NOT TARGET esp_tensor_mdspan)
    add_library(esp_tensor_mdspan INTERFACE)
    add_library(esp_tensor::mdspan ALIAS esp_tensor_mdspan)
    target_include_directories(
        esp_tensor_mdspan
        SYSTEM
        INTERFACE
            $<BUILD_INTERFACE:${kokkos_mdspan_source_SOURCE_DIR}/include>
    )
endif()
