package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/edcs/preflash/internal/check"
	"github.com/edcs/preflash/internal/project"
)

// checkCapture replays a capture against the chip models and prints each difference. It fails if
// any needs looking at, so a script can tell.
func checkCapture(args []string) error {
	flags, _ := newFlags("check")
	boardName := flags.String("board", "", "board file, or a stock board's name")
	words, err := parse(flags, args)
	if err != nil {
		return err
	}
	if len(words) != 1 || *boardName == "" {
		return errors.New("check needs a capture file and --board")
	}
	b, err := project.LoadBoard(*boardName)
	if err != nil {
		return err
	}
	file, err := os.Open(words[0])
	if err != nil {
		return err
	}
	defer file.Close()
	capture, err := check.Parse(file)
	if err != nil {
		return fmt.Errorf("%s: %w", words[0], err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	report, err := check.Compare(ctx, capture, b, check.Models)
	if err != nil {
		return err
	}
	report.Path = words[0]
	if err := report.Write(os.Stdout); err != nil {
		return err
	}
	if report.Failed() {
		return errors.New("the capture and the models disagree")
	}
	return nil
}
