package board

import (
	"encoding/json"
	"os"
	"path/filepath"
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
