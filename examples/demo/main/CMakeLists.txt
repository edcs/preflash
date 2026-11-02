set(srcs
    app_main.cpp
    audio.cpp
    console.cpp
    i2c_chip.cpp
    monitor.cpp
    motion.cpp
    mishaps.cpp
    network.cpp
    power.cpp
    rtc.cpp
    settings.cpp
    timekeeper.cpp
    update.cpp
    ui/ui.cpp
    ui/widgets.cpp
    ui/page_home.cpp
    ui/page_time.cpp
    ui/page_motion.cpp
    ui/page_power.cpp
    ui/page_sound.cpp
    ui/page_network.cpp
    ui/page_update.cpp
    ui/page_settings.cpp
    ui/page_faults.cpp)

# One app, two ways to reach a screen. Kconfig picks which file gets built, so the rest of
# the code sees only board.h.
if(CONFIG_DEMO_BOARD_4B)
  list(APPEND srcs board_4b.cpp)
elseif(CONFIG_DEMO_BOARD_349)
  list(APPEND srcs board_349.cpp)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_update console esp_driver_gpio esp_driver_i2c esp_driver_ledc
                                     esp_driver_spi esp_event esp_http_client esp_https_ota esp_lcd
                                     esp_netif esp_timer esp_wifi lvgl__lvgl mbedtls nvs_flash
                                     espressif__esp_lcd_axs15231b waveshare__esp32_s3_touch_lcd_4b)
