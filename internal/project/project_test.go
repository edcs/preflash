package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
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
	base := []string{"PATH=/bin", "PREFLASH_HEADLESS=1", "IDF_PATH=/real/esp-idf"}
	p := Project{Dir: "/x/demo", id: "demo-12345678"}
	t.Setenv("HOME", "/home/me")

	windowed := p.environment(base, "/repo", false)
	assert.Contains(t, windowed, "PATH=/bin")
	assert.Contains(t, windowed, "IDF_PATH=/repo/runtime/idf")
	assert.Contains(t, windowed, "PREFLASH_BOARD=/home/me/.preflash/build/demo-12345678/board.json")
	assert.Contains(t, windowed, "PREFLASH_STATE=/home/me/.preflash/state/demo-12345678")
	assert.Contains(t, windowed, "PREFLASH_PROJECT=demo")
	assert.NotContains(t, windowed, "PREFLASH_HEADLESS=1")
	assert.NotContains(t, windowed, "IDF_PATH=/real/esp-idf")

	assert.Contains(t, p.environment(base, "/repo", true), "PREFLASH_HEADLESS=1")
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
