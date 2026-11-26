//go:build linux

package tui

// sampleNames is what addr2line says for sampleCrash's three frames.
const sampleNames = `0x0000000100001000
show_lost_time_card(int)
/p/components/ui/cards.cpp:412
0x0000000100001fff
clock_screen_update()
/p/components/ui/clock_screen.cpp:731
0x0000000100002fff
lv_timer_handler
/p/managed_components/lvgl__lvgl/src/misc/lv_timer.c:90
`
