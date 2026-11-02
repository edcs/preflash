idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES console esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi
                                     esp_driver_usb_serial_jtag esp_lcd esp_timer
                                     espressif__esp_lcd_axs15231b lvgl__lvgl)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
