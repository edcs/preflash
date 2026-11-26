//go:build darwin

package crash

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const namesOutput = `show_lost_time_card(int) (in app) (/p/components/ui/cards.cpp:412)

ns::clock_screen_update() const (in app) (/p/components/ui/clock_screen.cpp:731)

lv_timer_handler (in app) (/p/managed_components/lvgl__lvgl/src/misc/lv_timer.c:90)

`

// answer is what atos says for these frames, a blank line after each address's.
func answer(groups ...[]Frame) string {
	var out strings.Builder
	for _, group := range groups {
		for _, f := range group {
			fmt.Fprintf(&out, "%s() (in app) (%s:%d)\n", f.Function, f.File, f.Line)
		}
		out.WriteString("\n")
	}
	return out.String()
}

// threadStartOutput is what atos says for a task's first frame and the thread start under it.
const threadStartOutput = `task(void*) (in app) (/p/main/a.cpp:3)
xTaskCreatePinnedToCore::$_0::operator()() const (in app) (/r/tasks.cpp:113)
void std::__1::__thread_execute[abi:nqe220106]<a>(b) (in app) (/sdk/thread.h:7)

void* std::__1::__thread_proxy[abi:nqe220106]<std::__1::tuple<a>>(void*) (in app) (/sdk/thread.h:7)

`

func TestAddressesOutsideTheAppAreLeftOutAndCallersLookedUpAtTheirCall(t *testing.T) {
	var name string
	var asked []string
	r := sampleReport(t)
	r.Resolve("/p", func(command string, args ...string) ([]byte, error) {
		name, asked = command, args
		return []byte(namesOutput), nil
	})
	assert.Equal(t, "atos", name)
	assert.Contains(t, asked, r.Executable)
	assert.Contains(t, asked, "0x100000000")
	assert.Contains(t, asked, "0x100001000", "the fault's own address stays exact")
	assert.Contains(t, asked, "0x100001fff", "a return address sits after its call")
	assert.NotContains(t, asked, "0x18fffffff")
}

// atos reads a dSYM and not the debug map in the objects, so one has to exist.
func TestDebugSymbolsAreGatheredOnceForAtos(t *testing.T) {
	r := sampleReport(t)
	dwarf := filepath.Join(r.Executable+".dSYM", "Contents", "Resources", "DWARF", filepath.Base(r.Executable))
	var commands [][]string
	runner := func(name string, args ...string) ([]byte, error) {
		commands = append(commands, append([]string{name}, args...))
		if name == "dsymutil" {
			require.NoError(t, os.MkdirAll(filepath.Dir(dwarf), 0o755))
			require.NoError(t, os.WriteFile(dwarf, nil, 0o644))
		}
		return []byte(namesOutput), nil
	}
	r.Resolve("/p", runner)
	require.Len(t, commands, 2)
	assert.Equal(t, []string{"dsymutil", r.Executable, "-o", r.Executable + ".dSYM"}, commands[0])
	assert.Equal(t, "atos", commands[1][0])
	assert.Contains(t, commands[1], dwarf)

	commands = nil
	r.Resolve("/p", runner)
	require.Len(t, commands, 1, "already gathered for this executable")
	assert.Equal(t, "atos", commands[0][0])
}
