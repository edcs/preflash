//go:build darwin

package tui

// sampleNames is what atos says for sampleCrash's three frames.
const sampleNames = `show_lost_time_card(int) (in app) (/p/components/ui/cards.cpp:412)

clock_screen_update() (in app) (/p/components/ui/clock_screen.cpp:731)

lv_timer_handler (in app) (/p/managed_components/lvgl__lvgl/src/misc/lv_timer.c:90)

`
