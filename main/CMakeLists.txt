idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_mm esp_driver_parlio esp_driver_gpio esp_tinyusb
)
