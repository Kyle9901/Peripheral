idf_component_register(
    SRCS
        main.cpp
        dht_sensor.c
        bh1750.c
        sensor_manager.c
    INCLUDE_DIRS
        .
    PRIV_REQUIRES
        esp_matter
        esp_driver_gpio
        esp_driver_i2c
        esp_timer
        nvs_flash
)

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
