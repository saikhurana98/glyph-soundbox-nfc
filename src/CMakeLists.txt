idf_component_register(
    SRCS "main.cpp" "pn532.cpp" "nau8325.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES "../assets/card-tap.mp3"
    REQUIRES driver esp_driver_i2c esp_driver_i2s fatfs nvs_flash sdmmc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -O2)
