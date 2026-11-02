// Package check compares what real chips answered, as saved by the capture firmware, with what
// the chip models answer to the same transactions.
package check

import (
	"bufio"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strings"
)

const (
	captureFormat = 1

	// sourceBoard is firmware that ran on a chip of the real board.
	sourceBoard = "board"
	// sourceSynthetic is anything else: written by hand, or run under Preflash.
	sourceSynthetic = "synthetic"

	espOK = "ESP_OK"

	lastBus     = 1
	lastAddress = 0x7f
)

// Capture is one saved run of the capture firmware.
type Capture struct {
	Board string
	// source is what the file says it came from. It stays private, behind Synthetic,
	// so that anything but a real board counts as synthetic.
	source       string
	Note         string
	Transactions []Transaction
}

// Synthetic says that no real chip took part in this capture.
func (c Capture) Synthetic() bool { return c.source != sourceBoard }

// Transaction is one driver call: a write, then a read if ReadLen isn't 0.
type Transaction struct {
	Seq     int
	Chip    string
	Bus     int
	Addr    int
	Write   []byte
	ReadLen int
	Read    []byte
	Err     string
}

// Acked says the real chip took the transaction.
func (t Transaction) Acked() bool { return t.Err == espOK }

type line struct {
	Type string `json:"type"`

	Format int    `json:"format"`
	Source string `json:"source"`
	Board  string `json:"board"`
	Note   string `json:"note"`

	Seq     int     `json:"seq"`
	Chip    string  `json:"chip"`
	Bus     int     `json:"bus"`
	Addr    int     `json:"addr"`
	Write   string  `json:"write"`
	ReadLen int     `json:"read_len"`
	Read    string  `json:"read"`
	Err     *string `json:"err"`

	Transactions int `json:"transactions"`
}

// Parse reads a capture. It takes nothing on trust: a serial log loses lines, so a line that
// doesn't read, or a capture that doesn't run from its first line to its last, is an error.
func Parse(r io.Reader) (Capture, error) {
	var capture Capture
	number := 0
	finished := false
	scanner := bufio.NewScanner(r)
	for scanner.Scan() {
		number++
		text := strings.TrimSpace(scanner.Text())
		fail := func(err error) (Capture, error) {
			return Capture{}, fmt.Errorf("line %d: %w", number, err)
		}
		if finished {
			return fail(errors.New("the capture has ended, so this line belongs to another run: save one run to a file"))
		}
		var l line
		decoder := json.NewDecoder(strings.NewReader(text))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&l); err != nil {
			return fail(fmt.Errorf("not a capture line, %w: keep only the lines that start with {", err))
		}
		switch {
		case number == 1 && l.Type == "capture":
			if err := capture.setHeader(l); err != nil {
				return fail(err)
			}
		case number == 1:
			return fail(errors.New("a capture starts with its capture line"))
		case l.Type == "tx":
			t, err := l.transaction(len(capture.Transactions) + 1)
			if err != nil {
				return fail(err)
			}
			capture.Transactions = append(capture.Transactions, t)
		case l.Type == "end":
			if l.Transactions != len(capture.Transactions) {
				return fail(fmt.Errorf("the firmware sent %d transactions and this file has %d", l.Transactions, len(capture.Transactions)))
			}
			finished = true
		default:
			return fail(fmt.Errorf("unexpected %q line", l.Type))
		}
	}
	if err := scanner.Err(); err != nil {
		return Capture{}, err
	}
	if number == 0 {
		return Capture{}, errors.New("no capture line: the file is empty")
	}
	if !finished {
		return Capture{}, errors.New("no end line: the capture stopped part way, so run it again")
	}
	return capture, nil
}

func (c *Capture) setHeader(l line) error {
	if l.Format != captureFormat {
		return fmt.Errorf("format %d: this Preflash reads format %d", l.Format, captureFormat)
	}
	if l.Source != sourceBoard && l.Source != sourceSynthetic {
		return fmt.Errorf("source %q: it must say %s or %s", l.Source, sourceBoard, sourceSynthetic)
	}
	if l.Board == "" {
		return errors.New("no board named")
	}
	c.source = l.Source
	c.Board = l.Board
	c.Note = l.Note
	return nil
}

func (l line) transaction(seq int) (Transaction, error) {
	if l.Seq != seq {
		return Transaction{}, fmt.Errorf("transaction %d where %d comes next: a line was lost", l.Seq, seq)
	}
	if l.Chip == "" {
		return Transaction{}, errors.New("no chip named")
	}
	if l.Bus < 0 || l.Bus > lastBus {
		return Transaction{}, fmt.Errorf("bus %d: an ESP32-S3 has buses 0 and 1", l.Bus)
	}
	if l.Addr < 0 || l.Addr > lastAddress {
		return Transaction{}, fmt.Errorf("address %d isn't a 7-bit address", l.Addr)
	}
	if l.Err == nil {
		return Transaction{}, errors.New("no err status")
	}
	write, err := hex.DecodeString(l.Write)
	if err != nil {
		return Transaction{}, fmt.Errorf("write: %w", err)
	}
	read, err := hex.DecodeString(l.Read)
	if err != nil {
		return Transaction{}, fmt.Errorf("read: %w", err)
	}
	t := Transaction{Seq: l.Seq, Chip: l.Chip, Bus: l.Bus, Addr: l.Addr, Write: write, ReadLen: l.ReadLen, Read: read, Err: *l.Err}
	if t.ReadLen < 0 || (t.Acked() && len(t.Read) != t.ReadLen) {
		return Transaction{}, fmt.Errorf("%d bytes read where read_len says %d", len(t.Read), t.ReadLen)
	}
	return t, nil
}
