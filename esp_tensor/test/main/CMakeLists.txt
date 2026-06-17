idf_component_register(SRCS "test_main.cpp"
                            "test_tensor_view.cpp"
                            "test_tensor.cpp"
                            "test_tensor_shape.cpp"
                            "test_tensor_arith_affine.cpp"
                            "test_tensor_arith_fixed_point.cpp"
                            "test_tensor_dot.cpp"
                            "test_tensor_ops.cpp"
                            "test_tensor_requantize.cpp"
                            INCLUDE_DIRS "."
                            WHOLE_ARCHIVE
                            PRIV_REQUIRES console esp_tensor)

if(IDF_TOOLCHAIN STREQUAL "clang" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
        ${COMPONENT_LIB}
        PRIVATE
            -Wall
            -Wextra
            -Wconversion
            -Wsign-conversion
            -Wshift-overflow
            -Wshadow
            -Wformat=2
            -Wnull-dereference
            -Wdouble-promotion
            -Wimplicit-fallthrough
            -Werror
    )
endif()
