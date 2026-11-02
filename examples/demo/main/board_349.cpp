// The ESP32-S3-Touch-LCD-3.49 V2, with no board package: the panel goes through esp_lcd, touch
// is a chip on its own I2C bus, and this file runs LVGL itself. Pin numbers come from
// Waveshare's schematic.
#include <algorithm>
#include <cstring>

#include "board.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace board {
namespace {

constexpr const char* kTag = "board";

constexpr int kWidth = 172;
constexpr int kHeight = 640;
constexpr int kBytesPerPixel = 2;
constexpr int kChunkRows = 64;
constexpr size_t kFrameBytes = kWidth * kHeight * kBytesPerPixel;
constexpr size_t kChunkBytes = kWidth * kChunkRows * kBytesPerPixel;

constexpr gpio_num_t kSensorSda = GPIO_NUM_47;
constexpr gpio_num_t kSensorScl = GPIO_NUM_48;
constexpr gpio_num_t kTouchSda = GPIO_NUM_17;
constexpr gpio_num_t kTouchScl = GPIO_NUM_18;
constexpr int kPanelSelect = 9;
constexpr int kPanelClock = 10;
constexpr int kPanelData0 = 11;
constexpr int kPanelData1 = 12;
constexpr int kPanelData2 = 13;
constexpr int kPanelData3 = 14;
constexpr int kBacklightPin = 42;
constexpr uint32_t kBusHz = 100'000;
constexpr int kI2cTimeoutMs = 100;
constexpr int kPanelResetMs = 20;
constexpr int kPanelWakeMs = 120;
constexpr int kTouchTimeoutMs = 20;

constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kExpanderOutput = 0x01;
constexpr uint8_t kExpanderConfig = 0x03;
constexpr uint8_t kBacklightEnableBit = 1 << 1;
constexpr uint8_t kPanelResetBit = 1 << 5;  // low holds the panel in reset
constexpr uint8_t kSystemEnableBit = 1 << 6;
constexpr uint8_t kTouchAddress = 0x3b;

constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_1;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_3;
constexpr int kBacklightMaxDuty = 255;

// The panel's own defaults need only waking. The driver's built-in list is for another panel.
const axs15231b_lcd_init_cmd_t kPanelStartup[] = {
    {0x11, nullptr, 0, 100},  // sleep out
    {0x29, nullptr, 0, 100},  // display on
};

i2c_master_bus_handle_t g_sensor_bus = nullptr;
i2c_master_dev_handle_t g_expander = nullptr;
i2c_master_dev_handle_t g_touch = nullptr;
uint8_t g_expander_output = 0;

esp_lcd_panel_handle_t g_panel = nullptr;
SemaphoreHandle_t g_chunk_sent = nullptr;
uint8_t* g_turned_frame = nullptr;
uint8_t* g_chunk = nullptr;
lv_display_t* g_display = nullptr;

i2c_master_bus_handle_t new_bus(i2c_port_t port, gpio_num_t sda, gpio_num_t scl) {
  i2c_master_bus_config_t config = {};
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.i2c_port = port;
  config.sda_io_num = sda;
  config.scl_io_num = scl;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
  return bus;
}

i2c_master_dev_handle_t new_device(i2c_master_bus_handle_t bus, uint8_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = kBusHz;
  i2c_master_dev_handle_t device = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &device));
  return device;
}

esp_err_t write_expander(uint8_t reg, uint8_t value) {
  const uint8_t frame[] = {reg, value};
  return i2c_master_transmit(g_expander, frame, sizeof frame, kI2cTimeoutMs);
}

void set_expander_bits(uint8_t bits, bool on) {
  g_expander_output = on ? g_expander_output | bits : g_expander_output & ~bits;
  ESP_ERROR_CHECK(write_expander(kExpanderOutput, g_expander_output));
}

// The expander holds the board's power on, so nothing else can start before it.
void start_power() {
  g_expander = new_device(g_sensor_bus, kExpanderAddress);
  g_expander_output = kSystemEnableBit | kPanelResetBit;
  ESP_ERROR_CHECK(write_expander(kExpanderOutput, g_expander_output));
  const uint8_t outputs = kSystemEnableBit | kPanelResetBit | kBacklightEnableBit;
  ESP_ERROR_CHECK(write_expander(kExpanderConfig, static_cast<uint8_t>(~outputs)));
}

void pulse_panel_reset() {
  set_expander_bits(kPanelResetBit, false);
  vTaskDelay(pdMS_TO_TICKS(kPanelResetMs));
  set_expander_bits(kPanelResetBit, true);
  vTaskDelay(pdMS_TO_TICKS(kPanelWakeMs));
}

void start_backlight() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = kBacklightTimer;
  timer.freq_hz = 50'000;
  timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  ledc_channel_config_t channel = {};
  channel.gpio_num = kBacklightPin;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = kBacklightChannel;
  channel.timer_sel = kBacklightTimer;
  channel.duty = kBacklightMaxDuty;  // active low, so this is off
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(g_chunk_sent, &woken);
  return woken == pdTRUE;
}

void start_panel() {
  // Set by hand, because the driver's macro lists fields out of order, which C++ refuses.
  spi_bus_config_t bus = {};
  bus.sclk_io_num = kPanelClock;
  bus.data0_io_num = kPanelData0;
  bus.data1_io_num = kPanelData1;
  bus.data2_io_num = kPanelData2;
  bus.data3_io_num = kPanelData3;
  bus.max_transfer_sz = kChunkBytes;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

  const esp_lcd_panel_io_spi_config_t io_config = AXS15231B_PANEL_IO_QSPI_CONFIG(kPanelSelect, on_chunk_sent, nullptr);
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io));

  axs15231b_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  vendor.init_cmds = kPanelStartup;
  vendor.init_cmds_size = sizeof kPanelStartup / sizeof kPanelStartup[0];
  esp_lcd_panel_dev_config_t panel = {};
  panel.reset_gpio_num = -1;  // it's on the expander
  panel.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel.bits_per_pixel = 16;
  panel.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &panel, &g_panel));
  pulse_panel_reset();
  ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
}

// The panel takes whole frames only, in chunks small enough for DMA from internal RAM.
void send_frame(const uint8_t* frame) {
  xSemaphoreTake(g_chunk_sent, 0);
  for (int y = 0; y < kHeight; y += kChunkRows) {
    const int rows = std::min(kChunkRows, kHeight - y);
    std::memcpy(g_chunk, frame + y * kWidth * kBytesPerPixel, rows * kWidth * kBytesPerPixel);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(g_panel, 0, y, kWidth, y + rows, g_chunk));
    if (xSemaphoreTake(g_chunk_sent, pdMS_TO_TICKS(1000)) != pdTRUE) {
      ESP_LOGE(kTag, "a chunk never finished");
      return;
    }
  }
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
  lv_draw_sw_rgb565_swap(pixels, kWidth * kHeight);  // the panel wants big-endian pixels
  const lv_display_rotation_t rotation = lv_display_get_rotation(display);
  const uint8_t* frame = pixels;
  if (rotation != LV_DISPLAY_ROTATION_0) {
    const int32_t width = lv_area_get_width(area);
    lv_draw_sw_rotate(pixels, g_turned_frame, width, lv_area_get_height(area), width * kBytesPerPixel,
                      kWidth * kBytesPerPixel, rotation, LV_COLOR_FORMAT_RGB565);
    frame = g_turned_frame;
  }
  send_frame(frame);
  lv_display_flush_ready(display);
}

// The touch chip answers this fixed request with a count of points and the first point.
void read_touch(lv_indev_t*, lv_indev_data_t* data) {
  const uint8_t request[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
  uint8_t reply[32] = {};
  data->state = LV_INDEV_STATE_RELEASED;
  const esp_err_t err =
      i2c_master_transmit_receive(g_touch, request, sizeof request, reply, sizeof reply, kTouchTimeoutMs);
  const int points = reply[1];
  if (err != ESP_OK || points == 0 || points >= 5) return;
  const int long_side = std::min(((reply[2] & 0x0f) << 8) | reply[3], kHeight - 1);
  const int short_side = std::min(((reply[4] & 0x0f) << 8) | reply[5], kWidth - 1);
  data->point.x = short_side;
  data->point.y = kHeight - 1 - long_side;
  data->state = LV_INDEV_STATE_PRESSED;
}

uint32_t milliseconds() { return esp_timer_get_time() / 1000; }

void lvgl_task(void*) {
  for (;;) {
    const uint32_t wait_ms = lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(std::clamp<uint32_t>(wait_ms, 5, 100)));
  }
}

void start_lvgl() {
  lv_init();
  lv_tick_set_cb(milliseconds);

  void* frame = heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM);
  g_turned_frame = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
  g_chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  ESP_ERROR_CHECK(frame && g_turned_frame && g_chunk ? ESP_OK : ESP_ERR_NO_MEM);

  g_display = lv_display_create(kWidth, kHeight);
  lv_display_set_flush_cb(g_display, flush);
  lv_display_set_buffers(g_display, frame, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_FULL);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, read_touch);

  ESP_ERROR_CHECK(xTaskCreate(lvgl_task, "lvgl", 8192, nullptr, 2, nullptr) == pdPASS ? ESP_OK : ESP_FAIL);
}

}  // namespace

void start() {
  g_sensor_bus = new_bus(I2C_NUM_0, kSensorSda, kSensorScl);
  g_touch = new_device(new_bus(I2C_NUM_1, kTouchSda, kTouchScl), kTouchAddress);
  g_chunk_sent = xSemaphoreCreateBinary();
  start_power();
  start_backlight();
  start_panel();
  start_lvgl();
}

const char* name() { return "ESP32-S3-Touch-LCD-3.49 V2"; }

i2c_master_bus_handle_t sensor_bus() { return g_sensor_bus; }

void set_brightness(int percent) {
  const uint32_t duty = kBacklightMaxDuty - percent * kBacklightMaxDuty / 100;
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel, duty));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel));
  set_expander_bits(kBacklightEnableBit, percent > 0);
}

bool can_turn_to(int degrees) { return degrees == 0 || degrees == 180; }

void turn_to(int degrees) {
  lv_display_set_rotation(g_display, static_cast<lv_display_rotation_t>(degrees / 90));
}

bool has_power_chip() { return false; }

esp_codec_dev_handle_t speaker() { return nullptr; }

Lock::Lock() { lv_lock(); }

Lock::~Lock() { lv_unlock(); }

}  // namespace board
