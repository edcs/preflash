package main

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestChooseCommand(t *testing.T) {
	for name, test := range map[string]struct {
		args       []string
		inTerminal bool
		command    string
		rest       []string
	}{
		"nothing in a terminal": {nil, true, "tui", nil},
		"nothing in a pipe":     {nil, false, "", nil},
		"a flag in a terminal":  {[]string{"--project", "x"}, true, "tui", []string{"--project", "x"}},
		"a flag in a pipe":      {[]string{"--project", "x"}, false, "tui", []string{"--project", "x"}},
		"a command":             {[]string{"run", "--headless"}, true, "run", []string{"--headless"}},
		"an unknown word":       {[]string{"frob"}, true, "frob", []string{}},
		"--version":             {[]string{"--version"}, false, "version", nil},
		"tui by name":           {[]string{"tui", "--headless"}, false, "tui", []string{"--headless"}},
	} {
		t.Run(name, func(t *testing.T) {
			command, rest := chooseCommand(test.args, test.inTerminal)
			assert.Equal(t, test.command, command)
			assert.Equal(t, len(test.rest), len(rest))
		})
	}
}
