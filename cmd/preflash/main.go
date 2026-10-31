// Command preflash runs ESP-IDF firmware on a Mac, against simulated boards.
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"

	"github.com/edcs/preflash/internal/project"
)

const usage = "usage: preflash run [--project dir] [--board file] [--headless]\n"

func main() {
	if len(os.Args) < 2 || os.Args[1] != "run" {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	if err := run(os.Args[2:]); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprint(os.Stdout, usage)
			return
		}
		fmt.Fprintf(os.Stderr, "preflash: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	flags := flag.NewFlagSet("run", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	dir := flags.String("project", ".", "project folder")
	boardName := flags.String("board", "", "board file, or a stock board's name")
	headless := flags.Bool("headless", false, "run with no window")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() > 0 {
		return fmt.Errorf("unexpected argument %q", flags.Arg(0))
	}

	p, err := project.Find(*dir)
	if err != nil {
		return err
	}
	b, err := p.Board(*boardName)
	if err != nil {
		return err
	}
	return p.Run(b, *headless)
}
