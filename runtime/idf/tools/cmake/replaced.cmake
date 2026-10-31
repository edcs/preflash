# Managed components Preflash provides itself under runtime/, so the project's
# copies, which talk to real hardware, stay out of the build.
set(PREFLASH_REPLACED_COMPONENTS
    "waveshare__*"
    "espressif__esp_lvgl_port"
    "espressif__esp_lcd_*"
    "espressif__esp_codec_dev"
    "espressif__esp_io_expander*"
    "espressif__cmake_utilities")
