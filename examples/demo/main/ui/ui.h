// The screen: a home page of tiles, and a page behind each tile. Pages are built when they
// open and thrown away when they close, so only the visible one costs anything.
#pragma once

#include "lvgl.h"
#include "monitor.h"
#include "network.h"
#include "update.h"

namespace ui {

struct Page {
  const char* name;
  const char* title;
  void (*build)(lv_obj_t* body);
  void (*refresh)();  // runs on LVGL's task a few times a second
};

extern const Page kHome;
extern const Page kTime;
extern const Page kMotion;
extern const Page kPower;
extern const Page kSound;
extern const Page kNetwork;
extern const Page kUpdate;
extern const Page kSettings;
extern const Page kFaults;

void start();

// False if there's no page by that name. Safe from any task.
bool open_by_name(const char* name);

// Opens a page from one of LVGL's own callbacks.
void open(const Page& page);

// The screen is short of width on the long thin panel, and pages lay out to suit.
bool narrow();

// How many rows of the faults page are showing a problem.
int faults_showing();

// Words for each state, shared by the pages and the console.
const char* describe(rtc::Status status);
const char* describe(network::Wifi wifi);
const char* describe(network::Fetch fetch);
const char* describe(update::Check check);

}  // namespace ui
