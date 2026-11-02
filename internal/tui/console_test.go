package tui

import (
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestConsoleJoinsChunksIntoLines(t *testing.T) {
	c := newConsole()

	assert.Equal(t, []string{"one"}, c.feed([]byte("one\ntw")))
	assert.Equal(t, []string{"two"}, c.feed([]byte("o\r\n")))

	assert.Equal(t, []string{"one", "two"}, c.texts())
}

func TestConsoleShowsALineStillArriving(t *testing.T) {
	c := newConsole()
	assert.Empty(t, c.feed([]byte("Enter name: ")))
	assert.Equal(t, []string{"Enter name: "}, c.texts())
	c.feed([]byte("x\n"))
	assert.Equal(t, []string{"Enter name: x"}, c.texts())
}

func TestConsoleKeepsAUTF8SequenceSplitAcrossChunks(t *testing.T) {
	c := newConsole()
	degree := []byte("90°\n")
	c.feed(degree[:3])
	c.feed(degree[3:])
	assert.Equal(t, []string{"90°"}, c.texts())
}

func TestConsoleCleansEscapesAndControlCharacters(t *testing.T) {
	c := newConsole()
	c.feed([]byte("\x1b[0;32mI (12) tag: hi\x1b[0m\n"))
	c.feed([]byte("a\tb\x07\n"))
	c.feed([]byte{0xff, 'x', '\n'})
	assert.Equal(t, []string{"I (12) tag: hi", "a    b ", "�x"}, c.texts())
}

func TestConsoleKeepsAnEscapeSplitAcrossChunks(t *testing.T) {
	c := newConsole()
	c.feed([]byte("\x1b[0;3"))
	c.feed([]byte("2mgreen\x1b[0m\n"))
	assert.Equal(t, []string{"green"}, c.texts())
}

func TestABareCarriageReturnStartsTheLineAgain(t *testing.T) {
	c := newConsole()
	c.feed([]byte("10%\r20%\r30%\n"))
	assert.Equal(t, []string{"30%"}, c.texts())
}

func TestConsoleForgetsItsOldestLines(t *testing.T) {
	c := newConsole()
	c.limit = 3
	c.feed([]byte("1\n2\n3\n4\n5\n"))
	assert.Equal(t, []string{"3", "4", "5"}, c.texts())
}

func TestAVeryLongLineIsCutInPieces(t *testing.T) {
	c := newConsole()
	c.feed([]byte(strings.Repeat("x", partialLimit+5)))
	assert.Len(t, c.lines, 1)
	assert.Len(t, c.lines[0].text, partialLimit)
}

func TestLogLevelsSetTheKind(t *testing.T) {
	c := newConsole()
	c.feed([]byte("I (1) a\nW (2) b\nE (3) c\nplain\n"))
	var kinds []lineKind
	for _, l := range c.lines {
		kinds = append(kinds, l.kind)
	}
	assert.Equal(t, []lineKind{lineDim, lineWarn, lineBad, lineText}, kinds)
}

func TestRowsWrapLongLinesFromTheBottom(t *testing.T) {
	c := newConsole()
	c.feed([]byte("first\n123456789\nlast\n"))

	rows, _ := c.rows(4, 3, 0)
	assert.Equal(t, []string{"5678", "9", "last"}, plain(rows))

	rows, _ = c.rows(4, 3, 2)
	assert.Equal(t, []string{"t", "1234", "5678"}, plainWide(rows))
}

func TestScrollStopsAtTheTop(t *testing.T) {
	c := newConsole()
	c.feed([]byte("a\nb\nc\n"))
	rows, scroll := c.rows(10, 2, 99)
	assert.Equal(t, 1, scroll)
	assert.Equal(t, []string{"a", "b"}, plain(rows))
}

func TestAShortConsoleSitsAtTheBottom(t *testing.T) {
	c := newConsole()
	c.feed([]byte("a\n"))
	rows, _ := c.rows(10, 3, 0)
	assert.Equal(t, []string{"a"}, plain(rows), "the caller pads above")
}

func plain(rows []row) []string { return plainWide(rows) }

func plainWide(rows []row) []string {
	out := make([]string, len(rows))
	for i, r := range rows {
		out[i] = r.text
	}
	return out
}

func TestAFullConsoleDoesNotCopyItselfForEachLine(t *testing.T) {
	c := newConsole()
	c.limit = 1000
	for range 2000 {
		c.push(line{text: "x"})
	}
	allocations := testing.AllocsPerRun(1000, func() { c.push(line{text: "x"}) })
	assert.Less(t, allocations, 0.1)
	assert.Len(t, c.lines, 1000)
}
