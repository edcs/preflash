//go:build linux

package crash

import (
	"debug/elf"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// buildProgram compiles a small C program, which is the only way to get real DWARF to name.
func buildProgram(t *testing.T, flags ...string) (exe string, inner, outer uint64) {
	t.Helper()
	cc, err := exec.LookPath("cc")
	if err != nil {
		t.Skip("no C compiler")
	}
	if _, err := exec.LookPath("addr2line"); err != nil {
		t.Skip("no addr2line")
	}
	dir := t.TempDir()
	source := filepath.Join(dir, "sample.c")
	require.NoError(t, os.WriteFile(source, []byte(`int pick(int x) {
  return x + 1;
}

int outer_call(int x) {
  return pick(x) * 2;
}

int main(void) {
  return outer_call(0) - 2;
}
`), 0o644))
	exe = filepath.Join(dir, "sample")
	args := append([]string{"-g", "-O0", "-fno-omit-frame-pointer", "-Wl,--build-id"}, flags...)
	out, err := exec.Command(cc, append(args, "-o", exe, source)...).CombinedOutput()
	require.NoError(t, err, string(out))
	file, err := elf.Open(exe)
	require.NoError(t, err)
	defer file.Close()
	symbols, err := file.Symbols()
	require.NoError(t, err)
	for _, s := range symbols {
		switch s.Name {
		case "pick":
			inner = s.Value
		case "outer_call":
			outer = s.Value
		}
	}
	require.NotZero(t, inner)
	require.NotZero(t, outer)
	return exe, inner, outer
}

func reportFor(exe string, load uint64, frames ...uint64) *Report {
	r := &Report{Signal: "SIGSEGV", Address: "0x0", Access: "read", Task: "console", Executable: exe,
		LoadAddress: hexString(load), ImageSize: 0x100000}
	for _, f := range frames {
		r.Frames = append(r.Frames, hexString(f))
	}
	return r
}

func hexString(v uint64) string { return "0x" + string(appendHex(nil, v)) }

func appendHex(b []byte, v uint64) []byte {
	const digits = "0123456789abcdef"
	if v >= 16 {
		b = appendHex(b, v/16)
	}
	return append(b, digits[v%16])
}

func TestAddr2lineNamesFramesOfARealProgram(t *testing.T) {
	exe, pick, outer := buildProgram(t, "-no-pie")
	file, err := elf.Open(exe)
	require.NoError(t, err)
	id := buildID(file)
	file.Close()
	require.Len(t, id, 40)

	// The second frame is a return address, so it points a few bytes into its function.
	r := reportFor(exe, 0, pick+4, outer+8)
	r.UUID = id
	r.Resolve("/", RunCommand)

	assert.False(t, r.Rebuilt)
	require.Len(t, r.Stack, 2)
	assert.Equal(t, "pick", r.Stack[0].Function)
	assert.Equal(t, filepath.Join(filepath.Dir(exe), "sample.c"), r.Stack[0].File)
	assert.Equal(t, 1, r.Stack[0].Line)
	assert.Equal(t, "outer_call", r.Stack[1].Function)
	assert.Equal(t, 5, r.Stack[1].Line)
	assert.Equal(t, "sample.c:1", r.Where(r.Stack[0]))
}

func TestAddr2lineHandlesAPositionIndependentProgram(t *testing.T) {
	exe, pick, outer := buildProgram(t, "-pie", "-fPIE")
	const load = 0x7f0000000000
	r := reportFor(exe, load, load+pick+4, load+outer+8)
	r.Resolve("/", RunCommand)
	require.Len(t, r.Stack, 2)
	assert.Equal(t, "pick", r.Stack[0].Function)
	assert.Equal(t, "outer_call", r.Stack[1].Function)
}

func TestFramesOutsideTheCodeAreLeftOut(t *testing.T) {
	exe, pick, _ := buildProgram(t, "-no-pie")
	r := reportFor(exe, 0, pick+4, 0x7f1234567890)
	r.Resolve("/", RunCommand)
	require.Len(t, r.Stack, 1)
	assert.Equal(t, "pick", r.Stack[0].Function)
}

func TestAnotherBuildIdMeansRebuilt(t *testing.T) {
	exe, pick, _ := buildProgram(t, "-no-pie")
	r := reportFor(exe, 0, pick+4)
	r.UUID = "0000000000000000000000000000000000000000"
	asked := false
	r.Resolve("/", func(string, ...string) ([]byte, error) {
		asked = true
		return nil, nil
	})
	assert.False(t, asked)
	assert.True(t, r.Rebuilt)
	assert.Contains(t, r.Text(), "The app has been rebuilt since, so frames can't be named.")
}

func TestAddr2lineUnknownsStayAddresses(t *testing.T) {
	groups := parseAddr2line("0x0000000000401000\n??\n??:0\n0x0000000000401fff\nfoo(int)\n/a/b.c:7 (discriminator 3)\nbar\n/a/b.h:?\n",
		[]string{"0x401000", "0x401fff"})
	require.Len(t, groups, 2)
	assert.Equal(t, []Frame{{Function: "0x401000"}}, groups[0])
	assert.Equal(t, []Frame{{Function: "foo", File: "/a/b.c", Line: 7}, {Function: "bar", File: "/a/b.h"}}, groups[1])
}

func TestCopyFailsClearlyWithNoTool(t *testing.T) {
	t.Setenv("PREFLASH_PBCOPY", "")
	t.Setenv("WAYLAND_DISPLAY", "wayland-0")
	t.Setenv("PATH", t.TempDir())
	assert.EqualError(t, Copy("x"), "no clipboard tool found: install wl-clipboard or xclip")
}

func TestCopyTriesWaylandThenTheXTools(t *testing.T) {
	bin := t.TempDir()
	out := filepath.Join(t.TempDir(), "copied")
	stub := func(name string) {
		script := "#!/bin/sh\necho " + name + " \"$@\" > '" + out + "'\n/bin/cat >> '" + out + "'\n"
		require.NoError(t, os.WriteFile(filepath.Join(bin, name), []byte(script), 0o755))
	}
	t.Setenv("PREFLASH_PBCOPY", "")
	t.Setenv("PATH", bin)
	stub("xsel")
	require.NoError(t, Copy("one"))
	got, _ := os.ReadFile(out)
	assert.Equal(t, "xsel --clipboard --input\none", string(got))

	stub("xclip")
	require.NoError(t, Copy("two"))
	got, _ = os.ReadFile(out)
	assert.Equal(t, "xclip -selection clipboard\ntwo", string(got))

	stub("wl-copy")
	t.Setenv("WAYLAND_DISPLAY", "")
	require.NoError(t, Copy("three"))
	got, _ = os.ReadFile(out)
	assert.Contains(t, string(got), "xclip", "wl-copy is skipped outside Wayland")

	t.Setenv("WAYLAND_DISPLAY", "wayland-0")
	require.NoError(t, Copy("four"))
	got, _ = os.ReadFile(out)
	assert.Equal(t, "wl-copy\nfour", string(got))
}

func TestDebuggerDefaultsToGdbOnLinux(t *testing.T) {
	t.Setenv("PREFLASH_LLDB", "")
	assert.Equal(t, []string{"gdb", "-p", "42"}, Debugger(42).Args)
	assert.Equal(t, "gdb", DebuggerName())
}
