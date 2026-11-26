//go:build linux

package crash

import (
	"fmt"
	"strings"
)

// answer is what addr2line says for these frames, each address headed by itself.
func answer(groups ...[]Frame) string {
	var out strings.Builder
	for i, group := range groups {
		fmt.Fprintf(&out, "0x%016x\n", 0x100001000+i)
		for _, f := range group {
			fmt.Fprintf(&out, "%s()\n%s:%d\n", f.Function, f.File, f.Line)
		}
	}
	return out.String()
}

// namesOutput is addr2line's answer for sampleReport's three frames in the app.
const namesOutput = `0x0000000100001000
show_lost_time_card(int)
/p/components/ui/cards.cpp:412
0x0000000100001fff
ns::clock_screen_update() const
/p/components/ui/clock_screen.cpp:731
0x0000000100002fff
lv_timer_handler
/p/managed_components/lvgl__lvgl/src/misc/lv_timer.c:90
`

// threadStartOutput is addr2line's answer for a task's first frame and the thread start under it.
const threadStartOutput = `0x0000000100001000
task(void*)
/p/main/a.cpp:3
xTaskCreatePinnedToCore::$_0::operator()() const
/r/tasks.cpp:113
std::thread::_State_impl<std::thread::_Invoker<std::tuple<a> > >::_M_run()
/usr/include/c++/15/bits/std_thread.h:7
0x0000000100002fff
execute_native_thread_routine
/usr/include/c++/15/thread:7
`
