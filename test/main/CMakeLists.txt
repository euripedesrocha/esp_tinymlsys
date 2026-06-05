idf_component_register(SRCS "test_main.cpp"
                            "test_tensor.cpp"
                            INCLUDE_DIRS "."
                            WHOLE_ARCHIVE
                            PRIV_REQUIRES console esp_tensor)
