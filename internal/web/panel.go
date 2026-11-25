package web

import "math"

// toPanel maps a point u,v (0..1 across the panel as shown, after rotation) to panel
// coordinates, as window::to_panel in runtime/window/layout.cpp does. The shown size is
// panelH by panelW at 90 and 270. A point outside the panel lands on its edge, so a drag that
// leaves it lets go there.
func toPanel(u, v float64, boardRotation, panelW, panelH int) (x, y int) {
	rotation := ((boardRotation % 360) + 360) % 360
	shownW, shownH := panelW, panelH
	if rotation == 90 || rotation == 270 {
		shownW, shownH = panelH, panelW
	}
	sx := pixel(u, shownW)
	sy := pixel(v, shownH)
	switch rotation {
	case 90:
		return sy, panelH - 1 - sx
	case 180:
		return panelW - 1 - sx, panelH - 1 - sy
	case 270:
		return panelW - 1 - sy, sx
	default:
		return sx, sy
	}
}

// pixel puts f, 0..1 across n pixels, on the nearest one.
func pixel(f float64, n int) int {
	if math.IsNaN(f) {
		f = 0
	}
	p := int(math.Round(f * float64(n-1)))
	return min(max(p, 0), n-1)
}
