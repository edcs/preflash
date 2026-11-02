package check

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/board"
)

// fakeTool writes a shell script to stand in for the replay tool.
func fakeTool(t *testing.T, script string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "replay")
	require.NoError(t, os.WriteFile(path, []byte("#!/bin/sh\n"+script+"\n"), 0o755))
	return path
}

func oneTransaction() []Transaction {
	return []Transaction{{Seq: 1, Chip: "tca9554", Bus: 0, Addr: 0x20, Write: []byte{0x03}, ReadLen: 1}}
}

func TestRunToolPassesTheTransactionsAndReadsTheAnswers(t *testing.T) {
	tool := fakeTool(t, `read line; case "$line" in
*'"bus":0'*'"addr":32'*'"write":"03"'*'"read_len":1'*) echo '{"acked":true,"read":"ff"}';;
*) echo "bad line: $line" >&2; exit 1;;
esac`)
	found, err := runTool(context.Background(), tool, board.Board{Name: "B"}, oneTransaction())
	require.NoError(t, err)
	assert.Equal(t, []Answer{{Acked: true, Read: "ff"}}, found)
}

func TestRunToolGivesTheBoardAndAnEmptyStateAndNoForcedFailures(t *testing.T) {
	t.Setenv("PREFLASH_STATE", "/somewhere/with/faults")
	t.Setenv("PREFLASH_RTC_START", "2026-01-01T00:00:00Z")
	tool := fakeTool(t, `cat "$PREFLASH_BOARD" | grep -q '"name":"B"' || exit 2
[ "$(ls -A "$PREFLASH_STATE")" = "" ] || exit 3
[ -z "$PREFLASH_RTC_START" ] || exit 4
echo '{"acked":true,"read":"ff"}'`)
	_, err := runTool(context.Background(), tool, board.Board{Name: "B"}, oneTransaction())
	require.NoError(t, err)
}

func TestRunToolReportsWhyItStopped(t *testing.T) {
	tool := fakeTool(t, `echo "the power chip switched the board off" >&2; exit 0`)
	_, err := runTool(context.Background(), tool, board.Board{Name: "B"}, oneTransaction())
	require.Error(t, err)
	assert.Contains(t, err.Error(), "#1")
	assert.Contains(t, err.Error(), "switched the board off")

	tool = fakeTool(t, `echo "board: no model for chip x" >&2; exit 1`)
	_, err = runTool(context.Background(), tool, board.Board{Name: "B"}, oneTransaction())
	require.Error(t, err)
	assert.Contains(t, err.Error(), "no model for chip x")
}
