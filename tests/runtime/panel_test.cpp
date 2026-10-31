#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "preflash/panel.h"
#include "preflash/touch.h"

namespace fs = std::filesystem;
namespace panel = preflash::panel;
namespace touch = preflash::touch;

namespace {

// Not square, so that swapped axes can't pass by accident.
constexpr int kWidth = 16;
constexpr int kHeight = 8;
constexpr uint16_t kRed = 0xf800;

class TestBoard : public testing::Environment {
 public:
  void SetUp() override {
    path_ = fs::temp_directory_path() / ("preflash_panel_" + std::to_string(getpid()) + ".json");
    std::ofstream(path_) << R"({"name": "Test", "display": {"width": 16, "height": 8}, "i2c": [],
                               "backlight": {"pin": 4, "active_low": true}, "audio_codec": "none"})";
    setenv("PREFLASH_BOARD", path_.c_str(), 1);
  }
  void TearDown() override { fs::remove(path_); }

 private:
  fs::path path_;
};

const auto* const g_board = testing::AddGlobalTestEnvironment(new TestBoard);

esp_lcd_panel_handle_t virtual_panel() {
  esp_lcd_panel_handle_t handle = nullptr;
  esp_lcd_panel_io_handle_t io = nullptr;
  const bsp_display_config_t config = {};
  EXPECT_EQ(bsp_display_new(&config, &handle, &io), ESP_OK);
  return handle;
}

std::vector<uint8_t> current_frame() {
  std::vector<uint8_t> frame;
  panel::take_frame(&frame);
  EXPECT_EQ(frame.size(), static_cast<size_t>(kWidth * kHeight * 2));
  return frame;
}

uint16_t pixel(const std::vector<uint8_t>& frame, int x, int y) {
  uint16_t value;
  std::memcpy(&value, frame.data() + (y * kWidth + x) * 2, sizeof value);
  return value;
}

void clear_panel() {
  const std::vector<uint16_t> black(kWidth * kHeight, 0);
  panel::draw(0, 0, kWidth, kHeight, black.data());
}

}  // namespace

TEST(Panel, DrawBitmapWritesPixels) {
  clear_panel();
  const uint16_t px[] = {0x1111, 0x2222, 0x3333, 0x4444};
  ASSERT_EQ(esp_lcd_panel_draw_bitmap(virtual_panel(), 1, 1, 3, 3, px), ESP_OK);

  const auto frame = current_frame();
  EXPECT_EQ(pixel(frame, 1, 1), 0x1111);
  EXPECT_EQ(pixel(frame, 2, 1), 0x2222);
  EXPECT_EQ(pixel(frame, 1, 2), 0x3333);
  EXPECT_EQ(pixel(frame, 2, 2), 0x4444);
  // The end is exclusive, so these stay as they were.
  EXPECT_EQ(pixel(frame, 3, 1), 0);
  EXPECT_EQ(pixel(frame, 1, 3), 0);
  EXPECT_EQ(pixel(frame, 0, 1), 0);
  EXPECT_EQ(pixel(frame, 1, 0), 0);
}

TEST(Panel, DrawBitmapRejectsBadCalls) {
  const uint16_t px = 0x1111;
  EXPECT_EQ(esp_lcd_panel_draw_bitmap(nullptr, 0, 0, 1, 1, &px), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_lcd_panel_draw_bitmap(virtual_panel(), 1, 1, 1, 2, &px), ESP_ERR_INVALID_ARG);
}

TEST(Panel, DrawClipsAtTheEdgeAndKeepsTheSourceStride) {
  clear_panel();
  const uint16_t px[] = {1, 2, 3, 4};
  panel::draw(kWidth - 1, kHeight - 2, kWidth + 1, kHeight, px);

  const auto frame = current_frame();
  EXPECT_EQ(pixel(frame, kWidth - 1, kHeight - 2), 1);
  EXPECT_EQ(pixel(frame, kWidth - 1, kHeight - 1), 3);
}

TEST(Panel, TakeFrameReportsEachChangeOnce) {
  std::vector<uint8_t> frame;
  const uint16_t px = 0x1234;
  panel::draw(0, 0, 1, 1, &px);
  EXPECT_TRUE(panel::take_frame(&frame));
  EXPECT_FALSE(panel::take_frame(&frame));
}

TEST(Panel, SavesABitmapOfThePanelsSize) {
  clear_panel();
  const uint16_t top_left = kRed;
  panel::draw(0, 0, 1, 1, &top_left);
  const fs::path path = fs::temp_directory_path() / ("preflash_" + std::to_string(getpid()) + ".bmp");
  panel::save_bmp(path.c_str());

  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bmp((std::istreambuf_iterator<char>(in)), {});
  fs::remove(path);
  ASSERT_GT(bmp.size(), 54u);
  EXPECT_EQ(bmp[0], 'B');
  EXPECT_EQ(bmp[1], 'M');
  int32_t width, height;
  std::memcpy(&width, &bmp[18], sizeof width);
  std::memcpy(&height, &bmp[22], sizeof height);
  EXPECT_EQ(width, kWidth);
  EXPECT_EQ(height, kHeight);
  // Rows run bottom to top, and a pixel is blue, green, red.
  const size_t top_row = 54 + (kHeight - 1) * ((kWidth * 3 + 3) & ~3);
  EXPECT_EQ(bmp[top_row + 0], 0);
  EXPECT_EQ(bmp[top_row + 1], 0);
  EXPECT_EQ(bmp[top_row + 2], 255);
}

TEST(Panel, SavingWhereItCantThrows) {
  EXPECT_THROW(panel::save_bmp("/nonexistent/folder/shot.bmp"), std::runtime_error);
}

TEST(Panel, BrightnessStaysAPercentage) {
  bsp_display_brightness_set(140);
  EXPECT_EQ(panel::brightness(), 100);
  bsp_display_brightness_set(-3);
  EXPECT_EQ(panel::brightness(), 0);
  bsp_display_brightness_set(37);
  EXPECT_EQ(panel::brightness(), 37);
  EXPECT_EQ(bsp_display_brightness_get(), 37);
}

TEST(Panel, RotatedFlushKeepsPhysicalOrientation) {
  bsp_display_cfg_t cfg = {.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG()};
  lv_display_t* disp = bsp_display_start_with_config(&cfg);
  ASSERT_NE(disp, nullptr);

  ASSERT_TRUE(bsp_display_lock(0));
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_t* mark = lv_obj_create(screen);
  lv_obj_remove_style_all(mark);
  lv_obj_set_pos(mark, 0, 0);
  lv_obj_set_size(mark, 3, 2);
  lv_obj_set_style_bg_color(mark, lv_color_hex(0xff0000), 0);
  lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);

  // Where the mark's first and last pixels land on a panel that never turns.
  struct Case {
    lv_display_rotation_t rotation;
    int first_x, first_y, last_x, last_y;
  };
  const Case cases[] = {
      {LV_DISPLAY_ROTATION_0, 0, 0, 2, 1},
      {LV_DISPLAY_ROTATION_90, 0, kHeight - 1, 1, kHeight - 3},
      {LV_DISPLAY_ROTATION_180, kWidth - 1, kHeight - 1, kWidth - 3, kHeight - 2},
      {LV_DISPLAY_ROTATION_270, kWidth - 1, 0, kWidth - 2, 2},
  };
  for (const Case& c : cases) {
    bsp_display_rotate(disp, c.rotation);
    lv_refr_now(disp);
    const auto frame = current_frame();
    int red = 0;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) red += pixel(frame, x, y) == kRed;
    }
    EXPECT_EQ(red, 6) << "rotation " << c.rotation;
    EXPECT_EQ(pixel(frame, c.first_x, c.first_y), kRed) << "rotation " << c.rotation;
    EXPECT_EQ(pixel(frame, c.last_x, c.last_y), kRed) << "rotation " << c.rotation;
  }
  // A device added later doesn't take the place of the touch input.
  lv_indev_t* touch_input = bsp_display_get_input_dev();
  ASSERT_NE(touch_input, nullptr);
  EXPECT_EQ(lv_indev_get_type(touch_input), LV_INDEV_TYPE_POINTER);
  lv_indev_t* added = lv_indev_create();
  EXPECT_EQ(bsp_display_get_input_dev(), touch_input);
  lv_indev_delete(added);
  bsp_display_unlock();
}

TEST(Touch, QuickTapSurvivesBetweenReads) {
  touch::push({5, 6, true});
  touch::push({5, 6, false});

  const auto down = touch::pop();
  ASSERT_TRUE(down.has_value());
  EXPECT_EQ(down->x, 5);
  EXPECT_EQ(down->y, 6);
  EXPECT_TRUE(down->down);
  const auto up = touch::pop();
  ASSERT_TRUE(up.has_value());
  EXPECT_FALSE(up->down);
  EXPECT_FALSE(touch::pop().has_value());
}

TEST(Lock, IsRecursive) {
  ASSERT_TRUE(bsp_display_lock(0));
  ASSERT_TRUE(bsp_display_lock(0));
  bsp_display_unlock();

  // One unlock leaves it held, as the first lock is still open.
  auto try_lock = [] {
    return std::async(std::launch::async, [] {
      const bool taken = lvgl_port_lock(20);
      if (taken) lvgl_port_unlock();
      return taken;
    });
  };
  EXPECT_FALSE(try_lock().get());
  bsp_display_unlock();
  EXPECT_TRUE(try_lock().get());
}
