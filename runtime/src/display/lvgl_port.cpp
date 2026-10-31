#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "preflash/board.h"
#include "preflash/fail.h"
#include "preflash/panel.h"
#include "preflash/touch.h"

namespace {

constexpr int kBytesPerPixel = 2;
constexpr uint32_t kLongestSleepMs = 5;

std::recursive_timed_mutex g_lock;
bool g_started = false;

uint32_t tick_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// LVGL draws what a rotated firmware sees, and the panel never turns, so the
// flush puts each area into the panel's own orientation.
void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const int w = lv_area_get_width(area);
  const int h = lv_area_get_height(area);
  const int source_stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
  lv_area_t physical = *area;
  lv_display_rotate_area(disp, &physical);
  const int panel_width = lv_area_get_width(&physical);
  const int panel_stride = panel_width * kBytesPerPixel;

  std::vector<uint8_t> packed(panel_stride * lv_area_get_height(&physical));
  const lv_display_rotation_t rotation = lv_display_get_rotation(disp);
  if (rotation == LV_DISPLAY_ROTATION_0) {
    for (int y = 0; y < h; ++y) {
      std::memcpy(packed.data() + y * panel_stride, px + y * source_stride, panel_stride);
    }
  } else {
    lv_draw_sw_rotate(px, packed.data(), w, h, source_stride, panel_stride, rotation,
                      LV_COLOR_FORMAT_RGB565);
  }
  preflash::panel::draw(physical.x1, physical.y1, physical.x2 + 1, physical.y2 + 1, packed.data());
  lv_display_flush_ready(disp);
}

void read_touch(lv_indev_t*, lv_indev_data_t* data) {
  static preflash::touch::Event last{0, 0, false};
  const auto event = preflash::touch::pop();
  if (event) last = *event;
  data->point.x = last.x;
  data->point.y = last.y;
  data->state = last.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  // Reading on until nothing came keeps a quick tap from waiting a whole period.
  data->continue_reading = event.has_value();
}

void lvgl_task(void*) {
  for (;;) {
    uint32_t wait;
    {
      const std::lock_guard<std::recursive_timed_mutex> guard(g_lock);
      wait = lv_timer_handler();
    }
    // LVGL may say "no timer due", but touches still need reading.
    usleep(std::clamp<uint32_t>(wait, 1, kLongestSleepMs) * 1000);
  }
}

}  // namespace

extern "C" {

esp_err_t lvgl_port_init(const lvgl_port_cfg_t* cfg) {
  if (!cfg) return ESP_ERR_INVALID_ARG;
  const std::lock_guard<std::recursive_timed_mutex> guard(g_lock);
  if (g_started) return ESP_ERR_INVALID_STATE;
  lv_init();
  lv_tick_set_cb(tick_ms);
  if (xTaskCreate(lvgl_task, "taskLVGL", cfg->task_stack, nullptr, cfg->task_priority, nullptr) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  g_started = true;
  return ESP_OK;
}

bool lvgl_port_lock(uint32_t timeout_ms) {
  if (timeout_ms == 0) {
    g_lock.lock();
    return true;
  }
  return g_lock.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void lvgl_port_unlock(void) { g_lock.unlock(); }

lv_display_t* lvgl_port_add_disp(const lvgl_port_display_cfg_t* disp_cfg) {
  if (!disp_cfg) return nullptr;
  const preflash::Board& board = preflash::board();
  if (static_cast<int>(disp_cfg->hres) != board.width ||
      static_cast<int>(disp_cfg->vres) != board.height) {
    preflash_unsupported("lvgl_port_add_disp", "hres and vres must match the board's display");
  }
  if (disp_cfg->monochrome || disp_cfg->flags.direct_mode ||
      (disp_cfg->color_format != LV_COLOR_FORMAT_UNKNOWN &&
       disp_cfg->color_format != LV_COLOR_FORMAT_RGB565)) {
    preflash_unsupported("lvgl_port_add_disp", "only partial or full RGB565 rendering is modelled");
  }
  if (disp_cfg->rotation.swap_xy || disp_cfg->rotation.mirror_x || disp_cfg->rotation.mirror_y) {
    preflash_unsupported("lvgl_port_add_disp", "the panel can't rotate in hardware");
  }

  const std::lock_guard<std::recursive_timed_mutex> guard(g_lock);
  lv_display_t* disp = lv_display_create(board.width, board.height);
  if (!disp) return nullptr;
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

  // A whole screen whatever size firmware asked for: the buffer's size only changes
  // how an area is split, and there's no memory to save here. LVGL keeps it for good.
  const uint32_t size =
      lv_draw_buf_width_to_stride(board.width, LV_COLOR_FORMAT_RGB565) * board.height;
  void* memory = std::malloc(size + LV_DRAW_BUF_ALIGN);
  if (!memory) {
    lv_display_delete(disp);
    return nullptr;
  }
  void* buffer = lv_draw_buf_align(memory, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(disp, buffer, nullptr, size,
                         disp_cfg->flags.full_refresh ? LV_DISPLAY_RENDER_MODE_FULL
                                                      : LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);

  lv_indev_t* pointer = lv_indev_create();
  lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer, read_touch);
  lv_indev_set_display(pointer, disp);
  return disp;
}

}  // extern "C"
