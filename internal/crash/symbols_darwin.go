//go:build darwin

package crash

import (
	"debug/macho"
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

const (
	// uuidCommand is LC_UUID, which debug/macho hands over as raw bytes.
	uuidCommand    = 0x1b
	uuidCommandLen = 24
)

// fileUUID reads an executable's build ID, or nothing if it has gone.
func fileUUID(path string) string {
	file, err := macho.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	for _, load := range file.Loads {
		raw := load.Raw()
		if len(raw) >= uuidCommandLen && binary.LittleEndian.Uint32(raw) == uuidCommand {
			id := raw[8:uuidCommandLen]
			return strings.ToUpper(fmt.Sprintf("%x-%x-%x-%x-%x", id[:4], id[4:6], id[6:8], id[8:10], id[10:]))
		}
	}
	return ""
}

// inImage says whether an address is inside the app, which sits at the one load address.
func (r *Report) inImage(address uint64) bool {
	load, ok := parseAddress(r.LoadAddress)
	return ok && address >= load && address < load+r.ImageSize
}

// name asks atos for each address's frames.
func (r *Report) name(run Runner, asked []string) ([][]Frame, error) {
	args := append([]string{"-o", r.symbols(run), "-l", r.LoadAddress, "-i", "-fullPath"}, asked...)
	output, err := run("atos", args...)
	if err != nil {
		return nil, err
	}
	return parseAtos(string(output)), nil
}

// symbols gives the file for atos to read. The build leaves debug information in its objects,
// and atos only reads a dSYM, so this gathers one the first time an app crashes. That takes
// seconds, which nobody should pay on every build.
func (r *Report) symbols(run Runner) string {
	dsym := r.Executable + ".dSYM"
	dwarf := filepath.Join(dsym, "Contents", "Resources", "DWARF", filepath.Base(r.Executable))
	built, err := os.Stat(r.Executable)
	if err != nil {
		return r.Executable
	}
	if gathered, err := os.Stat(dwarf); err == nil && !gathered.ModTime().Before(built.ModTime()) {
		return dwarf
	}
	if _, err := run("dsymutil", r.Executable, "-o", dsym); err != nil {
		return r.Executable
	}
	if _, err := os.Stat(dwarf); err != nil {
		return r.Executable
	}
	return dwarf
}

// atosLine is atos's answer for an address it knows the source of, and one it doesn't.
var (
	atosSource = regexp.MustCompile(`^(.+) \(in [^)]*\) \((.+):(\d+)\)$`)
	atosSymbol = regexp.MustCompile(`^(.+) \(in [^)]*\)`)
)

// parseAtos gives each address's frames. With -i, one address can be several, for inlining,
// and a blank line ends each.
func parseAtos(output string) [][]Frame {
	var groups [][]Frame
	for _, group := range strings.Split(output, "\n\n") {
		var frames []Frame
		for _, line := range strings.Split(group, "\n") {
			if line = strings.TrimSpace(line); line != "" {
				frames = append(frames, parseAtosLine(line))
			}
		}
		if len(frames) > 0 {
			groups = append(groups, frames)
		}
	}
	return groups
}

func parseAtosLine(line string) Frame {
	if m := atosSource.FindStringSubmatch(line); m != nil {
		number, _ := strconv.Atoi(m[3])
		return Frame{Function: functionName(m[1]), File: m[2], Line: number}
	}
	if m := atosSymbol.FindStringSubmatch(line); m != nil {
		return Frame{Function: functionName(m[1])}
	}
	return Frame{Function: line}
}
