package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"text/tabwriter"

	"github.com/edcs/preflash/internal/client"
)

var traceStreams = []string{"i2c", "gpio", "task", "nvs", "net"}

func trace(args []string) error {
	flags, dir := newFlags("trace")
	stream := flags.String("stream", "", "show one stream: i2c, gpio, task, nvs or net")
	asJSON := flags.Bool("json", false, "print JSON")
	save := flags.String("save", "", "save what the app has kept to this file and stop")
	if err := noArguments(flags, args); err != nil {
		return err
	}
	if *stream != "" && !slices.Contains(traceStreams, *stream) {
		return fmt.Errorf("trace has no stream %q: try %s", *stream, strings.Join(traceStreams, ", "))
	}
	if *save != "" {
		return saveTrace(*dir, *save)
	}
	socket, err := socketPath(*dir)
	if err != nil {
		return err
	}
	return describe(client.Watch(socket, client.Watcher{
		Streams: []string{"trace"},
		Trace:   traceWriter(*stream, *asJSON),
	}), *dir)
}

// traceWriter prints each record the stream names, and warns of any gap. A new boot's numbers
// start again, which is no gap.
func traceWriter(stream string, asJSON bool) func(client.TraceRecord) {
	next := uint64(0)
	encoder := json.NewEncoder(os.Stdout)
	return func(r client.TraceRecord) {
		if r.Seq > next {
			fmt.Fprintf(os.Stderr, "preflash: %d trace records were lost\n", r.Seq-next)
		}
		next = r.Seq + 1
		if stream != "" && r.Stream != stream {
			return
		}
		if asJSON {
			encoder.Encode(r)
			return
		}
		fmt.Println(traceLine(r))
	}
}

func traceLine(r client.TraceRecord) string {
	subject := r.Subject
	if r.Device != "" {
		subject += " " + r.Device
	}
	return fmt.Sprintf("%-12s %-5s %-24s %-12s %s", r.Time(), r.Stream, subject, r.Op, r.Detail)
}

// The app writes the file, and its working folder is not this one.
func saveTrace(dir, path string) error {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return err
	}
	return withApp(dir, func(c *client.Client) error {
		saved, records, err := c.SaveTrace(absolute)
		if err != nil {
			return err
		}
		fmt.Printf("saved %d records to %s\n", records, saved)
		return nil
	})
}

func hardware(args []string) error {
	flags, dir := newFlags("hardware")
	asJSON := flags.Bool("json", false, "print JSON")
	words, err := parse(flags, args)
	if err != nil {
		return err
	}
	if len(words) != 0 && len(words) != 2 {
		return errors.New("hardware needs a name and a value, or neither")
	}
	return withApp(*dir, func(c *client.Client) error {
		var items []client.HardwareItem
		var err error
		if len(words) == 2 {
			items, err = c.SetHardware(words[0], words[1])
		} else {
			items, err = c.Hardware()
		}
		switch {
		case err != nil:
			return err
		case *asJSON:
			return json.NewEncoder(os.Stdout).Encode(items)
		case len(words) == 2:
			fmt.Println(appliedLine(items, words[0]))
			return nil
		}
		return printHardware(items)
	})
}

// appliedLine says whether a change is in force yet.
func appliedLine(items []client.HardwareItem, name string) string {
	at := slices.IndexFunc(items, func(item client.HardwareItem) bool { return item.Name == name })
	if at < 0 {
		return name + " changed"
	}
	item := items[at]
	if item.Waiting() {
		return fmt.Sprintf("%s: %s on the next boot", item.Name, item.Label(item.Next))
	}
	return fmt.Sprintf("%s: %s", item.Name, item.Label(item.Value))
}

func printHardware(items []client.HardwareItem) error {
	table := tabwriter.NewWriter(os.Stdout, 0, 0, 2, ' ', 0)
	for _, item := range items {
		values := make([]string, len(item.Options))
		for i, option := range item.Options {
			values[i] = option.Value
		}
		shown := item.Label(item.Value)
		if item.Waiting() {
			shown += ", " + item.Label(item.Next) + " on the next boot"
		}
		fmt.Fprintf(table, "%s\t%s\t%s\n", item.Name, shown, strings.Join(values, " "))
	}
	return table.Flush()
}
