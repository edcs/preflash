package project

import (
	"bufio"
	"context"
	"fmt"
	"io/fs"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/board"
)

const idfCMake = "cmake_minimum_required(VERSION 3.16)\ninclude($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(demo)\n"

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	require.NoError(t, os.MkdirAll(filepath.Dir(path), 0o755))
	require.NoError(t, os.WriteFile(path, []byte(content), 0o644))
}

func idfProject(t *testing.T, parent string) string {
	t.Helper()
	dir := filepath.Join(parent, "demo")
	writeFile(t, filepath.Join(dir, "CMakeLists.txt"), idfCMake)
	return dir
}

func TestStateDirsDifferPerCheckout(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)

	one, err := Find(idfProject(t, filepath.Join(t.TempDir(), "one")))
	require.NoError(t, err)
	two, err := Find(idfProject(t, filepath.Join(t.TempDir(), "two")))
	require.NoError(t, err)

	assert.NotEqual(t, one.StateDir(), two.StateDir())
	assert.NotEqual(t, one.BuildDir(), two.BuildDir())
	for _, dir := range []string{one.StateDir(), two.StateDir(), one.BuildDir(), two.BuildDir()} {
		assert.True(t, strings.HasPrefix(dir, filepath.Join(home, ".preflash")+string(filepath.Separator)), dir)
		assert.Regexp(t, `/demo-[0-9a-f]{8}$`, dir)
	}
	assert.Equal(t, filepath.Join(home, ".preflash", "state"), filepath.Dir(one.StateDir()))
	assert.Equal(t, filepath.Join(home, ".preflash", "build"), filepath.Dir(one.BuildDir()))
}

func TestSpellingsOfOnePathShareFolders(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	dir := idfProject(t, t.TempDir())
	link := filepath.Join(t.TempDir(), "link")
	require.NoError(t, os.Symlink(dir, link))

	direct, err := Find(dir)
	require.NoError(t, err)
	linked, err := Find(link)
	require.NoError(t, err)

	assert.Equal(t, direct.BuildDir(), linked.BuildDir())
}

func TestNotAnIdfProjectSaysSo(t *testing.T) {
	t.Run("no CMakeLists.txt", func(t *testing.T) {
		dir := t.TempDir()
		_, err := Find(dir)
		require.Error(t, err)
		assert.Contains(t, err.Error(), "not an ESP-IDF project")
		assert.Contains(t, err.Error(), dir)
	})
	t.Run("CMakeLists.txt without project.cmake", func(t *testing.T) {
		dir := t.TempDir()
		writeFile(t, filepath.Join(dir, "CMakeLists.txt"), "project(plain C)\n")
		_, err := Find(dir)
		require.Error(t, err)
		assert.Contains(t, err.Error(), "not an ESP-IDF project")
	})
}

func TestFindErrorsNameTheAbsolutePath(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	_, err := Find(".")
	require.Error(t, err)
	assert.Contains(t, err.Error(), "not an ESP-IDF project")
	assert.NotContains(t, err.Error(), " . ")
	assert.Contains(t, err.Error(), dir+" is not")

	_, err = Find("missing")
	require.Error(t, err)
	assert.Contains(t, err.Error(), filepath.Join(dir, "missing"))
}

func TestHashInsideAnArgumentIsNotAComment(t *testing.T) {
	for name, line := range map[string]string{
		"quoted":  `include("/opt/idf#5/tools/cmake/project.cmake")`,
		"escaped": `set(X "a\"#") include("/opt/idf/tools/cmake/project.cmake")`,
		"bracket": `include([=[/opt/idf#5/tools/cmake/project.cmake]=])`,
	} {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			writeFile(t, filepath.Join(dir, "CMakeLists.txt"), line+"\nproject(demo)\n")
			_, err := Find(dir)
			require.NoError(t, err)
		})
	}
}

func TestCommentedProjectCmakeIsNotAProject(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "CMakeLists.txt"), "# include($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(plain C)\n")
	_, err := Find(dir)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "not an ESP-IDF project")

	writeFile(t, filepath.Join(dir, "CMakeLists.txt"), "#[[ include(project.cmake)\n]] project(plain C)\n")
	_, err = Find(dir)
	require.Error(t, err)

	writeFile(t, filepath.Join(dir, "CMakeLists.txt"), "include($ENV{IDF_PATH}/tools/cmake/project.cmake) # the real one\n")
	_, err = Find(dir)
	require.NoError(t, err)
}

func TestBoardWithNothingNamedIsTheDefault(t *testing.T) {
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)

	b, err := p.Board("")
	require.NoError(t, err)
	assert.Equal(t, "ESP32-S3-Touch-LCD-4B", b.Name)
}

const childBoard = "name: From Rc\nextends: ESP32-S3-Touch-LCD-4B\n"

func TestBoardComesFromRc(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "sim", "board.yaml"), childBoard)
	writeFile(t, filepath.Join(dir, ".preflashrc"), "board: sim/board.yaml\nconsole:\n  prompt: \"demo>\"\n")
	p, err := Find(dir)
	require.NoError(t, err)

	b, err := p.Board("")
	require.NoError(t, err)
	assert.Equal(t, "From Rc", b.Name)
}

func TestBoardFlagWinsOverRc(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, ".preflashrc"), "board: sim/not-there.yaml\n")
	flagBoard := filepath.Join(t.TempDir(), "flag.yaml")
	writeFile(t, flagBoard, "name: From Flag\nextends: ESP32-S3-Touch-LCD-4B\n")
	p, err := Find(dir)
	require.NoError(t, err)

	b, err := p.Board(flagBoard)
	require.NoError(t, err)
	assert.Equal(t, "From Flag", b.Name)
}

func TestBoardNamedByStockName(t *testing.T) {
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)

	b, err := p.Board("ESP32-S3-Touch-LCD-4B")
	require.NoError(t, err)
	assert.Equal(t, 480, b.Display.Width)
}

func TestMissingBoardListsStockBoards(t *testing.T) {
	t.Run("from the flag", func(t *testing.T) {
		p, err := Find(idfProject(t, t.TempDir()))
		require.NoError(t, err)
		_, err = p.Board("no-such-board.yaml")
		require.Error(t, err)
		assert.Contains(t, err.Error(), "no-such-board.yaml")
		assert.Contains(t, err.Error(), "ESP32-S3-Touch-LCD-4B")
	})
	t.Run("from the rc file", func(t *testing.T) {
		dir := idfProject(t, t.TempDir())
		writeFile(t, filepath.Join(dir, ".preflashrc"), "board: sim/gone.yaml\n")
		p, err := Find(dir)
		require.NoError(t, err)
		_, err = p.Board("")
		require.Error(t, err)
		assert.Contains(t, err.Error(), "sim/gone.yaml")
		assert.Contains(t, err.Error(), "ESP32-S3-Touch-LCD-4B")
	})
}

func TestBrokenBoardKeepsItsOwnError(t *testing.T) {
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	path := filepath.Join(t.TempDir(), "typo.yaml")
	writeFile(t, path, "name: Typo\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  backlite:\n    pin: gpio1\n")

	_, err = p.Board(path)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "backlite")
}

func TestUnreadableRcIsAnError(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, ".preflashrc"), "board: [not, a, string\n")
	p, err := Find(dir)
	require.NoError(t, err)

	_, err = p.Board("")
	require.Error(t, err)
	assert.Contains(t, err.Error(), ".preflashrc")
}

func TestEnvironmentForTheApp(t *testing.T) {
	base := []string{"PATH=/bin", "PREFLASH_HEADLESS=1", "IDF_PATH=/real/esp-idf", "PREFLASH_SOCKET=/old.sock",
		"PREFLASH_PORT=/old.tty", "PREFLASH_PORT_LINK=/old-link.tty", "PREFLASH_PORT_FDS=7:8", "PREFLASH_TRACE=1"}
	p := Project{Dir: "/x/demo", id: "demo-12345678"}
	t.Setenv("HOME", "/home/me")

	windowed := p.environment(base, "/repo", "", false)
	assert.Contains(t, windowed, "PATH=/bin")
	assert.Contains(t, windowed, "IDF_PATH=/repo/runtime/idf")
	assert.Contains(t, windowed, "PREFLASH_BOARD=/home/me/.preflash/build/demo-12345678/board.json")
	assert.Contains(t, windowed, "PREFLASH_STATE=/home/me/.preflash/state/demo-12345678")
	assert.Contains(t, windowed, "PREFLASH_PROJECT=demo")
	assert.Contains(t, windowed, "PREFLASH_SOCKET="+p.SocketPath())
	assert.NotContains(t, windowed, "PREFLASH_SOCKET=/old.sock")
	assert.Contains(t, windowed, "PREFLASH_PORT="+p.PortPath())
	for _, stale := range []string{"PREFLASH_PORT=/old.tty", "PREFLASH_PORT_LINK=/old-link.tty", "PREFLASH_PORT_FDS=7:8", "PREFLASH_TRACE=1"} {
		assert.NotContains(t, windowed, stale)
	}
	assert.NotContains(t, windowed, "PREFLASH_HEADLESS=1")
	assert.NotContains(t, windowed, "IDF_PATH=/real/esp-idf")

	assert.Contains(t, p.environment(base, "/repo", "", true), "PREFLASH_HEADLESS=1")
}

func TestEnvironmentNamesTheProjectsLink(t *testing.T) {
	p := Project{Dir: "/work/demo", id: "demo-1234"}
	assert.Contains(t, p.environment(nil, "/repo", "/work/demo/nyx.tty", false), "PREFLASH_PORT_LINK=/work/demo/nyx.tty")
}

func TestPortPathSitsBesideTheSocket(t *testing.T) {
	p := Project{Dir: "/work/demo"}
	assert.Equal(t, strings.TrimSuffix(p.SocketPath(), ".sock")+".tty", p.PortPath())
}

func TestSerialLinkIsRelativeToTheProject(t *testing.T) {
	home := t.TempDir()
	t.Setenv("HOME", home)
	dir := t.TempDir()
	p := Project{Dir: dir}

	link, err := p.serialLink()
	require.NoError(t, err)
	assert.Empty(t, link, "no .preflashrc asks for none")

	for value, want := range map[string]string{
		"sim/nyx.tty":  filepath.Join(dir, "sim", "nyx.tty"),
		"/tmp/nyx.tty": "/tmp/nyx.tty",
		"~/nyx.tty":    filepath.Join(home, "nyx.tty"),
	} {
		require.NoError(t, os.WriteFile(filepath.Join(dir, ".preflashrc"), []byte("serial_link: "+value+"\n"), 0o644))
		link, err := p.serialLink()
		require.NoError(t, err)
		assert.Equal(t, want, link)
	}
}

func TestSocketPathFitsAUnixSocket(t *testing.T) {
	t.Setenv("HOME", "/Users/someone/with/a/fairly/long/home/folder")
	p := Project{Dir: "/some/very/deep/folder/" + strings.Repeat("x", 200)}

	path := p.SocketPath()
	assert.Regexp(t, `/Library/Caches/preflash/[0-9a-f]{12}\.sock$`, path)
	assert.Less(t, len(path), 104)
	assert.NotEqual(t, path, Project{Dir: "/elsewhere"}.SocketPath())
}

func TestSocketPathFallsBackWhenHomeIsLong(t *testing.T) {
	t.Setenv("HOME", "/Users/"+strings.Repeat("x", 100))
	t.Setenv("TMPDIR", "/tmp/short")

	path := Project{Dir: "/x/demo"}.SocketPath()
	assert.Less(t, len(path), socketPathLimit)
	assert.True(t, strings.HasPrefix(path, "/tmp/short/"), path)
	assert.Regexp(t, `[0-9a-f]{12}\.sock$`, path)
}

func TestRunRefusesASocketPathThatCannotFit(t *testing.T) {
	t.Setenv("HOME", "/Users/"+strings.Repeat("x", 100))
	t.Setenv("TMPDIR", "/tmp/"+strings.Repeat("y", 100))

	assert.ErrorContains(t, Project{Dir: "/x/demo"}.checkSocketPath(), "too long")
}

func TestRunRefusesAnAppItCannotTalkTo(t *testing.T) {
	home, err := os.MkdirTemp("/tmp", "pf")
	require.NoError(t, err)
	t.Cleanup(func() { os.RemoveAll(home) })
	t.Setenv("HOME", home)
	p := Project{Dir: "/x/demo"}
	require.NoError(t, os.MkdirAll(filepath.Dir(p.SocketPath()), 0o755))
	listener, err := net.Listen("unix", p.SocketPath())
	require.NoError(t, err)
	defer listener.Close()
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		bufio.NewReader(conn).ReadBytes('\n')
		fmt.Fprintln(conn, `{"id":1,"ok":true,"protocol":99}`)
	}()

	assert.ErrorContains(t, p.CheckNoApp(), "99")
}

func TestNoAppMeansRunCanGoAhead(t *testing.T) {
	home, err := os.MkdirTemp("/tmp", "pf")
	require.NoError(t, err)
	t.Cleanup(func() { os.RemoveAll(home) })
	t.Setenv("HOME", home)

	assert.NoError(t, Project{Dir: "/x/demo"}.CheckNoApp())
}

func TestRootComesFromTheEnvironment(t *testing.T) {
	root := t.TempDir()
	writeFile(t, filepath.Join(root, "runtime", "idf", "tools", "cmake", "project.cmake"), "")
	t.Setenv("PREFLASH_ROOT", root)

	got, err := Root()
	require.NoError(t, err)
	assert.Equal(t, root, got)
}

func TestRootWithoutARuntimeSaysSo(t *testing.T) {
	t.Setenv("PREFLASH_ROOT", t.TempDir())

	_, err := Root()
	require.Error(t, err)
	assert.Contains(t, err.Error(), "PREFLASH_ROOT")
}

func TestRootDefaultsToThisCheckout(t *testing.T) {
	t.Setenv("PREFLASH_ROOT", "")

	got, err := Root()
	require.NoError(t, err)
	assert.FileExists(t, filepath.Join(got, "boards.go"))
}

func TestStateDirIsPrivate(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	require.NoError(t, p.makeDirs())
	info, err := os.Stat(p.StateDir())
	require.NoError(t, err)
	assert.Equal(t, os.FileMode(0o700), info.Mode().Perm())
}

func TestBoardStopsOnAnUnknownKeyInTheConfig(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "CMakeLists.txt"), "include($ENV{IDF_PATH}/tools/cmake/project.cmake)\n")
	writeFile(t, filepath.Join(dir, ".preflashrc"), "board: a\nboarf: b\n")
	p, err := Find(dir)
	require.NoError(t, err)

	_, err = p.Board("")

	require.Error(t, err)
	assert.Contains(t, err.Error(), ".preflashrc:2")
}

func TestBoardsListProjectFilesAndStock(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "sim", "mod.yaml"), "name: mod\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  backlight: {enable: gpio43}\n")
	writeFile(t, filepath.Join(dir, "sim", "broken.yml"), "name: broken\nextends: nothing-like-this\n")
	writeFile(t, filepath.Join(dir, "sim", "notes.yaml"), "title: not a board\n")
	writeFile(t, filepath.Join(dir, "elsewhere.yaml"), "name: stray\nextends: ESP32-S3-Touch-LCD-4B\n")
	p, err := Find(dir)
	require.NoError(t, err)

	choices := p.Boards()

	var names []string
	for _, c := range choices {
		names = append(names, c.Board.Name+"/"+c.File)
	}
	assert.Equal(t, []string{"stray/elsewhere.yaml", "/sim/broken.yml", "mod/sim/mod.yaml", "ESP32-S3-Touch-LCD-4B/", "ESP32-S3-Touch-LCD-3.49-V2/"}, names)
	assert.Error(t, choices[1].Err, "a board file that won't load stays on the list")
	assert.True(t, choices[0].FromProject)
	assert.False(t, choices[3].FromProject)
}

func TestBoardsPutTheConfiguredFileFirst(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "hw", "custom.yaml"), "name: custom\nextends: ESP32-S3-Touch-LCD-4B\n")
	writeFile(t, filepath.Join(dir, "sim", "mod.yaml"), "name: mod\nextends: ESP32-S3-Touch-LCD-4B\n")
	writeFile(t, filepath.Join(dir, ".preflashrc"), "board: hw/custom.yaml\n")
	p, err := Find(dir)
	require.NoError(t, err)

	choices := p.Boards()

	require.Len(t, choices, 4)
	assert.Equal(t, "custom", choices[0].Board.Name)
	assert.Equal(t, "mod", choices[1].Board.Name)
}

func TestBoardsLeaveNonBoardYamlOut(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "broken.yaml"), "name: [\n")
	p, err := Find(dir)
	require.NoError(t, err)
	assert.Len(t, p.Boards(), len(board.Stock()))
}

func TestLVGLVersionComesFromTheManagedComponent(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	p, err := Find(dir)
	require.NoError(t, err)
	assert.Empty(t, p.LVGLVersion())

	writeFile(t, filepath.Join(dir, "managed_components", "lvgl__lvgl", "lv_version.h"),
		"#define LVGL_VERSION_MAJOR 9\n#define LVGL_VERSION_MINOR 3\n#define LVGL_VERSION_PATCH 0\n")
	assert.Equal(t, "9.3.0", p.LVGLVersion())
}

func TestInstallCopiesTheAppSoABuildCantTouchWhatRuns(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	dir := idfProject(t, t.TempDir())
	p, err := Find(dir)
	require.NoError(t, err)
	require.NoError(t, os.MkdirAll(p.BuildDir(), 0o755))
	writeFile(t, filepath.Join(p.BuildDir(), "app"), "first")
	b, err := board.LoadStock("ESP32-S3-Touch-LCD-4B")
	require.NoError(t, err)
	env := []string{"PREFLASH_BOARD=old", "PATH=/bin"}

	path, runEnv, err := p.Install(b, env)

	require.NoError(t, err)
	writeFile(t, filepath.Join(p.BuildDir(), "app"), "second")
	got, err := os.ReadFile(path)
	require.NoError(t, err)
	assert.Equal(t, "first", string(got))
	info, err := os.Stat(path)
	require.NoError(t, err)
	assert.NotZero(t, info.Mode()&0o100, "the copy must run")

	assert.Contains(t, runEnv, "PATH=/bin")
	assert.NotContains(t, runEnv, "PREFLASH_BOARD=old")
	var boardFile string
	for _, entry := range runEnv {
		if value, ok := strings.CutPrefix(entry, "PREFLASH_BOARD="); ok {
			boardFile = value
		}
	}
	data, err := os.ReadFile(boardFile)
	require.NoError(t, err)
	assert.Contains(t, string(data), b.Name)
}

func TestBuildWritesToTheGivenOutputAndStopsOnFailure(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	dir := idfProject(t, t.TempDir())
	p, err := Find(dir)
	require.NoError(t, err)
	require.NoError(t, os.MkdirAll(p.BuildDir(), 0o755))
	var output strings.Builder

	// cmake refuses this project, because IDF_PATH leads nowhere.
	err = p.Build(context.Background(), []string{"PATH=" + os.Getenv("PATH"), "IDF_PATH=/nowhere"}, &output)

	require.Error(t, err)
	assert.Contains(t, err.Error(), "the build failed")
	assert.Contains(t, output.String(), "/nowhere")
}

func TestLogPathIsInTheBuildFolder(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	assert.Equal(t, p.BuildDir(), filepath.Dir(p.LogPath()))
}

func TestStopSignalsLeaveAnInheritedIgnoreAlone(t *testing.T) {
	assert.Contains(t, stopSignals(), os.Signal(syscall.SIGHUP))

	signal.Ignore(syscall.SIGHUP)
	defer signal.Reset(syscall.SIGHUP)

	assert.NotContains(t, stopSignals(), os.Signal(syscall.SIGHUP))
	assert.Contains(t, stopSignals(), os.Interrupt)
}

func TestABuildStoppedOnPurposeStaysQuiet(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	require.NoError(t, os.MkdirAll(p.BuildDir(), 0o755))
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var printed strings.Builder

	err = p.buildQuietly(ctx, []string{"PATH=" + os.Getenv("PATH")}, &printed)

	assert.EqualError(t, err, "the build was stopped")
	assert.Empty(t, printed.String())
}

func TestAFailedBuildPrintsItsOutput(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	require.NoError(t, os.MkdirAll(p.BuildDir(), 0o755))
	var printed strings.Builder

	err = p.buildQuietly(context.Background(), []string{"PATH=" + os.Getenv("PATH"), "IDF_PATH=/nowhere"}, &printed)

	assert.ErrorContains(t, err, "the build failed")
	assert.Contains(t, printed.String(), "/nowhere")
}

func TestABoardFileWithASyntaxErrorStaysOnTheList(t *testing.T) {
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "sim", "mod.yaml"), "name: mod\nextends: ESP32-S3-Touch-LCD-4B\nchanges: [\n")
	p, err := Find(dir)
	require.NoError(t, err)

	choices := p.Boards()

	require.Len(t, choices, len(board.Stock())+1)
	assert.Equal(t, filepath.Join("sim", "mod.yaml"), choices[0].File)
	assert.Error(t, choices[0].Err)
}

func TestAFailedInstallLeavesTheRunningBoardFileAlone(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	require.NoError(t, os.MkdirAll(p.BuildDir(), 0o755))
	boardFile := filepath.Join(p.BuildDir(), "board.running.json")
	writeFile(t, boardFile, "the old board")
	b, err := board.LoadStock("ESP32-S3-Touch-LCD-4B")
	require.NoError(t, err)

	_, _, err = p.Install(b, nil)

	require.ErrorContains(t, err, "the build made no app")
	got, err := os.ReadFile(boardFile)
	require.NoError(t, err)
	assert.Equal(t, "the old board", string(got))
}

func TestBoardsAreFoundUnderAPathWithGlobCharacters(t *testing.T) {
	dir := idfProject(t, filepath.Join(t.TempDir(), "app [old]"))
	writeFile(t, filepath.Join(dir, "sim", "mod.yaml"), "name: mod\nextends: ESP32-S3-Touch-LCD-4B\n")
	p, err := Find(dir)
	require.NoError(t, err)
	assert.Equal(t, "mod", p.Boards()[0].Board.Name)
}

func TestStopSignalsLeaveAnIgnoredInterruptAlone(t *testing.T) {
	signal.Ignore(os.Interrupt)
	defer signal.Reset(os.Interrupt)
	assert.NotContains(t, stopSignals(), os.Interrupt)
}

func TestASanitizerBuildsInItsOwnFolder(t *testing.T) {
	t.Setenv("HOME", "/home/me")
	p := Project{Dir: "/x/demo", id: "demo-12345678"}
	assert.Equal(t, "/home/me/.preflash/build/demo-12345678-address", p.Sanitized("address").BuildDir())
	assert.Equal(t, "/home/me/.preflash/build/demo-12345678-thread", p.Sanitized("thread").BuildDir())
	assert.Equal(t, p.BuildDir(), p.Sanitized("").BuildDir())
	assert.Equal(t, p.StateDir(), p.Sanitized("address").StateDir(), "saved state stays the checkout's")
	assert.Equal(t, p.SocketPath(), p.Sanitized("thread").SocketPath(), "one app per checkout still")
}

func TestOnlyKnownSanitizersPass(t *testing.T) {
	assert.NoError(t, CheckSanitizer(""))
	assert.NoError(t, CheckSanitizer("address"))
	assert.NoError(t, CheckSanitizer("thread"))
	assert.ErrorContains(t, CheckSanitizer("memory"), "address or thread")
}

func optionOf(env []string, name string) string {
	for _, entry := range env {
		if value, ok := strings.CutPrefix(entry, name+"="); ok {
			return value
		}
	}
	return ""
}

func TestEnvironmentAsksForCrashReports(t *testing.T) {
	t.Setenv("HOME", "/home/me")
	p := Project{Dir: "/x/demo", id: "demo-1"}
	env := p.environment([]string{"PREFLASH_CRASH_FILE=/old", "PREFLASH_CRASH_HOLD=1", "PREFLASH_SANITIZE=thread", "PREFLASH_SANITIZER_LOG=/old"}, "/repo", "", false)
	assert.Equal(t, "/home/me/.preflash/build/demo-1/crash.json", optionOf(env, "PREFLASH_CRASH_FILE"))
	assert.NotContains(t, env, "PREFLASH_CRASH_FILE=/old")
	assert.NotContains(t, env, "PREFLASH_CRASH_HOLD=1")
	assert.NotContains(t, env, "PREFLASH_SANITIZE=thread")
	assert.NotContains(t, env, "PREFLASH_SANITIZER_LOG=/old")
}

func TestEnvironmentSetsUpTheSanitizerAndKeepsTheUsersOptions(t *testing.T) {
	t.Setenv("HOME", "/home/me")
	p := Project{Dir: "/x/demo", id: "demo-1"}.Sanitized("address")
	env := p.environment([]string{"ASAN_OPTIONS=verbosity=1"}, "/repo", "", false)
	assert.Contains(t, env, "PREFLASH_SANITIZE=address")
	log := "/home/me/.preflash/build/demo-1-address/sanitizer.log"
	assert.Contains(t, env, "PREFLASH_SANITIZER_LOG="+log)
	options := optionOf(env, "ASAN_OPTIONS")
	assert.True(t, strings.HasPrefix(options, "verbosity=1:"), options)
	for _, want := range []string{"abort_on_error=1", "halt_on_error=1", "use_sigaltstack=0", `log_path="` + log + `"`} {
		assert.Contains(t, options, want)
	}

	thread := Project{Dir: "/x/demo", id: "demo-1"}.Sanitized("thread")
	options = optionOf(thread.environment(nil, "/repo", "", false), "TSAN_OPTIONS")
	assert.Contains(t, options, "abort_on_error=1")
	assert.Contains(t, options, "halt_on_error=1")
	assert.Contains(t, options, `suppressions="/repo/runtime/tsan.supp"`)
}

func TestLastCrashIsTheNewestOfAnyBuild(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p := Project{Dir: "/x/demo", id: "demo-1"}
	_, err := p.LastCrash()
	assert.ErrorIs(t, err, fs.ErrNotExist)

	write := func(q Project, pid int, age time.Duration) {
		require.NoError(t, os.MkdirAll(q.BuildDir(), 0o755))
		path := q.CrashPath()
		require.NoError(t, os.WriteFile(path, []byte(fmt.Sprintf(`{"pid":%d}`, pid)), 0o644))
		when := time.Now().Add(-age)
		require.NoError(t, os.Chtimes(path, when, when))
	}
	write(p, 1, time.Hour)
	write(p.Sanitized("address"), 2, time.Minute)
	write(p.Sanitized("thread"), 3, time.Hour)

	report, err := p.LastCrash()
	require.NoError(t, err)
	assert.Equal(t, 2, report.PID)
}

func TestABoardCanHaveAnSdkconfigOfItsOwn(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "sdkconfig"), "CONFIG_A=y\n")
	writeFile(t, filepath.Join(dir, "sdkconfig.small"), "CONFIG_B=y\n")
	writeFile(t, filepath.Join(dir, ".preflashrc"), "sdkconfig:\n  small: sdkconfig.small\n")
	p, err := Find(dir)
	require.NoError(t, err)

	env, err := p.Environment(board.Board{Name: "small"}, true)
	require.NoError(t, err)
	assert.Contains(t, env, "PREFLASH_SDKCONFIG="+filepath.Join(p.Dir, "sdkconfig.small"))

	env, err = p.Environment(board.Board{Name: "other"}, true)
	require.NoError(t, err)
	for _, entry := range env {
		assert.NotContains(t, entry, "PREFLASH_SDKCONFIG=")
	}
}

func TestAnSdkconfigThatIsntThereStopsTheRun(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, ".preflashrc"), "sdkconfig:\n  small: sdkconfig.small\n")
	p, err := Find(dir)
	require.NoError(t, err)

	_, err = p.Environment(board.Board{Name: "small"}, true)

	require.ErrorContains(t, err, "sdkconfig.small")
}
