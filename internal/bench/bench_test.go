package bench

import (
	"bytes"
	"encoding/json"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

var sample = Result{
	CleanBuild:       90 * time.Second,
	Rebuild:          2500 * time.Millisecond,
	BootToFirstFrame: 120 * time.Millisecond,
	FramesPerSecond:  61.5,
	SocketRoundTrip:  150 * time.Microsecond,
	SerialMBPerSec:   42.25,
}

func TestJSONNamesItsUnits(t *testing.T) {
	var out bytes.Buffer
	require.NoError(t, sample.WriteJSON(&out))

	var got map[string]float64
	require.NoError(t, json.Unmarshal(out.Bytes(), &got))
	assert.Equal(t, map[string]float64{
		"clean_build_s":          90,
		"rebuild_s":              2.5,
		"boot_to_first_frame_ms": 120,
		"frames_per_second":      61.5,
		"socket_round_trip_us":   150,
		"serial_mb_per_second":   42.25,
	}, got)
}

func TestTableHasARowForEachMeasure(t *testing.T) {
	var out bytes.Buffer
	sample.WriteTable(&out)

	for _, row := range []string{
		"Clean build", "90.0 s", "Rebuild", "2.5 s", "Boot to first frame", "120 ms",
		"Frame rate", "61.5 fps", "Socket round trip", "150 µs", "Serial throughput", "42.2 MB/s",
	} {
		assert.Contains(t, out.String(), row)
	}
}

func TestDoneMarkerIsFoundAcrossChunks(t *testing.T) {
	var w serialCounter
	w.add([]byte("xxxxspew do"))
	assert.False(t, w.done)
	w.add([]byte("ne\n"))
	assert.True(t, w.done)
	assert.Equal(t, 14, w.bytes)
}
