package tui

import (
	"strings"

	"charm.land/lipgloss/v2"
	"github.com/charmbracelet/x/ansi"
)

// The palette, from the approved mockups.
var (
	colourGround  = lipgloss.Color("#0E1113")
	colourSurface = lipgloss.Color("#15191C")
	colourLine    = lipgloss.Color("#2A3036")
	colourText    = lipgloss.Color("#E7ECEF")
	colourAccent  = lipgloss.Color("#5FD4E8")
	colourMuted   = lipgloss.Color("#8D98A1")
	colourAmber   = lipgloss.Color("#F2B84B")
	colourBad     = lipgloss.Color("#FF9A70")
	colourBadEdge = lipgloss.Color("#5B4034")
)

var (
	styleText     = lipgloss.NewStyle().Foreground(colourText)
	styleStrong   = lipgloss.NewStyle().Foreground(colourText).Bold(true)
	styleDim      = lipgloss.NewStyle().Foreground(colourMuted)
	styleAccent   = lipgloss.NewStyle().Foreground(colourAccent)
	styleKey      = lipgloss.NewStyle().Foreground(colourAccent).Bold(true)
	styleAmber    = lipgloss.NewStyle().Foreground(colourAmber)
	styleBad      = lipgloss.NewStyle().Foreground(colourBad)
	styleBadBold  = lipgloss.NewStyle().Foreground(colourBad).Bold(true)
	styleRule     = lipgloss.NewStyle().Foreground(colourLine)
	styleBadEdge  = lipgloss.NewStyle().Foreground(colourBadEdge)
	styleSelected = lipgloss.NewStyle().Foreground(colourGround).Background(colourAccent)
)

// segStyle is short for the style type the views pass around.
type segStyle = lipgloss.Style

// surfaceStyle paints a bar or a chip.
var surfaceStyle = lipgloss.NewStyle().Background(colourSurface)

const (
	// sidePad is the empty cells at each side of the window.
	sidePad = 2
	// boxPad is the empty cells inside a box's sides.
	boxPad = 1
)

// seg is a run of text in one style.
type seg struct {
	style lipgloss.Style
	text  string
}

func segsWidth(segs []seg) int {
	width := 0
	for _, s := range segs {
		width += ansi.StringWidth(s.text)
	}
	return width
}

// renderOn draws segments on a background. Each needs its own, because a style's end
// resets the whole line's.
func renderOn(segs []seg, background *lipgloss.Style) string {
	var out strings.Builder
	for _, s := range segs {
		style := s.style
		if background != nil {
			style = style.Background(background.GetBackground())
		}
		out.WriteString(style.Render(s.text))
	}
	return out.String()
}

// bar draws a line of the toolbar or footer, exactly width cells on the surface colour:
// left at the start and right at the end, cutting left short when both don't fit.
func bar(left, right []seg, width int) string {
	surface := surfaceStyle
	inner := width - 2*sidePad
	room := max(inner-segsWidth(right)-1, 0)
	drawn := ansi.Truncate(renderOn(left, &surface), room, "…")
	gap := max(inner-ansi.StringWidth(drawn)-segsWidth(right), 0)
	pad := strings.Repeat(" ", sidePad)
	return surface.Render(pad) + drawn + surface.Render(strings.Repeat(" ", gap)) +
		renderOn(right, &surface) + surface.Render(pad)
}

// fit cuts s to width cells and pads it to exactly that width.
func fit(s string, width int) string {
	if width <= 0 {
		return ""
	}
	s = ansi.Truncate(s, width, "…")
	if pad := width - ansi.StringWidth(s); pad > 0 {
		s += strings.Repeat(" ", pad)
	}
	return s
}

// indent puts s at the left margin of a line no wider than width.
func indent(s string, width int) string {
	return strings.Repeat(" ", sidePad) + ansi.Truncate(s, max(width-2*sidePad, 0), "…")
}
