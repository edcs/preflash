// The ESP32-S3-Touch-LCD-3.49 V2, which has no board package. LVGL renders every frame whole into
// PSRAM, and each flush streams that frame to the panel through a small strip of DMA memory.
#include <algorithm>
#include <cstdint>

#include "board.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
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
#include "i2c_chip.h"
#include "lvgl.h"

namespace board {
namespace {

constexpr const char* kTag = "board";

constexpr int kWidth = 172;
constexpr int kHeight = 640;
constexpr int kStripRows = 40;
constexpr size_t kFramePixels = kWidth * kHeight;
constexpr size_t kStripPixels = kWidth * kStripRows;
static_assert(kHeight % kStripRows == 0, "every strip is a full one");

constexpr spi_host_device_t kPanelHost = SPI3_HOST;
constexpr int kPanelCs = 9;
constexpr int kPanelClock = 10;
constexpr int kPanelData[] = {11, 12, 13, 14};
constexpr uint32_t kPanelHz = 40'000'000;

constexpr uint8_t kSleepOut = 0x11;
constexpr uint8_t kDisplayOn = 0x29;
constexpr axs15231b_lcd_init_cmd_t kPanelStartup[] = {
    {kSleepOut, nullptr, 0, 100},
    {kDisplayOn, nullptr, 0, 100},
};

constexpr gpio_num_t kSensorSda = GPIO_NUM_47;
constexpr gpio_num_t kSensorScl = GPIO_NUM_48;
constexpr gpio_num_t kTouchSda = GPIO_NUM_17;
constexpr gpio_num_t kTouchScl = GPIO_NUM_18;

constexpr uint8_t kExpanderAddress = 0x20;
constexpr uint8_t kExpanderOutputs = 0x01;
constexpr uint8_t kExpanderDirections = 0x03;  // a set bit is an input
constexpr uint8_t kBacklightEnable = 1 << 1;
constexpr uint8_t kPanelReset = 1 << 5;  // active low

constexpr int kBacklightPin = 42;
constexpr ledc_timer_t kPwmTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kPwmChannel = LEDC_CHANNEL_0;
constexpr int kPwmHz = 20'000;
constexpr uint32_t kPwmFullScale = 1 << LEDC_TIMER_10_BIT;

constexpr uint16_t kTouchAddress = 0x3b;
constexpr uint32_t kTouchBusHz = 300'000;
constexpr int kTouchTimeoutMs = 20;
constexpr size_t kTouchReplyBytes = 14;
constexpr uint8_t kTouchQuery[] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, kTouchReplyBytes, 0x00, 0x00, 0x00};

constexpr uint32_t kLongestRenderWaitMs = 20;

// What a flush needs. The strip belongs to the transfer in flight until strip_sent is given.
struct Screen {
  esp_lcd_panel_handle_t panel = nullptr;
  lv_display_t* display = nullptr;
  uint16_t* strip = nullptr;
  SemaphoreHandle_t strip_sent = nullptr;
};

Screen g_screen;
i2c_master_bus_handle_t g_sensor_bus = nullptr;
i2c_master_dev_handle_t g_touch = nullptr;
I2cChip g_expander;
uint8_t g_expander_levels = kPanelReset;

template <typename T>
T* allocate(size_t count, uint32_t caps) {
  void* memory = heap_caps_malloc(count * sizeof(T), caps);
  ESP_ERROR_CHECK(memory ? ESP_OK : ESP_ERR_NO_MEM);
  return static_cast<T*>(memory);
}

i2c_master_bus_handle_t open_bus(i2c_port_t port, gpio_num_t sda, gpio_num_t scl) {
  i2c_master_bus_config_t config = {};
  config.i2c_port = port;
  config.sda_io_num = sda;
  config.scl_io_num = scl;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
  return bus;
}

void start_expander() {
  g_expander.attach(g_sensor_bus, kExpanderAddress);
  // Levels first, so that the reset line never dips when it becomes an output.
  ESP_ERROR_CHECK(g_expander.write(kExpanderOutputs, g_expander_levels));
  ESP_ERROR_CHECK(g_expander.write(kExpanderDirections, static_cast<uint8_t>(~(kBacklightEnable | kPanelReset))));
}

esp_err_t set_expander_line(uint8_t line, bool high) {
  g_expander_levels = high ? g_expander_levels | line : g_expander_levels & ~line;
  return g_expander.write(kExpanderOutputs, g_expander_levels);
}

void pulse_panel_reset() {
  ESP_ERROR_CHECK(set_expander_line(kPanelReset, false));
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_ERROR_CHECK(set_expander_line(kPanelReset, true));
  vTaskDelay(pdMS_TO_TICKS(30));
}

// The driver's output inverts because the backlight is active low, so duty reads as brightness.
void start_backlight_pwm() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.timer_num = kPwmTimer;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.freq_hz = kPwmHz;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  ledc_channel_config_t channel = {};
  channel.gpio_num = kBacklightPin;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = kPwmChannel;
  channel.timer_sel = kPwmTimer;
  channel.flags.output_invert = true;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

bool strip_finished(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(g_screen.strip_sent, &woken);
  return woken == pdTRUE;
}

void start_panel() {
  g_screen.strip = allocate<uint16_t>(kStripPixels, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  g_screen.strip_sent = xSemaphoreCreateBinary();
  ESP_ERROR_CHECK(g_screen.strip_sent ? ESP_OK : ESP_ERR_NO_MEM);

  spi_bus_config_t bus = {};
  bus.sclk_io_num = kPanelClock;
  bus.data0_io_num = kPanelData[0];
  bus.data1_io_num = kPanelData[1];
  bus.data2_io_num = kPanelData[2];
  bus.data3_io_num = kPanelData[3];
  bus.max_transfer_sz = kStripPixels * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(kPanelHost, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t link = {};
  link.cs_gpio_num = kPanelCs;
  link.dc_gpio_num = -1;
  link.spi_mode = 3;
  link.pclk_hz = kPanelHz;
  link.trans_queue_depth = 10;
  link.on_color_trans_done = strip_finished;
  link.lcd_cmd_bits = 32;
  link.lcd_param_bits = 8;
  link.flags.quad_mode = true;
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)kPanelHost, &link, &io));

  axs15231b_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = true;
  vendor.init_cmds = kPanelStartup;
  vendor.init_cmds_size = sizeof kPanelStartup / sizeof kPanelStartup[0];
  esp_lcd_panel_dev_config_t device = {};
  device.reset_gpio_num = -1;  // it's on the expander
  device.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  device.bits_per_pixel = 16;
  device.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &device, &g_screen.panel));

  pulse_panel_reset();
  ESP_ERROR_CHECK(esp_lcd_panel_init(g_screen.panel));
}

// Over QSPI this panel takes rows only from the top down, so a frame always goes out whole. A half
// turn needs no second buffer, just reading the canvas backwards. Pixels go high byte first.
void flush(lv_display_t* display, const lv_area_t*, uint8_t* canvas) {
  const bool upside_down = lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_180;
  const auto* first = reinterpret_cast<const uint16_t*>(canvas);
  const uint16_t* source = upside_down ? first + kFramePixels - 1 : first;
  const int step = upside_down ? -1 : 1;

  for (int top = 0; top < kHeight; top += kStripRows) {
    for (size_t i = 0; i < kStripPixels; ++i, source += step) g_screen.strip[i] = __builtin_bswap16(*source);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(g_screen.panel, 0, top, kWidth, top + kStripRows, g_screen.strip));
    xSemaphoreTake(g_screen.strip_sent, portMAX_DELAY);
  }
  lv_display_flush_ready(display);
}

uint16_t twelve_bits_at(const uint8_t* reply) { return (reply[0] & 0x0f) << 8 | reply[1]; }

// LVGL keeps the last point on its own, and turns it to match the display.
void read_touch(lv_indev_t*, lv_indev_data_t* data) {
  uint8_t reply[kTouchReplyBytes] = {};
  const esp_err_t err =
      i2c_master_transmit_receive(g_touch, kTouchQuery, sizeof kTouchQuery, reply, sizeof reply, kTouchTimeoutMs);
  const uint8_t fingers = reply[1];
  if (err != ESP_OK || fingers == 0) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  // The chip counts along the panel from its far end.
  data->point.x = twelve_bits_at(reply + 4);
  data->point.y = kHeight - 1 - twelve_bits_at(reply + 2);
  data->state = LV_INDEV_STATE_PRESSED;
}

void start_touch(i2c_master_bus_handle_t bus) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = kTouchAddress;
  config.scl_speed_hz = kTouchBusHz;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &config, &g_touch));

  lv_indev_t* pointer = lv_indev_create();
  lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer, read_touch);
  lv_indev_set_display(pointer, g_screen.display);
}

uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void render_task(void*) {
  for (;;) {
    const uint32_t wait_ms = std::min(lv_timer_handler(), kLongestRenderWaitMs);
    vTaskDelay(std::max<TickType_t>(pdMS_TO_TICKS(wait_ms), 1));
  }
}

void start_lvgl() {
  lv_init();
  lv_tick_set_cb(uptime_ms);

  g_screen.display = lv_display_create(kWidth, kHeight);
  ESP_ERROR_CHECK(g_screen.display ? ESP_OK : ESP_ERR_NO_MEM);
  auto* canvas = allocate<uint16_t>(kFramePixels, MALLOC_CAP_SPIRAM);
  lv_display_set_color_format(g_screen.display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(g_screen.display, canvas, nullptr, kFramePixels * sizeof(uint16_t),
                         LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(g_screen.display, flush);
}

}  // namespace

void start() {
  g_sensor_bus = open_bus(I2C_NUM_0, kSensorSda, kSensorScl);
  start_expander();
  start_backlight_pwm();
  start_panel();
  start_lvgl();
  start_touch(open_bus(I2C_NUM_1, kTouchSda, kTouchScl));
  ESP_ERROR_CHECK(xTaskCreate(render_task, "lvgl", 8192, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_FAIL);
}

const char* name() { return "ESP32-S3-Touch-LCD-3.49 V2"; }

i2c_master_bus_handle_t sensor_bus() { return g_sensor_bus; }

void set_brightness(int percent) {
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, kPwmChannel, percent * kPwmFullScale / 100));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, kPwmChannel));
  const esp_err_t err = set_expander_line(kBacklightEnable, percent > 0);
  if (err != ESP_OK) ESP_LOGW(kTag, "couldn't switch the backlight: %s", esp_err_to_name(err));
}

bool can_turn_to(int degrees) { return degrees % 180 == 0; }

void turn_to(int degrees) {
  lv_display_set_rotation(g_screen.display, degrees == 180 ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_0);
}

bool has_power_chip() { return false; }

esp_codec_dev_handle_t speaker() { return nullptr; }

Lock::Lock() { lv_lock(); }

Lock::~Lock() { lv_unlock(); }

}  // namespace board
