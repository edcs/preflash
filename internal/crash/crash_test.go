package crash

import (
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func fakeAtos(output string) Runner {
	return func(string, ...string) ([]byte, error) { return []byte(output), nil }
}

func sampleReport(t *testing.T) *Report {
	t.Helper()
	exe := filepath.Join(t.TempDir(), "app")
	require.NoError(t, os.WriteFile(exe, []byte("not read"), 0o755))
	return &Report{
		PID: 42, Signal: "SIGSEGV", Address: "0x0", Access: "read", Task: "lvgl",
		Executable: exe, LoadAddress: "0x100000000", ImageSize: 0x10000, UptimeMs: 3000,
		Frames: []string{"0x100001000", "0x100002000", "0x190000000", "0x100003000"},
	}
}

const atosOutput = `show_lost_time_card(int) (in app) (/p/components/ui/cards.cpp:412)

ns::clock_screen_update() const (in app) (/p/components/ui/clock_screen.cpp:731)

lv_timer_handler (in app) (/p/managed_components/lvgl__lvgl/src/misc/lv_timer.c:90)

`

func symbolised(t *testing.T) *Report {
	t.Helper()
	r := sampleReport(t)
	r.Resolve("/p", fakeAtos(atosOutput))
	return r
}

func TestSymbolisedFramesTieToSource(t *testing.T) {
	r := symbolised(t)
	require.Len(t, r.Stack, 3)
	assert.Equal(t, Frame{Function: "show_lost_time_card", File: "/p/components/ui/cards.cpp", Line: 412}, r.Stack[0])
	assert.Equal(t, "ns::clock_screen_update", r.Stack[1].Function)
	assert.Equal(t, "cards.cpp:412", r.Where(r.Stack[0]))
	assert.Equal(t, "lvgl", r.Where(r.Stack[2]))
}

func TestAddressesOutsideTheAppAreLeftOutAndCallersLookedUpAtTheirCall(t *testing.T) {
	var name string
	var asked []string
	r := sampleReport(t)
	r.Resolve("/p", func(command string, args ...string) ([]byte, error) {
		name, asked = command, args
		return []byte(atosOutput), nil
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
		return []byte(atosOutput), nil
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

func TestNamesLoseWhatNobodyTyped(t *testing.T) {
	assert.Equal(t, "null_command", functionName("(anonymous namespace)::null_command(int, char**)"))
	assert.Equal(t, "preflash::line_task", functionName("preflash::(anonymous namespace)::line_task(void*)"))
}

func TestTheThreadStartIsLeftOffTheStack(t *testing.T) {
	r := sampleReport(t)
	r.Frames = []string{"0x100001000", "0x100002000"}
	r.Resolve("/p", fakeAtos(`task(void*) (in app) (/p/main/a.cpp:3)
xTaskCreatePinnedToCore::$_0::operator()() const (in app) (/r/tasks.cpp:113)
void std::__1::__thread_execute[abi:nqe220106]<a>(b) (in app) (/sdk/thread.h:7)

void* std::__1::__thread_proxy[abi:nqe220106]<std::__1::tuple<a>>(void*) (in app) (/sdk/thread.h:7)

`))
	require.Len(t, r.Stack, 1)
}

func TestInlinedFramesEachGetARow(t *testing.T) {
	r := sampleReport(t)
	r.Frames = []string{"0x100001000", "0x100002000"}
	r.Resolve("/p", fakeAtos(`inner() (in app) (/p/main/a.cpp:3)
outer() (in app) (/p/main/a.cpp:9)

task(void*) (in app) (/p/main/a.cpp:20)

`))
	require.Len(t, r.Stack, 3)
	assert.Equal(t, "outer", r.Stack[1].Function)
}

// A leaf function saves no frame, so its caller is only in the link register.
func TestTheLinkRegisterFillsInALeafFunctionsCaller(t *testing.T) {
	r := sampleReport(t)
	r.Frames = []string{"0x100001000", "0x100003000"}
	r.LR = "0x100002000"
	r.Resolve("/p", fakeAtos(`leaf() (in app) (/p/main/a.cpp:3)

caller() (in app) (/p/main/a.cpp:9)

task(void*) (in app) (/p/main/a.cpp:20)

`))
	require.Len(t, r.Stack, 3)
	assert.Equal(t, "caller", r.Stack[1].Function)
}

// In anything else it's left over from the last call, which came from the same function.
func TestAStaleLinkRegisterAddsNoRow(t *testing.T) {
	r := sampleReport(t)
	r.Frames = []string{"0x100001000", "0x100003000"}
	r.LR = "0x100001010"
	r.Resolve("/p", fakeAtos(`worker() (in app) (/p/main/a.cpp:3)

worker() (in app) (/p/main/a.cpp:4)

task(void*) (in app) (/p/main/a.cpp:20)

`))
	require.Len(t, r.Stack, 2)
	assert.Equal(t, "task", r.Stack[1].Function)
}

func TestTheFaultSitsOnTheFirstFrameFromTheProject(t *testing.T) {
	r := sampleReport(t)
	r.Frames = []string{"0x100001000", "0x100002000"}
	r.Signal, r.Kind, r.Message = "SIGABRT", "unsupported", "esp_x is not supported in the simulator: no"
	r.Resolve("/p", fakeAtos(`preflash_unsupported(char const*, char const*) (in app) (/root/runtime/src/os/fail.cpp:14)

draw() (in app) (/p/main/draw.cpp:7)

`))
	assert.Equal(t, 1, r.At)
	assert.Equal(t, "main/draw.cpp:7", r.Location)
	assert.Equal(t, "esp_x is not supported in the simulator: no", r.Title)
	assert.Equal(t, "unsupported call on the lvgl task", r.Headline())
}

func TestTitles(t *testing.T) {
	cases := map[string]struct {
		edit           func(*Report)
		title, summary string
	}{
		"null read":  {func(r *Report) {}, "Null pointer read in show_lost_time_card", "memory error"},
		"null write": {func(r *Report) { r.Access = "write" }, "Null pointer write in show_lost_time_card", "memory error"},
		"wild read":  {func(r *Report) { r.Address = "0xdeadbeef" }, "Bad memory read at 0xdeadbeef in show_lost_time_card", "memory error"},
		"near null":  {func(r *Report) { r.Address = "0x40" }, "Null pointer read in show_lost_time_card", "memory error"},
		"jump":       {func(r *Report) { r.Access = "execute" }, "Jump to null in show_lost_time_card", "memory error"},
		"abort":      {func(r *Report) { r.Signal = "SIGABRT" }, "abort() called in show_lost_time_card", "abort"},
		"illegal":    {func(r *Report) { r.Signal = "SIGILL" }, "Illegal instruction in show_lost_time_card", "illegal instruction"},
		"maths":      {func(r *Report) { r.Signal = "SIGFPE" }, "Arithmetic fault in show_lost_time_card", "arithmetic fault"},
		"trap":       {func(r *Report) { r.Signal = "SIGTRAP" }, "Trap in show_lost_time_card", "trap"},
		"check": {func(r *Report) { r.Signal, r.Kind, r.Message = "SIGABRT", "error_check", "ESP_ERR_NO_MEM" },
			"ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM", "failed check"},
		"system abort": {func(r *Report) { r.Signal, r.Kind, r.Message = "SIGABRT", "system_abort", "heap corrupt" },
			"esp_system_abort: heap corrupt", "system abort"},
		"watchdog": {func(r *Report) {
			r.Signal, r.Kind, r.Message = "SIGABRT", "watchdog", "these tasks didn't reset in time: a"
		},
			"Task watchdog: these tasks didn't reset in time: a", "task watchdog"},
	}
	for name, test := range cases {
		t.Run(name, func(t *testing.T) {
			r := sampleReport(t)
			test.edit(r)
			r.Resolve("/p", fakeAtos(atosOutput))
			assert.Equal(t, test.title, r.Title)
			assert.Equal(t, test.summary+" on the lvgl task", r.Headline())
		})
	}
}

func TestACrashOffAnyTaskSaysSo(t *testing.T) {
	r := symbolised(t)
	r.Task = ""
	assert.Equal(t, "memory error on a host thread", r.Headline())
}

func TestAnAppBuiltSinceLeavesAddresses(t *testing.T) {
	r := sampleReport(t)
	r.UUID = "AAAAAAAA-0000-0000-0000-000000000000"
	asked := false
	r.Resolve("/p", func(string, ...string) ([]byte, error) {
		asked = true
		return nil, nil
	})
	assert.False(t, asked)
	assert.True(t, r.Rebuilt)
	assert.Equal(t, "Null pointer read", r.Title)
	require.NotEmpty(t, r.Stack)
	assert.Equal(t, "0x100001000", r.Stack[0].Function)
	assert.Contains(t, r.Text(), "rebuilt since")
}

func TestNoAtosLeavesAddresses(t *testing.T) {
	r := sampleReport(t)
	r.Resolve("/p", func(string, ...string) ([]byte, error) { return nil, errors.New("no atos") })
	require.NotEmpty(t, r.Stack)
	assert.Equal(t, "0x100001000", r.Stack[0].Function)
}

const asanReport = `=================================================================
==50197==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000984 at pc 0x10 bp 0x1 sp 0x2
WRITE of size 1 at 0x602000000984 thread T0
    #0 0x00010452888c in leaf(char*) /p/main/a.cpp:9:40
    #1 0x0001045288bc in mid() /p/main/a.cpp:10
    #2 0x0001877afe7c in start+0x1a1c (dyld:arm64e+0x31e7c)

0x602000000984 is located 4 bytes after 16-byte region [0x602000000970,0x602000000980)
allocated by thread T0 here:
    #0 0x000104cad274 in malloc+0x78 (libclang_rt.asan_osx_dynamic.dylib:arm64e+0x41274)
    #1 0x0001045288b4 in mid() /p/main/a.cpp:10

SUMMARY: AddressSanitizer: heap-buffer-overflow /p/main/a.cpp:9:40 in leaf(char*)
`

const tsanReport = `==================
WARNING: ThreadSanitizer: data race (pid=50203)
  Write of size 4 at 0x0001041f8000 by thread T2:
    #0 t(void*) /p/main/race.cpp:11:3 (tsan:arm64+0x1000007c4)
    #1 <null> <null> (libsystem_pthread.dylib:arm64e+0x6c0c)

  Previous write of size 4 at 0x0001041f8000 by thread T1:
    #0 t(void*) /p/main/race.cpp:12 (tsan:arm64+0x1000007c4)

SUMMARY: ThreadSanitizer: data race /p/main/race.cpp:11:3 in t(void*)
==================
`

func TestAddressSanitizerReportsLeadTheReport(t *testing.T) {
	r := sampleReport(t)
	r.Signal, r.Sanitizer = "SIGABRT", asanReport
	r.Resolve("/p", fakeAtos(""))
	assert.Equal(t, "heap-buffer-overflow in leaf", r.Title)
	assert.Equal(t, "memory error on the lvgl task", r.Headline())
	assert.Equal(t, "main/a.cpp:9", r.Location)
	require.Len(t, r.Stack, 2, "the first stack only, and only what has source")
	assert.Equal(t, Frame{Function: "leaf", File: "/p/main/a.cpp", Line: 9}, r.Stack[0])
	assert.Contains(t, r.Text(), "0x602000000984 is located 4 bytes after")
}

func TestThreadSanitizerReportsLeadTheReport(t *testing.T) {
	r := sampleReport(t)
	r.Signal, r.Sanitizer = "SIGABRT", tsanReport
	r.Resolve("/p", fakeAtos(""))
	assert.Equal(t, "data race in t", r.Title)
	assert.Equal(t, "data race on the lvgl task", r.Headline())
	require.Len(t, r.Stack, 1)
	assert.Equal(t, 11, r.Stack[0].Line)
}

func TestASummaryWithNoSourceStillGivesTheKind(t *testing.T) {
	r := sampleReport(t)
	r.Signal = "SIGABRT"
	r.Sanitizer = "SUMMARY: ThreadSanitizer: data race (app:arm64+0x100048f3c) in dispatch+0x70\n"
	r.Resolve("/p", fakeAtos(atosOutput))
	assert.Equal(t, "data race in show_lost_time_card", r.Title)
}

func TestAtosBeatsASanitizersBareFileNames(t *testing.T) {
	r := sampleReport(t)
	r.Signal, r.Sanitizer = "SIGABRT", asanReport
	r.Resolve("/p", fakeAtos(atosOutput))
	assert.Equal(t, "/p/components/ui/cards.cpp", r.Stack[0].File)
	assert.Equal(t, "heap-buffer-overflow in show_lost_time_card", r.Title)
}

func TestTextHasEverythingAPersonWouldPaste(t *testing.T) {
	text := symbolised(t).Text()
	for _, want := range []string{
		"Null pointer read in show_lost_time_card",
		"components/ui/cards.cpp:412",
		"memory error on the lvgl task",
		"› show_lost_time_card",
		"clock_screen.cpp:731",
	} {
		assert.Contains(t, text, want)
	}
}

func TestReadTakesWhatTheRuntimeWrites(t *testing.T) {
	path := filepath.Join(t.TempDir(), "crash.json")
	require.NoError(t, os.WriteFile(path, []byte(`{"version":1,"pid":7,"signal":"SIGABRT","task":"main","frames":["0x1"],"load_address":"0x1","lr":"0x2","uuid":"U","image_size":9,"executable":"/a","uptime_ms":5,"time":6,"kind":"k","message":"m","access":"","address":"0x0","code":0}`), 0o644))
	r, err := Read(path)
	require.NoError(t, err)
	assert.Equal(t, 7, r.PID)
	assert.Equal(t, "main", r.Task)
	assert.Equal(t, int64(5), r.UptimeMs)
	assert.Equal(t, int64(6), r.Time)
}

func TestReadSaysWhatsWrong(t *testing.T) {
	_, err := Read(filepath.Join(t.TempDir(), "none.json"))
	assert.ErrorIs(t, err, os.ErrNotExist)
	path := filepath.Join(t.TempDir(), "crash.json")
	require.NoError(t, os.WriteFile(path, []byte("{"), 0o644))
	_, err = Read(path)
	assert.ErrorContains(t, err, path)
}

func TestToolsHonourTheirOverrides(t *testing.T) {
	t.Setenv("PREFLASH_LLDB", "/stub/lldb --flag")
	assert.Equal(t, []string{"/stub/lldb", "--flag", "-p", "42"}, Debugger(42).Args)

	t.Setenv("EDITOR", "/stub/edit --wait")
	assert.Equal(t, []string{"/stub/edit", "--wait", "+12", "/p/a.cpp"}, Editor("/p/a.cpp", 12).Args)

	t.Setenv("EDITOR", "code --wait")
	assert.Equal(t, []string{"code", "--wait", "-g", "/p/a.cpp:12"}, Editor("/p/a.cpp", 12).Args)

	t.Setenv("EDITOR", "")
	t.Setenv("VISUAL", "")
	assert.Equal(t, "vi", Editor("/p/a.cpp", 12).Args[0])
}

func TestCopyGoesThroughItsCommand(t *testing.T) {
	out := filepath.Join(t.TempDir(), "copied")
	script := filepath.Join(t.TempDir(), "copy")
	require.NoError(t, os.WriteFile(script, []byte("#!/bin/sh\ncat > '"+out+"'\n"), 0o755))
	t.Setenv("PREFLASH_PBCOPY", script)
	require.NoError(t, Copy("the report"))
	got, err := os.ReadFile(out)
	require.NoError(t, err)
	assert.Equal(t, "the report", string(got))
}
