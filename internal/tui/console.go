package tui

import (
	"bytes"
	"strings"
	"unicode"

	"github.com/charmbracelet/x/ansi"
)

const (
	// scrollbackLines is how much output the console keeps.
	scrollbackLines = 20000
	// partialLimit is the most of an unfinished line kept before it's cut, so firmware that
	// never prints a newline can't fill memory.
	partialLimit = 64 * 1024
	tabWidth     = 4
)

// lineKind says what a console line is, which sets its colour.
type lineKind int

const (
	lineText lineKind = iota
	lineDim
	lineWarn
	lineBad
	lineEcho
)

// line is one line of the console, safe to draw.
type line struct {
	text string
	kind lineKind
}

// row is one screen row of a line.
type row struct {
	text string
	kind lineKind
}

// console is the serial stream as lines, with whatever preflash adds among them.
type console struct {
	lines   []line
	limit   int
	partial []byte // the line still arriving, raw
}

func newConsole() *console { return &console{limit: scrollbackLines} }

// feed adds bytes from the stream, and gives back the lines they finished. Chunks can end
// anywhere, even inside an escape sequence, so a line is cleaned only when it's whole.
func (c *console) feed(data []byte) []string {
	c.partial = append(c.partial, data...)
	var finished []string
	for {
		end := bytes.IndexByte(c.partial, '\n')
		if end < 0 {
			break
		}
		finished = append(finished, c.finish(c.partial[:end]))
		c.partial = c.partial[end+1:]
	}
	for len(c.partial) >= partialLimit {
		finished = append(finished, c.finish(c.partial[:partialLimit]))
		c.partial = c.partial[partialLimit:]
	}
	// What's left must not keep a huge buffer alive through the slices above.
	c.partial = append([]byte(nil), c.partial...)
	return finished
}

func (c *console) finish(raw []byte) string {
	text := clean(raw)
	c.push(line{text: text, kind: kindOf(text)})
	return text
}

func (c *console) push(l line) {
	c.lines = append(c.lines, l)
	if len(c.lines) > c.limit {
		// Slicing is free, and append copies what's left only once it runs out of room.
		c.lines = c.lines[len(c.lines)-c.limit:]
	}
}

// add puts a line of preflash's own after everything so far, ending any line part way
// through arriving.
func (c *console) add(text string, kind lineKind) {
	c.flush()
	c.push(line{text: strings.ToValidUTF8(cleanText(text), "�"), kind: kind})
}

// flush ends a line that never got its newline, and gives it back if there was one.
func (c *console) flush() (string, bool) {
	if len(c.partial) == 0 {
		return "", false
	}
	text := c.finish(c.partial)
	c.partial = nil
	return text, true
}

func (c *console) clear() {
	c.lines = nil
	c.partial = nil
}

// clean makes a raw line safe to draw. A line that rewrites itself with carriage returns,
// as a progress bar does, keeps its last version.
func clean(raw []byte) string {
	raw = bytes.TrimRight(raw, "\r")
	if at := bytes.LastIndexByte(raw, '\r'); at >= 0 {
		raw = raw[at+1:]
	}
	return cleanText(strings.ToValidUTF8(string(raw), "�"))
}

// cleanText removes escape sequences and replaces control characters, so that nothing from
// the firmware can move the cursor or change how the screen reads.
func cleanText(s string) string {
	s = strings.ReplaceAll(ansi.Strip(s), "\t", strings.Repeat(" ", tabWidth))
	return strings.Map(func(r rune) rune {
		switch {
		case unicode.IsControl(r):
			return ' '
		case unicode.Is(unicode.Cf, r):
			return -1
		}
		return r
	}, s)
}

// kindOf reads ESP-IDF's log prefix.
func kindOf(text string) lineKind {
	switch {
	case strings.HasPrefix(text, "E ("):
		return lineBad
	case strings.HasPrefix(text, "W ("):
		return lineWarn
	case strings.HasPrefix(text, "I ("), strings.HasPrefix(text, "D ("), strings.HasPrefix(text, "V ("):
		return lineDim
	}
	return lineText
}

// wrapped breaks a line into rows of at most width cells.
func wrapped(text string, width int) []string {
	// Bytes are never fewer than cells, so short lines skip the wrapping.
	if len(text) <= width {
		return []string{text}
	}
	return strings.Split(ansi.Hardwrap(text, width, true), "\n")
}

// rowCount is how many rows text takes.
func rowCount(text string, width int) int {
	if len(text) <= width {
		return 1
	}
	return len(wrapped(text, width))
}

// partialRows is how many rows the line still arriving takes.
func (c *console) partialRows(width int) int {
	if len(c.partial) == 0 {
		return 0
	}
	return rowCount(clean(c.partial), width)
}

// rows gives the height rows that end scroll rows above the newest, which can be fewer on a
// short console, with scroll as it stands once held to what there is.
func (c *console) rows(width, height, scroll int) ([]row, int) {
	need := scroll + height
	var reversed []row
	take := func(text string, kind lineKind) {
		parts := wrapped(text, width)
		for i := len(parts) - 1; i >= 0 && len(reversed) < need; i-- {
			reversed = append(reversed, row{text: parts[i], kind: kind})
		}
	}
	if len(c.partial) > 0 {
		text := clean(c.partial)
		take(text, kindOf(text))
	}
	for i := len(c.lines) - 1; i >= 0 && len(reversed) < need; i-- {
		take(c.lines[i].text, c.lines[i].kind)
	}
	scroll = min(scroll, max(len(reversed)-height, 0))
	shown := reversed[scroll:min(scroll+height, len(reversed))]
	out := make([]row, len(shown))
	for i, r := range shown {
		out[len(shown)-1-i] = r
	}
	return out, scroll
}

// texts gives every line's text, the one still arriving included, for tests.
func (c *console) texts() []string {
	out := make([]string, 0, len(c.lines)+1)
	for _, l := range c.lines {
		out = append(out, l.text)
	}
	if len(c.partial) > 0 {
		out = append(out, clean(c.partial))
	}
	return out
}
