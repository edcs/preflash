package web

import (
	"strings"

	"github.com/charmbracelet/x/ansi"
	"github.com/charmbracelet/x/vt"
)

// carriedModes are the modes a snapshot passes on: how keys, the mouse, focus and pastes
// reach the app. Screen, cursor and origin modes are drawn rather than passed on.
var carriedModes = []ansi.DECMode{
	ansi.ModeCursorKeys,
	ansi.ModeAutoWrap,
	ansi.ModeMouseX10,
	ansi.ModeMouseNormal,
	ansi.ModeMouseButtonEvent,
	ansi.ModeMouseAnyEvent,
	ansi.ModeFocusEvent,
	ansi.ModeMouseExtUtf8,
	ansi.ModeMouseExtSgr,
	ansi.DECMode(1015), // urxvt mouse encoding
	ansi.ModeMouseExtSgrPixel,
	ansi.ModeBracketedPaste,
}

// drawScreen is the escape bytes that bring a blank xterm to the screen's state: the
// alternate screen if it's on, every cell with its style, the modes the app set, and the
// cursor's style, visibility and position. modes holds what the app set or reset; a mode it
// never touched is left at the terminal's default.
func drawScreen(emu *vt.SafeEmulator, modes map[ansi.Mode]bool) []byte {
	var b strings.Builder
	if emu.IsAltScreen() {
		b.WriteString(ansi.SetModeAltScreenSaveCursor)
	}
	b.WriteString(ansi.ResetStyle + ansi.CursorHomePosition + ansi.EraseEntireScreen)
	// Each row ends with its style reset, so rows can be placed one by one.
	for y, line := range strings.Split(emu.Render(), "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		b.WriteString(ansi.CursorPosition(1, y+1))
		b.WriteString(line)
	}
	for _, mode := range carriedModes {
		set, touched := modes[mode]
		switch {
		case !touched:
		case set:
			b.WriteString(ansi.SetMode(mode))
		default:
			b.WriteString(ansi.ResetMode(mode))
		}
	}
	if style, blink := emu.CursorStyle(); style != vt.CursorBlock || !blink {
		// DECSCUSR numbers blinking block, steady block, blinking underline and so on from 1.
		n := int(style)*2 + 1
		if !blink {
			n++
		}
		b.WriteString(ansi.SetCursorStyle(n))
	}
	if emu.CursorHidden() {
		b.WriteString(ansi.ResetModeTextCursorEnable)
	}
	at := emu.CursorPosition()
	b.WriteString(ansi.CursorPosition(at.X+1, at.Y+1))
	return []byte(b.String())
}
