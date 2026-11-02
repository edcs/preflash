#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "preflash/panel.h"

namespace panel = preflash::panel;

namespace {

constexpr uint32_t kLargestDuty = 255;

void start_bus() {
  spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(10, 11, 12, 13, 14, 4096);
  ASSERT_EQ(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), ESP_OK);
}

esp_lcd_panel_handle_t new_panel(esp_lcd_panel_io_handle_t io) {
  esp_lcd_panel_dev_config_t config = {};
  config.reset_gpio_num = -1;
  config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  config.bits_per_pixel = 16;
  esp_lcd_panel_handle_t handle = nullptr;
  EXPECT_EQ(esp_lcd_new_panel_axs15231b(io, &config, &handle), ESP_OK);
  return handle;
}

bool count_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user_ctx) {
  ++*static_cast<int*>(user_ctx);
  return false;
}

esp_lcd_panel_io_handle_t new_io(esp_lcd_panel_io_color_trans_done_cb_t done, void* context) {
  esp_lcd_panel_io_spi_config_t config = AXS15231B_PANEL_IO_QSPI_CONFIG(9, done, context);
  esp_lcd_panel_io_handle_t io = nullptr;
  EXPECT_EQ(esp_lcd_new_panel_io_spi(SPI3_HOST, &config, &io), ESP_OK);
  return io;
}

void start_backlight(uint32_t duty) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_3;
  timer.freq_hz = 50000;
  timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
  ASSERT_EQ(ledc_timer_config(&timer), ESP_OK);
  ledc_channel_config_t channel = {};
  channel.gpio_num = 4;  // the test board's backlight, which is active low
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_1;
  channel.timer_sel = LEDC_TIMER_3;
  channel.duty = duty;
  ASSERT_EQ(ledc_channel_config(&channel), ESP_OK);
}

esp_err_t fails(esp_err_t code) {
  ESP_RETURN_ON_ERROR(code, "test", "failed");
  return ESP_OK;
}

esp_err_t needs(bool ok) {
  ESP_RETURN_ON_FALSE(ok, ESP_ERR_INVALID_STATE, "test", "not ok");
  return ESP_OK;
}

esp_err_t jumps(esp_err_t code) {
  esp_err_t ret = ESP_OK;
  ESP_GOTO_ON_ERROR(code, end, "test", "failed");
  return ESP_FAIL;
end:
  return ret;
}

}  // namespace

TEST(Spi, ABusStartsOnce) {
  start_bus();
  spi_bus_config_t bus = {};
  EXPECT_EQ(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(spi_bus_free(SPI3_HOST), ESP_OK);
  EXPECT_EQ(spi_bus_free(SPI3_HOST), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), ESP_OK);
}

TEST(Spi, TheFlashBusAndBadArgumentsAreRefused) {
  spi_bus_config_t bus = {};
  EXPECT_EQ(spi_bus_initialize(SPI1_HOST, &bus, SPI_DMA_CH_AUTO), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(spi_bus_initialize(SPI2_HOST, nullptr, SPI_DMA_CH_AUTO), ESP_ERR_INVALID_ARG);
}

TEST(PanelIo, NeedsAStartedBus) {
  esp_lcd_panel_io_spi_config_t config = AXS15231B_PANEL_IO_QSPI_CONFIG(9, nullptr, nullptr);
  esp_lcd_panel_io_handle_t io = nullptr;
  EXPECT_EQ(esp_lcd_new_panel_io_spi(SPI3_HOST, &config, &io), ESP_ERR_INVALID_STATE);
}

TEST(PanelIo, RejectsBadArguments) {
  start_bus();
  esp_lcd_panel_io_spi_config_t config = {};
  esp_lcd_panel_io_handle_t io = nullptr;
  EXPECT_EQ(esp_lcd_new_panel_io_spi(SPI3_HOST, nullptr, &io), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_lcd_new_panel_io_spi(SPI3_HOST, &config, nullptr), ESP_ERR_INVALID_ARG);
}

TEST(PanelIo, SaysWhenEachBitmapIsDone) {
  start_bus();
  int done = 0;
  esp_lcd_panel_handle_t handle = new_panel(new_io(count_done, &done));
  const std::vector<uint8_t> pixels(4 * 2);
  EXPECT_EQ(esp_lcd_panel_draw_bitmap(handle, 0, 0, 2, 2, pixels.data()), ESP_OK);
  EXPECT_EQ(done, 1);
  EXPECT_EQ(esp_lcd_panel_draw_bitmap(handle, 0, 2, 2, 4, pixels.data()), ESP_OK);
  EXPECT_EQ(done, 2);
}

TEST(PanelIo, ACallbackCanBeRegisteredLater) {
  start_bus();
  esp_lcd_panel_io_handle_t io = new_io(nullptr, nullptr);
  esp_lcd_panel_handle_t handle = new_panel(io);
  int done = 0;
  const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = count_done};
  ASSERT_EQ(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, &done), ESP_OK);
  const std::vector<uint8_t> pixels(2);
  EXPECT_EQ(esp_lcd_panel_draw_bitmap(handle, 0, 0, 1, 1, pixels.data()), ESP_OK);
  EXPECT_EQ(done, 1);
}

TEST(PanelIo, ARefusedBitmapIsNeverDone) {
  start_bus();
  int done = 0;
  esp_lcd_panel_handle_t handle = new_panel(new_io(count_done, &done));
  EXPECT_EQ(esp_lcd_panel_draw_bitmap(handle, 2, 0, 2, 2, "x"), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(done, 0);
}

TEST(Axs15231b, RejectsBadArguments) {
  start_bus();
  esp_lcd_panel_io_handle_t io = new_io(nullptr, nullptr);
  esp_lcd_panel_dev_config_t config = {};
  esp_lcd_panel_handle_t handle = nullptr;
  EXPECT_EQ(esp_lcd_new_panel_axs15231b(nullptr, &config, &handle), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_lcd_new_panel_axs15231b(io, nullptr, &handle), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_lcd_new_panel_axs15231b(io, &config, nullptr), ESP_ERR_INVALID_ARG);
}

TEST(Axs15231bDeathTest, NamesAColourFormatItCantShow) {
  start_bus();
  esp_lcd_panel_io_handle_t io = new_io(nullptr, nullptr);
  esp_lcd_panel_dev_config_t config = {};
  config.bits_per_pixel = 18;
  esp_lcd_panel_handle_t handle = nullptr;
  EXPECT_DEATH(esp_lcd_new_panel_axs15231b(io, &config, &handle), "esp_lcd_new_panel_axs15231b");
  config.bits_per_pixel = 16;
  config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  EXPECT_DEATH(esp_lcd_new_panel_axs15231b(io, &config, &handle), "esp_lcd_new_panel_axs15231b");
}

TEST(Ledc, AChannelStartsTheBacklightAtItsDuty) {
  start_backlight(kLargestDuty);  // all the time off, on an active-low line
  EXPECT_EQ(panel::brightness_level(), 0);
  start_backlight(0);
  EXPECT_EQ(panel::brightness_level(), 100);
}

TEST(Ledc, ADutyWaitsForItsUpdate) {
  start_backlight(0);
  ASSERT_EQ(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 191), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 100);
  ASSERT_EQ(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 25);
}

TEST(Ledc, AnInvertedOutputLightsTheOtherWayRound) {
  start_backlight(0);
  ledc_channel_config_t channel = {};
  channel.gpio_num = 4;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_1;
  channel.timer_sel = LEDC_TIMER_3;
  channel.duty = 51;
  channel.flags.output_invert = 1;
  ASSERT_EQ(ledc_channel_config(&channel), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 20);
}

TEST(Ledc, ADutyPastFullScaleStopsThere) {
  start_backlight(0);
  ASSERT_EQ(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 256), ESP_OK);
  ASSERT_EQ(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 0);
}

TEST(Ledc, AnotherPinLeavesTheBacklightAlone) {
  start_backlight(0);
  ledc_channel_config_t channel = {};
  channel.gpio_num = 5;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_2;
  channel.timer_sel = LEDC_TIMER_3;
  channel.duty = kLargestDuty;
  ASSERT_EQ(ledc_channel_config(&channel), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 100);
}

TEST(Ledc, ChannelBeforeItsTimerStillSetsTheBacklight) {
  ledc_channel_config_t channel = {};
  channel.gpio_num = 4;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_0;
  channel.timer_sel = LEDC_TIMER_0;
  channel.duty = 0;
  ASSERT_EQ(ledc_channel_config(&channel), ESP_OK);
  EXPECT_EQ(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128), ESP_OK);
  EXPECT_EQ(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), ESP_ERR_INVALID_STATE);
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 5000;
  ASSERT_EQ(ledc_timer_config(&timer), ESP_OK);
  EXPECT_EQ(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 50);
}

TEST(Ledc, ATimerArrivingLaterStartsItsChannels) {
  ledc_channel_config_t channel = {};
  channel.gpio_num = 4;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_0;
  channel.timer_sel = LEDC_TIMER_0;
  channel.duty = 191;
  ASSERT_EQ(ledc_channel_config(&channel), ESP_OK);
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 5000;
  ASSERT_EQ(ledc_timer_config(&timer), ESP_OK);
  EXPECT_EQ(panel::brightness_level(), 25);
}

TEST(Ledc, RefusesWhatTheHardwareLacks) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = LEDC_TIMER_MAX;
  timer.freq_hz = 5000;
  EXPECT_EQ(ledc_timer_config(&timer), ESP_ERR_INVALID_ARG);
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 0;
  EXPECT_EQ(ledc_timer_config(&timer), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(ledc_timer_config(nullptr), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(ledc_channel_config(nullptr), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_MAX, 1), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(ledc_update_duty(LEDC_SPEED_MODE_MAX, LEDC_CHANNEL_0), ESP_ERR_INVALID_ARG);
}

TEST(Ledc, ADutyNeedsAChannel) {
  EXPECT_EQ(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, 1), ESP_ERR_INVALID_STATE);
}

TEST(EspCheck, ReturnsAndJumpsOnFailure) {
  EXPECT_EQ(fails(ESP_OK), ESP_OK);
  EXPECT_EQ(fails(ESP_ERR_NO_MEM), ESP_ERR_NO_MEM);
  EXPECT_EQ(needs(true), ESP_OK);
  EXPECT_EQ(needs(false), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(jumps(ESP_ERR_TIMEOUT), ESP_ERR_TIMEOUT);
  EXPECT_EQ(jumps(ESP_OK), ESP_FAIL);
}
