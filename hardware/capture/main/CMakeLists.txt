# Kconfig picks which board's list gets built.
if(CONFIG_CAPTURE_BOARD_4B)
  set(board board_4b.cpp)
elseif(CONFIG_CAPTURE_BOARD_349)
  set(board board_349.cpp)
endif()

idf_component_register(SRCS capture.cpp chips.cpp ${board}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2c esp_driver_usb_serial_jtag)
