package check

import (
	"context"
	"encoding/hex"
	"fmt"
	"slices"

	"github.com/edcs/preflash/internal/board"
)

// Answer is what the model made of one transaction.
type Answer struct {
	Acked   bool   `json:"acked"`
	Read    string `json:"read"`
	Refused string `json:"refused,omitempty"`
}

// Replay runs transactions against the board's models, one answer for each.
type Replay func(context.Context, board.Board, []Transaction) ([]Answer, error)

// Difference is one place the real chip and the model disagree. Reason says why that's no
// surprise, and is empty for a mismatch.
type Difference struct {
	Seq      int
	Chip     string
	Address  int
	Register int
	// Byte is the place in the reply, for a byte that has no register.
	Byte   int
	Real   string
	Model  string
	Reason string
}

// Mismatch says this difference needs looking at.
func (d Difference) Mismatch() bool { return d.Reason == "" }

// ChipResult is how one chip of the board did.
type ChipResult struct {
	Chip         string
	Bus          int
	Address      int
	Transactions int
	Matched      int
	Expected     int
	Mismatched   int
	Differences  []Difference
}

// Totals counts transactions by how they did. A transaction with a mismatch counts as that,
// whatever else differed. Uncaptured counts the board's chips that the capture left out.
type Totals struct {
	Transactions int
	Matched      int
	Expected     int
	Mismatched   int
	Uncaptured   int
}

// Report is the outcome of a check.
type Report struct {
	Path    string
	Capture Capture
	Board   board.Board
	Chips   []ChipResult
}

// Compare replays the capture against the board's models.
func Compare(ctx context.Context, capture Capture, b board.Board, replay Replay) (Report, error) {
	if capture.Board != b.Name && capture.Board != b.Base {
		return Report{}, fmt.Errorf("this capture is of %s, not %s", capture.Board, b.Name)
	}
	answers, err := replay(ctx, b, capture.Transactions)
	if err != nil {
		return Report{}, err
	}
	if len(answers) < len(capture.Transactions) {
		next := capture.Transactions[len(answers)]
		return Report{}, fmt.Errorf("the chip models stopped before transaction #%d, on %s", next.Seq, next.Chip)
	}

	report := Report{Capture: capture, Board: b}
	for _, chip := range b.I2C {
		report.Chips = append(report.Chips, ChipResult{Chip: chip.Chip, Bus: chip.Bus, Address: chip.Address})
	}
	for i, t := range capture.Transactions {
		at := slices.IndexFunc(report.Chips, func(c ChipResult) bool {
			return c.Chip == t.Chip && c.Bus == t.Bus && c.Address == t.Addr
		})
		if at < 0 {
			report.Chips = append(report.Chips, ChipResult{Chip: t.Chip, Bus: t.Bus, Address: t.Addr})
			at = len(report.Chips) - 1
		}
		report.Chips[at].add(t, differences(t, answers[i], b))
	}
	return report, nil
}

func (r *ChipResult) add(t Transaction, found []Difference) {
	r.Transactions++
	r.Differences = append(r.Differences, found...)
	switch {
	case len(found) == 0:
		r.Matched++
	case slices.ContainsFunc(found, Difference.Mismatch):
		r.Mismatched++
	default:
		r.Expected++
	}
}

func differences(t Transaction, answer Answer, b board.Board) []Difference {
	whole := Difference{Seq: t.Seq, Chip: t.Chip, Address: t.Addr, Register: registerAt(t.Chip, t.Write, 0)}
	mismatch := func(real, model string) []Difference {
		whole.Real, whole.Model = real, model
		return []Difference{whole}
	}
	if !slices.ContainsFunc(b.I2C, func(c board.Chip) bool {
		return c.Chip == t.Chip && c.Bus == t.Bus && c.Address == t.Addr
	}) {
		return mismatch(t.Err, fmt.Sprintf("no %s at 0x%02x on bus %d", t.Chip, t.Addr, t.Bus))
	}
	if answer.Refused != "" {
		return mismatch(t.Err, "refused: "+answer.Refused)
	}
	if t.Acked() != answer.Acked {
		model := "no ACK"
		if answer.Acked {
			model = "acked"
		}
		return mismatch(t.Err, model)
	}
	if !t.Acked() {
		return nil
	}
	model, err := hex.DecodeString(answer.Read)
	if err != nil || len(model) != len(t.Read) {
		return mismatch(fmt.Sprintf("%d bytes", len(t.Read)), fmt.Sprintf("the reply %q", answer.Read))
	}
	var found []Difference
	for i, real := range t.Read {
		if real == model[i] {
			continue
		}
		register := registerAt(t.Chip, t.Write, i)
		found = append(found, Difference{
			Seq: t.Seq, Chip: t.Chip, Address: t.Addr, Register: register, Byte: i,
			Real: fmt.Sprintf("0x%02x", real), Model: fmt.Sprintf("0x%02x", model[i]),
			Reason: expectedReason(t.Chip, register),
		})
	}
	return found
}

// Totals adds up the chips.
func (r Report) Totals() Totals {
	var totals Totals
	for _, chip := range r.Chips {
		totals.Transactions += chip.Transactions
		totals.Matched += chip.Matched
		totals.Expected += chip.Expected
		totals.Mismatched += chip.Mismatched
		if chip.Transactions == 0 {
			totals.Uncaptured++
		}
	}
	return totals
}

// Failed says something needs looking at.
func (r Report) Failed() bool {
	totals := r.Totals()
	return totals.Mismatched > 0 || totals.Uncaptured > 0
}

// Differences lists every difference, in the order of the capture.
func (r Report) Differences() []Difference {
	var all []Difference
	for _, chip := range r.Chips {
		all = append(all, chip.Differences...)
	}
	slices.SortStableFunc(all, func(a, b Difference) int { return a.Seq - b.Seq })
	return all
}
