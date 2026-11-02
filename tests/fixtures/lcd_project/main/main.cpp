// Firmware for tests/integration/lcd.sh: the 3.49 V2 panel, its I2C chips and a serial console of
// its own, with no board package and no esp_lvgl_port. Each command below is one thing the
// script checks from outside.
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace {

constexpr int kPanelWidth = 172;
constexpr int kPanelHeight = 640;
constexpr int kChunkRows = 64;

constexpr uint16_t kRed = 0xf800;
constexpr uint16_t kGreen = 0x07e0;
constexpr uint16_t kBlue = 0x001f;
constexpr uint16_t kWhite = 0xffff;

constexpr uint8_t kExpander = 0x20;
constexpr uint8_t kExpanderOutputs = 0x01;
constexpr uint8_t kExpanderDirections = 0x03;
constexpr uint8_t kBacklightEnable = 1 << 1;
constexpr uint8_t kTouch = 0x3b;
constexpr uint8_t kMotion = 0x6b;
constexpr uint8_t kMotionCtrl1 = 0x02;
constexpr uint8_t kMotionCtrl7 = 0x08;
constexpr uint8_t kMotionAccelX = 0x35;

constexpr gpio_num_t kBootKey = GPIO_NUM_0;
constexpr int kBacklightPin = 42;
constexpr uint32_t kPwmTop = 255;
constexpr int kI2cTimeoutMs = 50;
constexpr size_t kLongestLine = 128;

// What the panel shows, high byte first, and what LVGL renders into.
alignas(4) uint16_t g_frame[kPanelWidth * kPanelHeight];
alignas(4) uint16_t g_canvas[kPanelWidth * kPanelHeight];

esp_lcd_panel_handle_t g_panel = nullptr;
SemaphoreHandle_t g_chunk_sent = nullptr;
lv_display_t* g_display = nullptr;
i2c_master_bus_handle_t g_buses[2] = {};
i2c_master_dev_handle_t g_expander_dev = nullptr;
i2c_master_dev_handle_t g_touch_dev = nullptr;
i2c_master_dev_handle_t g_motion_dev = nullptr;
std::atomic<int> g_flushes{0};

// One write per line, so that LVGL's task can't split a line from the console's.
void say(const char* format, ...) {
  char line[kLongestLine + 32];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(line, sizeof line - 1, format, args);
  va_end(args);
  const size_t kept = std::min<size_t>(length, sizeof line - 2);
  line[kept] = '\n';
  usb_serial_jtag_write_bytes(line, kept + 1, portMAX_DELAY);
}

i2c_master_dev_handle_t join(int bus, uint8_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = 400'000;
  i2c_master_dev_handle_t device = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(g_buses[bus], &config, &device));
  return device;
}

esp_err_t put(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
  const uint8_t bytes[] = {reg, value};
  return i2c_master_transmit(device, bytes, sizeof bytes, kI2cTimeoutMs);
}

bool chunk_finished(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(g_chunk_sent, &woken);
  return woken == pdTRUE;
}

int send_frame() {
  int chunks = 0;
  for (int top = 0; top < kPanelHeight; top += kChunkRows, ++chunks) {
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(g_panel, 0, top, kPanelWidth, top + kChunkRows,
                                              &g_frame[top * kPanelWidth]));
    xSemaphoreTake(g_chunk_sent, portMAX_DELAY);
  }
  return chunks;
}

// Where LVGL's x, y lands on the panel, which never turns.
int frame_index(lv_display_rotation_t turn, int x, int y) {
  switch (turn) {
    case LV_DISPLAY_ROTATION_90:
      return (kPanelHeight - 1 - x) * kPanelWidth + y;
    case LV_DISPLAY_ROTATION_180:
      return (kPanelHeight - 1 - y) * kPanelWidth + (kPanelWidth - 1 - x);
    case LV_DISPLAY_ROTATION_270:
      return x * kPanelWidth + (kPanelWidth - 1 - y);
    default:
      return y * kPanelWidth + x;
  }
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
  const lv_display_rotation_t turn = lv_display_get_rotation(display);
  const int width = lv_area_get_width(area);
  const int height = lv_area_get_height(area);
  const auto* source = reinterpret_cast<const uint16_t*>(pixels);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) g_frame[frame_index(turn, x, y)] = __builtin_bswap16(*source++);
  }
  send_frame();
  ++g_flushes;
  lv_display_flush_ready(display);
}

void read_touch(lv_indev_t*, lv_indev_data_t* data) {
  static constexpr uint8_t query[] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x0e, 0, 0, 0};
  static bool was_down = false;
  uint8_t reply[6] = {};
  const esp_err_t err =
      i2c_master_transmit_receive(g_touch_dev, query, sizeof query, reply, sizeof reply, kI2cTimeoutMs);
  const bool down = err == ESP_OK && reply[1] > 0;
  if (down) {
    data->point.x = reply[4] << 8 | reply[5];
    data->point.y = kPanelHeight - 1 - (reply[2] << 8 | reply[3]);
    if (!was_down) say("touch %d %d", static_cast<int>(data->point.x), static_cast<int>(data->point.y));
  }
  data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  was_down = down;
}

uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void lvgl_task(void*) {
  for (;;) {
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void start_i2c() {
  const struct {
    gpio_num_t sda, scl;
  } pins[] = {{GPIO_NUM_47, GPIO_NUM_48}, {GPIO_NUM_17, GPIO_NUM_18}};
  for (int port = 0; port < 2; ++port) {
    i2c_master_bus_config_t config = {};
    config.i2c_port = port;
    config.sda_io_num = pins[port].sda;
    config.scl_io_num = pins[port].scl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &g_buses[port]));
  }
  g_expander_dev = join(0, kExpander);
  g_motion_dev = join(0, kMotion);
  g_touch_dev = join(1, kTouch);
}

void start_backlight() {
  ESP_ERROR_CHECK(put(g_expander_dev, kExpanderDirections, static_cast<uint8_t>(~kBacklightEnable)));
  ESP_ERROR_CHECK(put(g_expander_dev, kExpanderOutputs, 0));

  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.timer_num = LEDC_TIMER_1;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.freq_hz = 50'000;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));
  ledc_channel_config_t channel = {};
  channel.gpio_num = kBacklightPin;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_2;
  channel.timer_sel = LEDC_TIMER_1;
  ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void start_panel() {
  g_chunk_sent = xSemaphoreCreateBinary();

  spi_bus_config_t bus = {};
  bus.sclk_io_num = 10;
  bus.data0_io_num = 11;
  bus.data1_io_num = 12;
  bus.data2_io_num = 13;
  bus.data3_io_num = 14;
  bus.max_transfer_sz = kChunkRows * kPanelWidth * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t link = {};
  link.cs_gpio_num = 9;
  link.dc_gpio_num = -1;
  link.pclk_hz = 40'000'000;
  link.on_color_trans_done = chunk_finished;
  link.flags.quad_mode = true;
  esp_lcd_panel_io_handle_t io = nullptr;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &link, &io));

  axs15231b_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = true;
  esp_lcd_panel_dev_config_t device = {};
  device.reset_gpio_num = -1;
  device.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  device.bits_per_pixel = 16;
  device.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io, &device, &g_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel, true));
}

// A black screen with a red square in LVGL's top left corner, so that a turn shows where it went.
void start_lvgl() {
  lv_init();
  lv_tick_set_cb(uptime_ms);

  g_display = lv_display_create(kPanelWidth, kPanelHeight);
  lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(g_display, g_canvas, nullptr, sizeof g_canvas, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(g_display, flush);

  lv_indev_t* pointer = lv_indev_create();
  lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer, read_touch);
  lv_indev_set_display(pointer, g_display);

  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(screen, [](lv_event_t*) { say("clicked"); }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* square = lv_obj_create(screen);
  lv_obj_remove_flag(square, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(square, 30, 30);
  lv_obj_set_pos(square, 0, 0);
  lv_obj_set_style_bg_color(square, lv_color_hex(0xff0000), 0);
  lv_obj_set_style_bg_opa(square, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(square, 0, 0);
  lv_obj_set_style_radius(square, 0, 0);

  lv_refr_now(g_display);
  ESP_ERROR_CHECK(xTaskCreate(lvgl_task, "lvgl", 8192, nullptr, 2, nullptr) == pdPASS ? ESP_OK : ESP_FAIL);
}

void show_pattern(int) {
  lv_lock();
  for (int y = 0; y < kPanelHeight; ++y) {
    for (int x = 0; x < kPanelWidth; ++x) {
      const bool right = x >= kPanelWidth / 2;
      const uint16_t colour = y < kPanelHeight / 2 ? (right ? kGreen : kRed) : (right ? kBlue : kWhite);
      g_frame[y * kPanelWidth + x] = __builtin_bswap16(colour);
    }
  }
  const int chunks = send_frame();
  lv_unlock();
  say("pattern drawn in %d chunks", chunks);
}

void redraw(int) {
  lv_lock();
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(g_display);
  lv_unlock();
  say("redrawn");
}

void rotate(int quarter_turns) {
  lv_lock();
  lv_display_set_rotation(g_display, static_cast<lv_display_rotation_t>(quarter_turns));
  lv_refr_now(g_display);
  lv_unlock();
  say("rotated %d", quarter_turns);
}

// LVGL's task would flush at once if the lock didn't hold it back.
void hold_lock(int) {
  constexpr TickType_t settle = pdMS_TO_TICKS(200);
  lv_lock();
  const int before = g_flushes;
  lv_obj_invalidate(lv_screen_active());
  vTaskDelay(settle);
  const int during = g_flushes - before;
  lv_unlock();
  vTaskDelay(settle);
  say("flushes during the lock %d, after it %d", during, g_flushes - before - during);
}

void set_backlight(int percent) {
  ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, (100 - percent) * kPwmTop / 100));
  ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2));
  ESP_ERROR_CHECK(put(g_expander_dev, kExpanderOutputs, percent > 0 ? kBacklightEnable : 0));
  say("backlight %d", percent);
}

void read_key(int) {
  gpio_config_t key = {};
  key.pin_bit_mask = 1ULL << kBootKey;
  key.mode = GPIO_MODE_INPUT;
  key.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&key));
  say("key level %d", gpio_get_level(kBootKey));
}

void probe_buses(int) {
  const struct {
    const char* name;
    int bus;
    uint8_t address;
  } probes[] = {{"touch", 0, kTouch}, {"expander", 1, kExpander}, {"expander", 0, kExpander}};
  for (const auto& probe : probes) {
    const uint8_t first_register = 0;
    const esp_err_t err = i2c_master_transmit(join(probe.bus, probe.address), &first_register, 1, kI2cTimeoutMs);
    say("%s on bus %d: %s", probe.name, probe.bus, esp_err_to_name(err));
  }
}

void read_motion(int) {
  ESP_ERROR_CHECK(put(g_motion_dev, kMotionCtrl1, 0x40));  // reads walk up the registers
  ESP_ERROR_CHECK(put(g_motion_dev, kMotionCtrl7, 0x01));  // accelerometer on
  int16_t axes[3];
  ESP_ERROR_CHECK(i2c_master_transmit_receive(g_motion_dev, &kMotionAccelX, 1, reinterpret_cast<uint8_t*>(axes),
                                              sizeof axes, kI2cTimeoutMs));
  say("accel %d %d %d", axes[0], axes[1], axes[2]);
}

void tell_reset_reason(int) { say("reset reason %d", static_cast<int>(esp_reset_reason())); }

void restart(int) { esp_restart(); }

constexpr struct {
  std::string_view name;
  void (*run)(int argument);
} kCommands[] = {
    {"pattern", show_pattern},
    {"redraw", redraw},
    {"rotate", rotate},
    {"lock", hold_lock},
    {"backlight", set_backlight},
    {"key", read_key},
    {"buses", probe_buses},
    {"motion", read_motion},
    {"reason", tell_reset_reason},
    {"restart", restart},
};

void handle(const std::string& line) {
  if (line.starts_with('@')) {
    say("protocol line: %s", line.c_str());
    return;
  }
  const size_t space = line.find(' ');
  const std::string_view name = std::string_view(line).substr(0, space);
  const int argument = space == std::string::npos ? 0 : std::atoi(line.c_str() + space);
  for (const auto& command : kCommands) {
    if (command.name == name) {
      command.run(argument);
      return;
    }
  }
  say("unknown command");
}

}  // namespace

extern "C" void app_main(void) {
  usb_serial_jtag_driver_config_t usb = {};
  usb.tx_buffer_size = 512;
  usb.rx_buffer_size = 512;
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));

  start_i2c();
  start_backlight();
  start_panel();
  start_lvgl();
  say("ready");

  std::string line;
  for (;;) {
    char piece[64];
    const int got = usb_serial_jtag_read_bytes(piece, sizeof piece, portMAX_DELAY);
    for (int i = 0; i < got; ++i) {
      if (piece[i] == '\n') {
        if (line.ends_with('\r')) line.pop_back();
        handle(line);
        line.clear();
      } else if (line.size() < kLongestLine) {
        line += piece[i];
      }
    }
  }
}
