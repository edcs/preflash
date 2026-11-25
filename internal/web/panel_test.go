package web

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestToPanel(t *testing.T) {
	for _, c := range []struct {
		u, v     float64
		rotation int
		x, y     int
	}{
		{0, 0, 0, 0, 0},
		{1, 1, 0, 171, 639},
		{0, 0, 90, 0, 639},
		{1, 1, 90, 171, 0},
		{0, 0, 180, 171, 639},
		{0, 0, 270, 171, 0},
		{1, 1, 270, 0, 639},
		{-0.5, 2, 0, 0, 639},
	} {
		x, y := toPanel(c.u, c.v, c.rotation, 172, 640)
		assert.Equal(t, []int{c.x, c.y}, []int{x, y}, "u=%v v=%v rotation=%d", c.u, c.v, c.rotation)
	}
}
