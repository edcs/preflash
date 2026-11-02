package board

import (
	"encoding/json"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const stock = "../../boards/esp32-s3-touch-lcd-4b.yaml"

func TestExtendsAppliesChanges(t *testing.T) {
	b, err := Load("testdata/snoozel-like.yaml")
	require.NoError(t, err)
	assert.Equal(t, 480, b.Display.Width)
	require.NotNil(t, b.Backlight.Enable)
	assert.Equal(t, 43, *b.Backlight.Enable)
	assert.Equal(t, 4, b.Backlight.Pin) // the parent's value survives
	assert.Equal(t, "Modded 4B", b.Name)
}

func TestStockBoard(t *testing.T) {
	b, err := Load(stock)
	require.NoError(t, err)
	assert.Equal(t, "ESP32-S3-Touch-LCD-4B", b.Name)
	assert.True(t, b.Backlight.ActiveLow)
	assert.Nil(t, b.Backlight.Enable)
	assert.Equal(t, "es8311", b.AudioCodec)
	assert.Contains(t, b.I2C, Chip{Chip: "pcf85063", Address: 0x51})
	assert.Contains(t, b.I2C, Chip{Chip: "qmi8658", Address: 0x6b})
	assert.Contains(t, b.I2C, Chip{Chip: "axp2101", Address: 0x34})
}

func TestResolveShape(t *testing.T) {
	b, err := Load("testdata/snoozel-like.yaml")
	require.NoError(t, err)
	out, err := Resolve(b)
	require.NoError(t, err)
	var m map[string]any
	require.NoError(t, json.Unmarshal(out, &m))
	assert.Equal(t, float64(480), m["display"].(map[string]any)["width"])
	assert.Equal(t, float64(43), m["backlight"].(map[string]any)["enable_pin"])
	assert.Equal(t, "es8311", m["audio_codec"])
}

func TestStockBoardsAreListedAndLoadByName(t *testing.T) {
	assert.Contains(t, Stock(), "ESP32-S3-Touch-LCD-4B")
	b, err := LoadStock("ESP32-S3-Touch-LCD-4B")
	require.NoError(t, err)
	assert.Equal(t, 480, b.Display.Width)
}

func TestLoadStockSaysWhenThereIsNone(t *testing.T) {
	_, err := LoadStock("no-such-board")
	assert.ErrorIs(t, err, fs.ErrNotExist)
}

func TestUnknownKeyNamesIt(t *testing.T) {
	_, err := Load("testdata/typo.yaml")
	require.Error(t, err)
	assert.Contains(t, err.Error(), "backlite")
}

func TestMissingParentNamesIt(t *testing.T) {
	_, err := Load("testdata/missing-parent.yaml")
	require.Error(t, err)
	assert.Contains(t, err.Error(), "no-such-board")
}

func TestExtendsLoopIsAnError(t *testing.T) {
	_, err := Load("testdata/loop-a.yaml")
	require.Error(t, err)
	assert.Contains(t, err.Error(), "loop")
}

func TestHugePinIsRejected(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "huge.yaml")
	require.NoError(t, os.WriteFile(path, []byte("name: Huge\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  backlight:\n    enable: gpio99999999999999999999\n"), 0o644))
	_, err := Load(path)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "out of range")
}

func TestMissingBacklightKeyNamesIt(t *testing.T) {
	for _, key := range []string{"pin", "active_low"} {
		t.Run(key, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "board.yaml")
			other := map[string]string{"pin": "active_low: true", "active_low": "pin: gpio4"}[key]
			src := "name: X\ndisplay:\n  width: 480\n  height: 320\ni2c: []\nbacklight:\n  " + other + "\naudio_codec: es8311\n"
			require.NoError(t, os.WriteFile(path, []byte(src), 0o644))
			_, err := Load(path)
			require.Error(t, err)
			assert.Contains(t, err.Error(), "backlight."+key)
		})
	}
}

func TestErrorsNameTheAbsolutePath(t *testing.T) {
	dir := t.TempDir()
	t.Chdir(dir)
	require.NoError(t, os.WriteFile("typo.yaml", []byte("name: Typo\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  backlight:\n    pin: nope\n"), 0o644))
	require.NoError(t, os.WriteFile("size.yaml", []byte("name: Size\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  display:\n    width: 0\n"), 0o644))

	for _, name := range []string{"typo.yaml", "size.yaml"} {
		_, err := Load(name)
		require.Error(t, err)
		resolved, evalErr := filepath.EvalSymlinks(dir)
		require.NoError(t, evalErr)
		abs := filepath.Join(dir, name)
		assert.True(t, strings.Contains(err.Error(), abs) || strings.Contains(err.Error(), filepath.Join(resolved, name)), err.Error())
	}
}

func TestABoardKnowsWhatItWasBasedOn(t *testing.T) {
	b, err := Load("../../tests/fixtures/snoozel-4b.yaml")
	require.NoError(t, err)
	assert.Equal(t, "ESP32-S3-Touch-LCD-4B", b.Base)
	assert.Equal(t, []Change{{Path: "backlight.enable", Value: "gpio43"}}, b.Changes)
}

func TestAChainKeepsItsRootAndEveryChange(t *testing.T) {
	dir := t.TempDir()
	require.NoError(t, os.WriteFile(filepath.Join(dir, "one.yaml"), []byte(
		"extends: ESP32-S3-Touch-LCD-4B\nname: one\nchanges:\n  backlight: {enable: gpio1}\n"), 0o644))
	require.NoError(t, os.WriteFile(filepath.Join(dir, "two.yaml"), []byte(
		"extends: one.yaml\nname: two\nchanges:\n  audio_codec: es7210\n  backlight: {enable: gpio2}\n"), 0o644))

	b, err := Load(filepath.Join(dir, "two.yaml"))

	require.NoError(t, err)
	assert.Equal(t, "ESP32-S3-Touch-LCD-4B", b.Base)
	assert.Equal(t, []Change{{Path: "audio_codec", Value: "es7210"}, {Path: "backlight.enable", Value: "gpio2"}}, b.Changes)
}

func TestAStockBoardHasNoBase(t *testing.T) {
	b, err := LoadStock("ESP32-S3-Touch-LCD-4B")
	require.NoError(t, err)
	assert.Empty(t, b.Base)
	assert.Empty(t, b.Changes)
}

func TestTheRuntimeDoesNotSeeTheLineage(t *testing.T) {
	b, err := Load("../../tests/fixtures/snoozel-4b.yaml")
	require.NoError(t, err)
	out, err := Resolve(b)
	require.NoError(t, err)
	assert.NotContains(t, string(out), "gpio43")
	assert.NotContains(t, string(out), "Base")
}

func TestAChangedListReadsAsReplaced(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "b.yaml")
	require.NoError(t, os.WriteFile(path, []byte(
		"extends: ESP32-S3-Touch-LCD-4B\nchanges:\n  i2c:\n    - {chip: gt911, address: 0x5d}\n"), 0o644))

	b, err := Load(path)

	require.NoError(t, err)
	assert.Equal(t, []Change{{Path: "i2c", Value: "replaced"}}, b.Changes)
}

func TestReloadReadsTheFileAgain(t *testing.T) {
	path := filepath.Join(t.TempDir(), "b.yaml")
	require.NoError(t, os.WriteFile(path, []byte("extends: ESP32-S3-Touch-LCD-4B\nname: first\n"), 0o644))
	b, err := Load(path)
	require.NoError(t, err)

	require.NoError(t, os.WriteFile(path, []byte("extends: ESP32-S3-Touch-LCD-4B\nname: second\n"), 0o644))
	b, err = Reload(b)

	require.NoError(t, err)
	assert.Equal(t, "second", b.Name)
}

func TestReloadLeavesAStockBoardAlone(t *testing.T) {
	b, err := LoadStock("ESP32-S3-Touch-LCD-4B")
	require.NoError(t, err)
	again, err := Reload(b)
	require.NoError(t, err)
	assert.Equal(t, b, again)
}

func writeBoard(t *testing.T, src string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "board.yaml")
	require.NoError(t, os.WriteFile(path, []byte(src), 0o644))
	return path
}

func TestThe349BoardFollowsItsV2Wiring(t *testing.T) {
	b, err := LoadStock("ESP32-S3-Touch-LCD-3.49-V2")
	require.NoError(t, err)
	assert.Equal(t, "axs15231b", b.Display.Controller)
	assert.Equal(t, 172, b.Display.Width)
	assert.Equal(t, 640, b.Display.Height)
	assert.Equal(t, "be", b.Display.ByteOrder)
	assert.ElementsMatch(t, []Chip{
		{Chip: "tca9554", Address: 0x20},
		{Chip: "qmi8658", Address: 0x6b},
		{Chip: "pcf85063", Address: 0x51},
		{Chip: "axs15231b", Address: 0x3b, Bus: 1},
	}, b.I2C)
	assert.Equal(t, 42, b.Backlight.Pin)
	assert.True(t, b.Backlight.ActiveLow)
	assert.Nil(t, b.Backlight.Enable)
	require.NotNil(t, b.Backlight.EnableExpander)
	assert.Equal(t, 1, *b.Backlight.EnableExpander)
	assert.Nil(t, b.PowerAmp)
}

func TestByteOrderDefaultsToLittleEndian(t *testing.T) {
	b, err := Load(stock)
	require.NoError(t, err)
	assert.Equal(t, "le", b.Display.ByteOrder)
}

func TestABadByteOrderNamesIt(t *testing.T) {
	path := writeBoard(t, "name: X\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  display:\n    byte_order: middle\n")
	_, err := Load(path)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "display.byte_order")
}

func TestTheBusMustBeOneTheChipHas(t *testing.T) {
	path := writeBoard(t, "name: X\ndisplay:\n  width: 480\n  height: 320\ni2c:\n  - {chip: gt911, address: 0x5d, bus: 2}\nbacklight:\n  pin: gpio4\n  active_low: true\naudio_codec: none\n")
	_, err := Load(path)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "i2c[0].bus")
}

func TestTheAmpSwitchComesFromTheBoard(t *testing.T) {
	b, err := Load(stock)
	require.NoError(t, err)
	require.NotNil(t, b.PowerAmp)
	assert.Equal(t, 3, *b.PowerAmp)
}

func TestExpanderBitsReachTheRuntime(t *testing.T) {
	b, err := LoadStock("ESP32-S3-Touch-LCD-3.49-V2")
	require.NoError(t, err)
	out, err := Resolve(b)
	require.NoError(t, err)
	var m map[string]any
	require.NoError(t, json.Unmarshal(out, &m))
	assert.Equal(t, float64(1), m["backlight"].(map[string]any)["enable_expander_bit"])
	assert.Equal(t, "be", m["display"].(map[string]any)["byte_order"])
	assert.NotContains(t, m, "power_amp_bit")

	chips := m["i2c"].([]any)
	buses := map[float64]float64{}
	for _, c := range chips {
		chip := c.(map[string]any)
		buses[chip["address"].(float64)] = chip["bus"].(float64)
	}
	assert.Equal(t, float64(1), buses[0x3b])
	assert.Equal(t, float64(0), buses[0x20])

	b, err = Load(stock)
	require.NoError(t, err)
	out, err = Resolve(b)
	require.NoError(t, err)
	require.NoError(t, json.Unmarshal(out, &m))
	assert.Equal(t, float64(3), m["power_amp_bit"])
}

func TestAPinCanReplaceAnExpanderEnable(t *testing.T) {
	path := writeBoard(t, "name: X\nextends: ESP32-S3-Touch-LCD-3.49-V2\nchanges:\n  backlight:\n    enable: gpio43\n")
	b, err := Load(path)
	require.NoError(t, err)
	require.NotNil(t, b.Backlight.Enable)
	assert.Equal(t, 43, *b.Backlight.Enable)
	assert.Nil(t, b.Backlight.EnableExpander)
}

func TestAnExpanderEnableNeedsAnExpander(t *testing.T) {
	path := writeBoard(t, "name: X\ndisplay:\n  width: 480\n  height: 320\ni2c: []\nbacklight:\n  pin: gpio4\n  active_low: true\n  enable: exio1\naudio_codec: none\n")
	_, err := Load(path)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "tca9554")
}

func TestExpanderBitsAreChecked(t *testing.T) {
	for _, src := range []string{
		"name: X\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  backlight:\n    enable: exio8\n",
		"name: X\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  power_amp: gpio3\n",
		"name: X\nextends: ESP32-S3-Touch-LCD-4B\nchanges:\n  backlight:\n    enable_expander: 1\n",
	} {
		_, err := Load(writeBoard(t, src))
		assert.Error(t, err, src)
	}
}
