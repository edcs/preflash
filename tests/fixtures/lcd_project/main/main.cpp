// A project with no board package: it brings up the 3.49 V2's panel, touch, backlight and
// sensor through esp_lcd and the plain drivers, and reads its own serial input.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace {

constexpr int kWidth = 172;
constexpr int kHeight = 640;
constexpr int kBytesPerPixel = 2;
constexpr int kChunkRows = 64;
constexpr size_t kFrameBytes = kWidth * kHeight * kBytesPerPixel;
constexpr size_t kChunkBytes = kWidth * kChunkRows * kBytesPerPixel;

constexpr uint8_t kExpander = 0x20;
constexpr uint8_t kExpanderOutput = 0x01;
constexpr uint8_t kExpanderConfig = 0x03;
constexpr uint8_t kBacklightEnable = 1 << 1;
constexpr uint8_t kSysEnable = 1 << 6;
constexpr uint8_t kMotion = 0x6b;
constexpr uint8_t kTouch = 0x3b;
constexpr gpio_num_t kPowerKey = GPIO_NUM_16;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
constexpr ledc_timer_t kTimer = LEDC_TIMER_3;
constexpr int kBoxSize = 40;

i2c_master_bus_handle_t g_sensors = nullptr;
i2c_master_bus_handle_t g_touch_bus = nullptr;
i2c_master_dev_handle_t g_expander = nullptr;
i2c_master_dev_handle_t g_motion = nullptr;
i2c_master_dev_handle_t g_touch = nullptr;
uint8_t g_expander_output = 0;
esp_lcd_panel_handle_t g_panel = nullptr;
SemaphoreHandle_t g_chunk_sent = nullptr;
int g_chunks_done = 0;
uint8_t* g_rotated = nullptr;
uint8_t* g_chunk = nullptr;
lv_display_t* g_display = nullptr;
int g_flushes = 0;

i2c_master_bus_handle_t new_bus(i2c_port_t port, int sda, int scl) {
  i2c_master_bus_config_t config = {};
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.i2c_port = port;
  config.scl_io_num = static_cast<gpio_num_t>(scl);
  config.sda_io_num = static_cast<gpio_num_t>(sda);
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
  return bus;
}

i2c_master_dev_handle_t new_device(i2c_master_bus_handle_t bus, uint8_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = 100000;
  i2c_master_dev_handle_t device = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device));
  return device;
}

void write_register(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
  const uint8_t frame[] = {reg, value};
  ESP_ERROR_CHECK(i2c_master_transmit(device, frame, sizeof frame, 100));
}

void set_backlight_enable(bool on) {
  g_expander_output = on ? g_expander_output | kBacklightEnable : g_expander_output & ~kBacklightEnable;
  write_register(g_expander, kExpanderOutput, g_expander_output);
}

void start_power() {
  g_expander = new_device(g_sensors, kExpander);
  g_expander_output = kSysEnable;
  write_register(g_expander, kExpanderOutput, g_expander_output);
  write_register(g_expander, kExpanderConfig, static_cast<uint8_t>(~(kSysEnable | kBacklightEnable)));

  gpio_config_t key = {};
  key.pin_bit_mask = 1ULL << kPowerKey;
  key.mode = GPIO_MODE_INPUT;
  key.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&key));
}

void start_backlight() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = kTimer;
  timer.freq_hz = 50 * 1000;
  timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  ledc_channel_config_t channel = {};
  channel.gpio_num = 42;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = kChannel;
  channel.timer_sel = kTimer;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void set_backlight(int percent) {
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel, 255 - percent * 255 / 100));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel));
  set_backlight_enable(percent > 0);
}

bool IRAM_ATTR chunk_sent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  ++g_chunks_done;
  xSemaphoreGiveFromISR(g_chunk_sent, &woken);
  return woken == pdTRUE;
}

void start_panel() {
  const spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(10, 11, 12, 13, 14, kChunkBytes);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

  const esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(9, chunk_sent, nullptr);
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io));

  axs15231b_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = -1;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = 16;
  panel_config.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &panel_config, &g_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
}

// The panel only takes whole frames, in rows of big-endian pixels.
void send_frame(const uint8_t* frame) {
  xSemaphoreTake(g_chunk_sent, 0);
  for (int y = 0; y < kHeight; y += kChunkRows) {
    const int rows = std::min(kChunkRows, kHeight - y);
    std::memcpy(g_chunk, frame + y * kWidth * kBytesPerPixel, rows * kWidth * kBytesPerPixel);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_chunk));
    if (xSemaphoreTake(g_chunk_sent, pdMS_TO_TICKS(1000)) != pdTRUE) {
      ESP_LOGE("lcd", "a chunk never finished");
      return;
    }
  }
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
  ++g_flushes;
  lv_draw_sw_rgb565_swap(pixels, kWidth * kHeight);
  const lv_display_rotation_t rotation = lv_display_get_rotation(display);
  const uint8_t* frame = pixels;
  if (rotation != LV_DISPLAY_ROTATION_0) {
    const int32_t width = lv_area_get_width(area);
    lv_draw_sw_rotate(pixels, g_rotated, width, lv_area_get_height(area), width * kBytesPerPixel,
                      kWidth * kBytesPerPixel, rotation, LV_COLOR_FORMAT_RGB565);
    frame = g_rotated;
  }
  send_frame(frame);
  lv_display_flush_ready(display);
}

void read_touch(lv_indev_t*, lv_indev_data_t* data) {
  static bool was_down = false;
  const uint8_t command[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
  uint8_t reply[32] = {};
  data->state = LV_INDEV_STATE_RELEASED;
  const bool read = i2c_master_transmit_receive(g_touch, command, sizeof command, reply, sizeof reply, 20) == ESP_OK;
  if (!read || reply[1] == 0 || reply[1] >= 5) {
    was_down = false;
    return;
  }
  const int long_side = std::min(((reply[2] & 0x0f) << 8) | reply[3], kHeight - 1);
  const int short_side = std::min(((reply[4] & 0x0f) << 8) | reply[5], kWidth - 1);
  data->point.x = short_side;
  data->point.y = kHeight - 1 - long_side;
  data->state = LV_INDEV_STATE_PRESSED;
  if (!was_down) printf("touch %d %d\n", static_cast<int>(data->point.x), static_cast<int>(data->point.y));
  was_down = true;
}

uint32_t tick_ms() { return esp_timer_get_time() / 1000; }

void on_clicked(lv_event_t*) { printf("clicked\n"); }

void lvgl_task(void*) {
  for (;;) {
    const uint32_t delay = lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(std::clamp<uint32_t>(delay, 5, 100)));
  }
}

void start_lvgl() {
  lv_init();
  lv_tick_set_cb(tick_ms);
  void* frame = heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM);
  g_rotated = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
  g_chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  assert(frame && g_rotated && g_chunk);

  g_display = lv_display_create(kWidth, kHeight);
  lv_display_set_flush_cb(g_display, flush);
  lv_display_set_buffers(g_display, frame, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_FULL);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, read_touch);

  // A red box in the corner LVGL calls the top left, which shows how the picture turned.
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_add_event_cb(screen, on_clicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* box = lv_obj_create(screen);
  lv_obj_set_pos(box, 0, 0);
  lv_obj_set_size(box, kBoxSize, kBoxSize);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_make(0xff, 0, 0), 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

  assert(xTaskCreate(lvgl_task, "lvgl", 8192, nullptr, 2, nullptr) == pdPASS);
}

uint16_t big_endian(uint16_t rgb565) { return static_cast<uint16_t>(rgb565 << 8 | rgb565 >> 8); }

// Straight to the panel, past LVGL: red, green, blue and white, clockwise from the top
// left, so that a wrong byte order or a wrong way up can't look right.
int pattern_command(int, char**) {
  static uint16_t frame[kWidth * kHeight];
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const bool right = x >= kWidth / 2;
      const bool bottom = y >= kHeight / 2;
      const uint16_t colour = bottom ? (right ? 0x001f : 0xffff) : (right ? 0x07e0 : 0xf800);
      frame[y * kWidth + x] = big_endian(colour);
    }
  }
  lv_lock();
  const int before = g_chunks_done;
  send_frame(reinterpret_cast<const uint8_t*>(frame));
  printf("pattern drawn in %d chunks\n", g_chunks_done - before);
  lv_unlock();
  return 0;
}

int redraw_command(int, char**) {
  lv_lock();
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(g_display);
  lv_unlock();
  printf("redrawn\n");
  return 0;
}

int rotate_command(int argc, char** argv) {
  if (argc != 2) return 1;
  lv_lock();
  lv_display_set_rotation(g_display, static_cast<lv_display_rotation_t>(atoi(argv[1])));
  lv_refr_now(g_display);
  lv_unlock();
  printf("rotated %s\n", argv[1]);
  return 0;
}

int backlight_command(int argc, char** argv) {
  if (argc != 2) return 1;
  set_backlight(atoi(argv[1]));
  printf("backlight %s\n", argv[1]);
  return 0;
}

int key_command(int, char**) {
  printf("key level %d\n", gpio_get_level(kPowerKey));
  return 0;
}

// Each chip answers on its own bus and is silent on the other.
int buses_command(int, char**) {
  i2c_master_dev_handle_t touch_on_sensors = new_device(g_sensors, kTouch);
  i2c_master_dev_handle_t expander_on_touch = new_device(g_touch_bus, kExpander);
  uint8_t reply[32];
  const uint8_t command[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
  const uint8_t reg = kExpanderOutput;
  printf("touch on bus 0: %s\n",
         esp_err_to_name(i2c_master_transmit_receive(touch_on_sensors, command, sizeof command, reply, sizeof reply, 20)));
  printf("expander on bus 1: %s\n",
         esp_err_to_name(i2c_master_transmit_receive(expander_on_touch, &reg, 1, reply, 1, 20)));
  printf("expander on bus 0: %s\n",
         esp_err_to_name(i2c_master_transmit_receive(g_expander, &reg, 1, reply, 1, 20)));
  return 0;
}

int motion_command(int, char**) {
  write_register(g_motion, 0x60, 0xb0);
  write_register(g_motion, 0x02, 0x40);
  write_register(g_motion, 0x03, 0x08);
  write_register(g_motion, 0x08, 0x01);
  const uint8_t reg = 0x35;
  uint8_t data[6];
  ESP_ERROR_CHECK(i2c_master_transmit_receive(g_motion, &reg, 1, data, sizeof data, 100));
  printf("accel %d %d %d\n", static_cast<int16_t>(data[0] | data[1] << 8), static_cast<int16_t>(data[2] | data[3] << 8),
         static_cast<int16_t>(data[4] | data[5] << 8));
  return 0;
}

// While this holds LVGL's lock, the LVGL task must not draw.
int lock_command(int, char**) {
  lv_lock();
  lv_obj_invalidate(lv_screen_active());
  const int before = g_flushes;
  vTaskDelay(pdMS_TO_TICKS(150));
  const int during = g_flushes - before;
  lv_unlock();
  vTaskDelay(pdMS_TO_TICKS(150));
  printf("flushes during the lock %d, after it %d\n", during, g_flushes - before - during);
  return 0;
}

int reason_command(int, char**) {
  printf("reset reason %d\n", static_cast<int>(esp_reset_reason()));
  return 0;
}

void restart_soon(void*) { esp_restart(); }

int restart_command(int, char**) {
  xTaskCreate(restart_soon, "restart", 4096, nullptr, 2, nullptr);
  return 0;
}

void add_command(const char* name, esp_console_cmd_func_t func) {
  esp_console_cmd_t cmd = {};
  cmd.command = name;
  cmd.func = func;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void start_console() {
  esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_init(&config));
  ESP_ERROR_CHECK(esp_console_register_help_command());
  add_command("pattern", pattern_command);
  add_command("redraw", redraw_command);
  add_command("rotate", rotate_command);
  add_command("backlight", backlight_command);
  add_command("key", key_command);
  add_command("buses", buses_command);
  add_command("motion", motion_command);
  add_command("lock", lock_command);
  add_command("reason", reason_command);
  add_command("restart", restart_command);
}

// Lines starting with @ are the protocol's, and the rest are the console's.
void handle_line(const std::string& line) {
  if (!line.empty() && line.front() == '@') {
    printf("protocol line: %s\n", line.c_str());
    return;
  }
  int result = 0;
  const esp_err_t err = esp_console_run(line.c_str(), &result);
  if (err == ESP_ERR_NOT_FOUND) printf("unknown command\n");
}

void reader_task(void*) {
  std::string line;
  uint8_t buffer[128];
  for (;;) {
    const int got = usb_serial_jtag_read_bytes(buffer, sizeof buffer, portMAX_DELAY);
    for (int i = 0; i < got; ++i) {
      if (buffer[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) handle_line(line);
        line.clear();
      } else {
        line.push_back(static_cast<char>(buffer[i]));
      }
    }
  }
}

void start_serial() {
  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.tx_buffer_size = 4096;
  config.rx_buffer_size = 2048;
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
  usb_serial_jtag_vfs_use_driver();
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
  assert(xTaskCreate(reader_task, "serial", 16384, nullptr, 3, nullptr) == pdPASS);
}

}  // namespace

extern "C" void app_main(void) {
  g_sensors = new_bus(I2C_NUM_0, 47, 48);
  g_touch_bus = new_bus(I2C_NUM_1, 17, 18);
  start_power();
  start_backlight();
  g_motion = new_device(g_sensors, kMotion);
  g_touch = new_device(g_touch_bus, kTouch);
  g_chunk_sent = xSemaphoreCreateBinary();
  start_panel();
  start_lvgl();
  start_console();
  start_serial();
  printf("ready\n");
}
