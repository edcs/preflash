//go:build linux

package crash

import (
	"debug/elf"
	"encoding/hex"
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

// noteGNUBuildID is NT_GNU_BUILD_ID, which debug/elf has no name for.
const noteGNUBuildID = 3

// span is a stretch of the app's address space that holds code.
type span struct{ start, end uint64 }

// fileUUID reads an executable's GNU build ID in lowercase hex, which is how the app
// writes it, or nothing if the file has gone or has none.
func fileUUID(path string) string {
	file, err := elf.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	return buildID(file)
}

func buildID(file *elf.File) string {
	note := file.Section(".note.gnu.build-id")
	if note == nil {
		return ""
	}
	data, err := note.Data()
	if err != nil || len(data) < 16 {
		return ""
	}
	order := file.ByteOrder
	nameSize, descSize := order.Uint32(data[0:]), order.Uint32(data[4:])
	start := 12 + (uint64(nameSize)+3)&^3
	if order.Uint32(data[8:]) != noteGNUBuildID || start+uint64(descSize) > uint64(len(data)) {
		return ""
	}
	return hex.EncodeToString(data[start : start+uint64(descSize)])
}

// codeSpans reads where the executable's code lands in the crashed process. A program that
// isn't position independent is linked to the addresses it runs at, which is why the report's
// load address is zero and its image size is no use as a range. Nothing readable gives nil.
func (r *Report) codeSpans() ([]span, bool) {
	file, err := elf.Open(r.Executable)
	if err != nil {
		return nil, false
	}
	defer file.Close()
	load, _ := parseAddress(r.LoadAddress)
	var spans []span
	for _, prog := range file.Progs {
		if prog.Type == elf.PT_LOAD && prog.Flags&elf.PF_X != 0 {
			spans = append(spans, span{load + prog.Vaddr, load + prog.Vaddr + prog.Memsz})
		}
	}
	return spans, file.Type == elf.ET_EXEC || file.Type == elf.ET_DYN
}

// inImage says whether an address is in the app's own code.
func (r *Report) inImage(address uint64) bool {
	if spans, ok := r.codeSpans(); ok {
		for _, s := range spans {
			if address >= s.start && address < s.end {
				return true
			}
		}
		return false
	}
	load, ok := parseAddress(r.LoadAddress)
	return ok && address >= load && address < load+r.ImageSize
}

// fileOffset is what addr2line wants for an address: where it sits in the file, which only
// differs from the address when the program was loaded at a base of the loader's choosing.
func (r *Report) fileOffset(address uint64) uint64 {
	file, err := elf.Open(r.Executable)
	if err != nil {
		return address
	}
	defer file.Close()
	if file.Type != elf.ET_DYN {
		return address
	}
	load, _ := parseAddress(r.LoadAddress)
	return address - load
}

var (
	addr2lineHeader = regexp.MustCompile(`^0x[0-9a-fA-F]+$`)
	addr2lineSource = regexp.MustCompile(`^(.+):(\d+|\?)(?: \(discriminator \d+\))?$`)
)

// name asks addr2line for each address's frames. With -i one address can give several, for
// inlining, and -a heads each address's lines with the address.
func (r *Report) name(run Runner, asked []string) ([][]Frame, error) {
	args := []string{"-a", "-f", "-i", "-C", "-e", r.Executable}
	for _, text := range asked {
		address, _ := parseAddress(text)
		args = append(args, fmt.Sprintf("0x%x", r.fileOffset(address)))
	}
	output, err := run("addr2line", args...)
	if err != nil {
		return nil, err
	}
	return parseAddr2line(string(output), asked), nil
}

// parseAddr2line gives each address's frames. An answer of ?? is system code, or code with
// no debug information, and stays the address, as atos's unknowns do.
func parseAddr2line(output string, asked []string) [][]Frame {
	var groups [][]Frame
	lines := strings.Split(output, "\n")
	for i := 0; i < len(lines); i++ {
		if !addr2lineHeader.MatchString(strings.TrimSpace(lines[i])) {
			continue
		}
		label := asked[min(len(groups), len(asked)-1)]
		var frames []Frame
		for i+2 < len(lines) && !addr2lineHeader.MatchString(strings.TrimSpace(lines[i+1])) {
			function, place := strings.TrimSpace(lines[i+1]), strings.TrimSpace(lines[i+2])
			i += 2
			frame := Frame{Function: label}
			if function != "??" {
				frame.Function = functionName(function)
			}
			if m := addr2lineSource.FindStringSubmatch(place); m != nil && m[1] != "??" {
				frame.File = m[1]
				frame.Line, _ = strconv.Atoi(m[2])
			}
			frames = append(frames, frame)
		}
		if len(frames) == 0 {
			frames = []Frame{{Function: label}}
		}
		groups = append(groups, frames)
	}
	return groups
}
