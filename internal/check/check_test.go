package check

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/edcs/preflash/internal/board"
)

const synthetic = `{"type":"capture","format":1,"source":"synthetic","board":"Test board","note":"written by hand for a test"}
`

func tx(seq int, chip string, addr int, write, read string) string {
	return fmt.Sprintf(`{"type":"tx","seq":%d,"chip":%q,"bus":0,"addr":%d,"write":%q,"read_len":%d,"read":%q,"err":"ESP_OK"}`+"\n",
		seq, chip, addr, write, len(read)/2, read)
}

func end(count int) string { return fmt.Sprintf(`{"type":"end","transactions":%d}`+"\n", count) }

func parsed(t *testing.T, text string) Capture {
	t.Helper()
	capture, err := Parse(strings.NewReader(text))
	require.NoError(t, err)
	return capture
}

func TestParseReadsAWholeCapture(t *testing.T) {
	capture := parsed(t, synthetic+tx(1, "tca9554", 0x20, "03", "ff")+end(1))
	assert.True(t, capture.Synthetic())
	assert.Equal(t, "Test board", capture.Board)
	assert.Equal(t, "written by hand for a test", capture.Note)
	require.Len(t, capture.Transactions, 1)
	got := capture.Transactions[0]
	assert.Equal(t, "tca9554", got.Chip)
	assert.Equal(t, []byte{0x03}, got.Write)
	assert.Equal(t, []byte{0xff}, got.Read)
	assert.True(t, got.Acked())
}

func TestParseToleratesWhatASerialLogLeavesOnALine(t *testing.T) {
	capture := parsed(t, strings.ReplaceAll(synthetic+end(0), "\n", "\r\n"))
	assert.Empty(t, capture.Transactions)
}

func TestParseRefuses(t *testing.T) {
	line := tx(1, "tca9554", 0x20, "03", "ff")
	for name, test := range map[string]struct{ text, want string }{
		"nothing":              {"", "no capture line"},
		"no label":             {`{"type":"capture","format":1,"board":"B"}` + "\n" + end(0), "source"},
		"an unknown label":     {`{"type":"capture","format":1,"source":"maybe","board":"B"}` + "\n" + end(0), "source"},
		"an unknown format":    {`{"type":"capture","format":9,"source":"synthetic","board":"B"}` + "\n" + end(0), "format"},
		"no board":             {`{"type":"capture","format":1,"source":"synthetic"}` + "\n" + end(0), "board"},
		"a log line":           {synthetic + "I (12) boot: ready\n" + end(0), "line 2"},
		"a transaction first":  {line, "line 1"},
		"no end":               {synthetic + line, "end line"},
		"a lost line":          {synthetic + tx(2, "tca9554", 0x20, "03", "ff") + end(1), "line 2"},
		"the wrong count":      {synthetic + line + end(2), "line 3"},
		"lines after the end":  {synthetic + end(0) + line, "line 3"},
		"a second header":      {synthetic + synthetic + end(0), "line 2"},
		"bad hex":              {synthetic + strings.Replace(line, `"read":"ff"`, `"read":"zz"`, 1) + end(1), "line 2"},
		"a short read":         {synthetic + strings.Replace(line, `"read_len":1`, `"read_len":2`, 1) + end(1), "line 2"},
		"a bus the chip lacks": {synthetic + strings.Replace(line, `"bus":0`, `"bus":2`, 1) + end(1), "line 2"},
		"a 10-bit address":     {synthetic + strings.Replace(line, `"addr":32`, `"addr":128`, 1) + end(1), "line 2"},
		"no status":            {synthetic + strings.Replace(line, `,"err":"ESP_OK"`, "", 1) + end(1), "line 2"},
	} {
		t.Run(name, func(t *testing.T) {
			_, err := Parse(strings.NewReader(test.text))
			require.Error(t, err)
			assert.Contains(t, err.Error(), test.want)
		})
	}
}

func testBoard() board.Board {
	return board.Board{Name: "Test board", I2C: []board.Chip{
		{Chip: "tca9554", Address: 0x20},
		{Chip: "pcf85063", Address: 0x51},
	}}
}

// answers gives each transaction the next answer in the list.
func answers(list ...Answer) Replay {
	return func(context.Context, board.Board, []Transaction) ([]Answer, error) { return list, nil }
}

func acked(read string) Answer { return Answer{Acked: true, Read: read} }

func compare(t *testing.T, text string, replay Replay) Report {
	t.Helper()
	report, err := Compare(context.Background(), parsed(t, text), testBoard(), replay)
	require.NoError(t, err)
	return report
}

func twoChips(pcfRead string) string {
	return synthetic + tx(1, "tca9554", 0x20, "03", "ff") + tx(2, "pcf85063", 0x51, "04", pcfRead) + end(2)
}

func TestCompareAgrees(t *testing.T) {
	report := compare(t, twoChips("12"), answers(acked("ff"), acked("12")))
	assert.Equal(t, Totals{Transactions: 2, Matched: 2}, report.Totals())
}

func TestABareDifferenceIsAMismatch(t *testing.T) {
	text := synthetic + tx(1, "tca9554", 0x20, "03", "f0") + tx(2, "pcf85063", 0x51, "04", "12") + end(2)
	report := compare(t, text, answers(acked("ff"), acked("12")))
	assert.Equal(t, Totals{Transactions: 2, Matched: 1, Mismatched: 1}, report.Totals())
	found := report.Differences()
	require.Len(t, found, 1)
	assert.Equal(t, Difference{Seq: 1, Chip: "tca9554", Address: 0x20, Register: 0x03, Real: "0xf0", Model: "0xff"}, found[0])
}

func TestExpectedDifferencesHaveAReason(t *testing.T) {
	for name, test := range map[string]struct {
		chip         string
		write        string
		register     int
		real, model  string
		reason       string
		wantMismatch bool
	}{
		"the RTC's seconds":        {"pcf85063", "04", 0x04, "11", "12", "time", false},
		"the RTC's year":           {"pcf85063", "0a", 0x0a, "26", "00", "time", false},
		"the RTC's control":        {"pcf85063", "00", 0x00, "01", "00", "", true},
		"an accelerometer reading": {"qmi8658", "36", 0x36, "12", "34", "sensor noise", false},
		"the motion sensor's id":   {"qmi8658", "00", 0x00, "12", "34", "", true},
		"the charger's state":      {"axp2101", "01", 0x01, "12", "34", "power state", false},
		"the fuel gauge":           {"axp2101", "a4", 0xa4, "12", "34", "power state", false},
		"the power chip's id":      {"axp2101", "03", 0x03, "12", "34", "", true},
		"a touch scan":             {"gt911", "814e", 0x814e, "80", "00", "sensor noise", false},
		"the touch chip's id":      {"gt911", "8140", 0x8140, "39", "00", "", true},
	} {
		t.Run(name, func(t *testing.T) {
			b := board.Board{Name: "Test board", I2C: []board.Chip{{Chip: test.chip, Address: 0x10}}}
			text := synthetic + tx(1, test.chip, 0x10, test.write, test.real) + end(1)
			report, err := Compare(context.Background(), parsed(t, text), b, answers(acked(test.model)))
			require.NoError(t, err)
			found := report.Differences()
			require.Len(t, found, 1)
			assert.Equal(t, test.register, found[0].Register)
			assert.Equal(t, test.reason, found[0].Reason)
			assert.Equal(t, test.wantMismatch, found[0].Mismatch())
		})
	}
}

func TestABurstIsComparedByteByByte(t *testing.T) {
	text := synthetic + tx(1, "pcf85063", 0x51, "03", "aa0102") + end(1)
	b := board.Board{Name: "Test board", I2C: []board.Chip{{Chip: "pcf85063", Address: 0x51}}}
	report, err := Compare(context.Background(), parsed(t, text), b, answers(acked("000905")))
	require.NoError(t, err)
	found := report.Differences()
	require.Len(t, found, 3)
	assert.Equal(t, []int{0x03, 0x04, 0x05}, []int{found[0].Register, found[1].Register, found[2].Register})
	assert.True(t, found[0].Mismatch())
	assert.Equal(t, "time", found[1].Reason)
	assert.Equal(t, Totals{Transactions: 1, Mismatched: 1}, report.Totals())
}

func TestRegistersFollowEachChipsAddressing(t *testing.T) {
	for name, test := range map[string]struct {
		chip  string
		write []byte
		at    int
		want  int
	}{
		"the expander stays where it is": {"tca9554", []byte{0x01}, 1, 0x01},
		"the RTC wraps":                  {"pcf85063", []byte{0x11}, 1, 0x00},
		"the touch chip counts in 16":    {"gt911", []byte{0x81, 0x4e}, 2, 0x8150},
		"a register chip moves on":       {"qmi8658", []byte{0x35}, 2, 0x37},
		"a command has no register":      {"axs15231b", []byte{0xb5, 0xab}, 0, noRegister},
		"no write has none":              {"qmi8658", nil, 0, noRegister},
		"half a 16-bit address has none": {"gt911", []byte{0x81}, 0, noRegister},
	} {
		t.Run(name, func(t *testing.T) {
			assert.Equal(t, test.want, registerAt(test.chip, test.write, test.at))
		})
	}
}

func TestFailuresAreMismatches(t *testing.T) {
	fails := strings.Replace(tx(1, "tca9554", 0x20, "03", ""), `"err":"ESP_OK"`, `"err":"ESP_ERR_TIMEOUT"`, 1)
	for name, test := range map[string]struct {
		text   string
		answer Answer
		want   Totals
		real   string
		model  string
	}{
		"the chip failed":  {synthetic + fails + end(1), acked(""), Totals{Transactions: 1, Mismatched: 1}, "ESP_ERR_TIMEOUT", "acked"},
		"the model failed": {synthetic + tx(1, "tca9554", 0x20, "03", "ff") + end(1), Answer{}, Totals{Transactions: 1, Mismatched: 1}, "ESP_OK", "no ACK"},
		"the model refused": {synthetic + tx(1, "tca9554", 0x20, "03", "ff") + end(1), Answer{Refused: "tca9554 register 0x03: no"},
			Totals{Transactions: 1, Mismatched: 1}, "ESP_OK", "refused: tca9554 register 0x03: no"},
	} {
		t.Run(name, func(t *testing.T) {
			b := board.Board{Name: "Test board", I2C: []board.Chip{{Chip: "tca9554", Address: 0x20}}}
			report, err := Compare(context.Background(), parsed(t, test.text), b, answers(test.answer))
			require.NoError(t, err)
			assert.Equal(t, test.want, report.Totals())
			found := report.Differences()
			require.Len(t, found, 1)
			assert.Equal(t, test.real, found[0].Real)
			assert.Equal(t, test.model, found[0].Model)
		})
	}
}

func TestBothFailingAgrees(t *testing.T) {
	fails := strings.Replace(tx(1, "tca9554", 0x20, "03", ""), `"err":"ESP_OK"`, `"err":"ESP_ERR_INVALID_STATE"`, 1)
	b := board.Board{Name: "Test board", I2C: []board.Chip{{Chip: "tca9554", Address: 0x20}}}
	report, err := Compare(context.Background(), parsed(t, synthetic+fails+end(1)), b, answers(Answer{}))
	require.NoError(t, err)
	assert.Equal(t, Totals{Transactions: 1, Matched: 1}, report.Totals())
}

func TestABoardChipWithNoTransactionsIsAMismatch(t *testing.T) {
	text := synthetic + tx(1, "tca9554", 0x20, "03", "ff") + end(1)
	report := compare(t, text, answers(acked("ff")))
	assert.Equal(t, Totals{Transactions: 1, Matched: 1, Uncaptured: 1}, report.Totals())
	assert.True(t, report.Failed())
}

func TestAChipTheBoardDoesntHaveIsAMismatch(t *testing.T) {
	text := twoChips("12")
	text = strings.Replace(text, `"chip":"tca9554"`, `"chip":"gt911"`, 1)
	report := compare(t, text, answers(acked("ff"), acked("12")))
	found := report.Differences()
	require.Len(t, found, 1)
	assert.True(t, found[0].Mismatch())
	assert.Contains(t, found[0].Model, "no gt911 at 0x20")
}

func TestTheCaptureMustBeOfTheBoard(t *testing.T) {
	never := func(context.Context, board.Board, []Transaction) ([]Answer, error) {
		t.Fatal("replayed against the wrong board")
		return nil, nil
	}
	other := testBoard()
	other.Name = "Another board"
	_, err := Compare(context.Background(), parsed(t, twoChips("12")), other, never)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "Test board")
	assert.Contains(t, err.Error(), "Another board")
}

func TestACaptureOfTheBoardAProjectExtendsWillDo(t *testing.T) {
	modded := testBoard()
	modded.Name = "my-modded-board"
	modded.Base = "Test board"
	_, err := Compare(context.Background(), parsed(t, twoChips("12")), modded, answers(acked("ff"), acked("12")))
	require.NoError(t, err)
}

func TestAReplayThatFailsIsAnError(t *testing.T) {
	failing := func(context.Context, board.Board, []Transaction) ([]Answer, error) {
		return nil, errors.New("no compiler")
	}
	_, err := Compare(context.Background(), parsed(t, twoChips("12")), testBoard(), failing)
	assert.ErrorContains(t, err, "no compiler")
}

func TestAReplayThatStopsEarlyNamesTheTransaction(t *testing.T) {
	_, err := Compare(context.Background(), parsed(t, twoChips("12")), testBoard(), answers(acked("ff")))
	require.Error(t, err)
	assert.Contains(t, err.Error(), "stopped at transaction #2")
}

func written(t *testing.T, report Report) string {
	t.Helper()
	var out strings.Builder
	require.NoError(t, report.Write(&out))
	return out.String()
}

func TestASyntheticReportSaysSoAtBothEnds(t *testing.T) {
	report := compare(t, twoChips("12"), answers(acked("ff"), acked("12")))
	report.Path = "made-up.jsonl"
	out := written(t, report)
	lines := strings.Split(strings.TrimSpace(out), "\n")
	assert.Contains(t, lines[1], "SYNTHETIC")
	assert.Contains(t, lines[len(lines)-1], "SYNTHETIC")
	assert.Contains(t, out, "not evidence about any real chip")
	assert.Contains(t, out, "written by hand for a test")
}

func TestARealReportDoesNotSayItIsSynthetic(t *testing.T) {
	text := strings.Replace(twoChips("12"), `"source":"synthetic"`, `"source":"board"`, 1)
	report := compare(t, text, answers(acked("ff"), acked("12")))
	assert.NotContains(t, written(t, report), "SYNTHETIC")
}

func TestTheReportMarksEachDifference(t *testing.T) {
	text := synthetic + tx(1, "tca9554", 0x20, "03", "f0") + tx(2, "pcf85063", 0x51, "04", "12") + end(2)
	report := compare(t, text, answers(acked("ff"), acked("13")))
	out := written(t, report)
	assert.Contains(t, out, "#1 register 0x03: real 0xf0, model 0xff: MISMATCH")
	assert.Contains(t, out, "#2 register 0x04: real 0x12, model 0x13: expected (time)")
	assert.Contains(t, out, "tca9554 at 0x20, bus 0: 1 transaction, 0 match, 0 expected, 1 mismatched")
}

func TestTheReportNamesAChipNobodyCaptured(t *testing.T) {
	text := synthetic + tx(1, "tca9554", 0x20, "03", "ff") + end(1)
	report := compare(t, text, answers(acked("ff")))
	assert.Contains(t, written(t, report), "pcf85063 at 0x51, bus 0: no transactions: MISMATCH")
}

func TestEveryFixtureSaysItIsSynthetic(t *testing.T) {
	paths, err := filepath.Glob("../../tests/fixtures/captures/*.jsonl")
	require.NoError(t, err)
	require.NotEmpty(t, paths)
	for _, path := range paths {
		file, err := os.Open(path)
		require.NoError(t, err)
		defer file.Close()
		capture, err := Parse(file)
		require.NoError(t, err, path)
		assert.True(t, capture.Synthetic(), path)
	}
}

// The folder holds evidence about real chips, so nothing synthetic may land there.
func TestNoSyntheticCaptureAmongTheRealOnes(t *testing.T) {
	paths, err := filepath.Glob("../../hardware/captures/*.jsonl")
	require.NoError(t, err)
	for _, path := range paths {
		file, err := os.Open(path)
		require.NoError(t, err)
		defer file.Close()
		capture, err := Parse(file)
		require.NoError(t, err, path)
		assert.False(t, capture.Synthetic(), path)
	}
}
