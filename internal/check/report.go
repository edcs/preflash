package check

import (
	"fmt"
	"io"
)

const syntheticWarning = "SYNTHETIC capture: no real chip took part, so this is not evidence about any real chip."

// Write prints the report. A synthetic capture says so at both ends, so that neither the top nor
// the tail of the output can pass for a result from a real chip.
func (r Report) Write(w io.Writer) error {
	var err error
	printf := func(format string, args ...any) {
		if err == nil {
			_, err = fmt.Fprintf(w, format, args...)
		}
	}
	printf("%s against the models of %s\n", r.Path, r.Board.Name)
	if r.Capture.Synthetic() {
		printf("%s\n", syntheticWarning)
	}
	if r.Capture.Note != "" {
		printf("Note: %s\n", r.Capture.Note)
	}
	printf("\n")
	for _, chip := range r.Chips {
		printf("%s\n", chip.summary())
		for _, d := range chip.Differences {
			printf("  #%d %s: real %s, model %s: %s\n", d.Seq, d.place(), d.Real, d.Model, d.verdict())
		}
	}
	totals := r.Totals()
	printf("\n%d transactions: %d match, %d expected, %d mismatched", totals.Transactions, totals.Matched, totals.Expected, totals.Mismatched)
	if totals.Uncaptured > 0 {
		printf(", %d chips not captured", totals.Uncaptured)
	}
	printf("\n")
	if r.Capture.Synthetic() {
		printf("%s\n", syntheticWarning)
	}
	return err
}

func (r ChipResult) summary() string {
	name := fmt.Sprintf("%s at 0x%02x, bus %d", r.Chip, r.Address, r.Bus)
	if r.Transactions == 0 {
		return name + ": no transactions: MISMATCH"
	}
	plural := "s"
	if r.Transactions == 1 {
		plural = ""
	}
	return fmt.Sprintf("%s: %d transaction%s, %d match, %d expected, %d mismatched", name, r.Transactions, plural, r.Matched, r.Expected, r.Mismatched)
}

func (d Difference) place() string {
	switch {
	case d.Register == noRegister:
		return fmt.Sprintf("byte %d", d.Byte)
	case d.Register > 0xff:
		return fmt.Sprintf("register 0x%04x", d.Register)
	default:
		return fmt.Sprintf("register 0x%02x", d.Register)
	}
}

func (d Difference) verdict() string {
	if d.Mismatch() {
		return "MISMATCH"
	}
	return "expected (" + d.Reason + ")"
}
