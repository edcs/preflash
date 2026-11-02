package check

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"github.com/edcs/preflash/internal/board"
	"github.com/edcs/preflash/internal/project"
)

// request is one transaction as the replay tool reads it.
type request struct {
	Bus     int    `json:"bus"`
	Addr    int    `json:"addr"`
	Write   string `json:"write"`
	ReadLen int    `json:"read_len"`
}

// Models replays transactions against the real chip models, building the replay tool from this
// checkout's runtime if it needs to.
func Models(ctx context.Context, b board.Board, transactions []Transaction) ([]Answer, error) {
	tool, err := buildTool(ctx)
	if err != nil {
		return nil, err
	}
	return runTool(ctx, tool, b, transactions)
}

func buildTool(ctx context.Context) (string, error) {
	root, err := project.Root()
	if err != nil {
		return "", err
	}
	// Each checkout gets a folder, because CMake won't move a build to another source.
	sum := sha256.Sum256([]byte(root))
	dir := filepath.Join(project.Home(), "replay", hex.EncodeToString(sum[:6]))
	steps := [][]string{
		{"-S", filepath.Join(root, "runtime", "replay"), "-B", dir},
		{"--build", dir, "--parallel", strconv.Itoa(runtime.NumCPU())},
	}
	for _, args := range steps {
		output, err := exec.CommandContext(ctx, "cmake", args...).CombinedOutput()
		if err != nil {
			return "", fmt.Errorf("couldn't build the chip models: cmake %s: %w\n%s", strings.Join(args, " "), err, output)
		}
	}
	return filepath.Join(dir, "replay"), nil
}

// runTool runs the tool on one fresh board: an empty state folder, so that no forced failure from
// a project comes along, and no PREFLASH_ variable from a run this might be inside.
func runTool(ctx context.Context, tool string, b board.Board, transactions []Transaction) ([]Answer, error) {
	dir, err := os.MkdirTemp("", "preflash-check-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	state := filepath.Join(dir, "state")
	if err := os.Mkdir(state, 0o700); err != nil {
		return nil, err
	}
	boardFile := filepath.Join(dir, "board.json")
	resolved, err := board.Resolve(b)
	if err != nil {
		return nil, err
	}
	if err := os.WriteFile(boardFile, resolved, 0o600); err != nil {
		return nil, err
	}

	var input bytes.Buffer
	encoder := json.NewEncoder(&input)
	for _, t := range transactions {
		encoded := request{Bus: t.Bus, Addr: t.Addr, Write: hex.EncodeToString(t.Write), ReadLen: t.ReadLen}
		if err := encoder.Encode(encoded); err != nil {
			return nil, err
		}
	}

	var output, failure bytes.Buffer
	cmd := exec.CommandContext(ctx, tool)
	cmd.Stdin = &input
	cmd.Stdout = &output
	cmd.Stderr = &failure
	cmd.Env = append(withoutPreflash(os.Environ()), "PREFLASH_BOARD="+boardFile, "PREFLASH_STATE="+state)
	if err := cmd.Run(); err != nil {
		var exit *exec.ExitError
		if !errors.As(err, &exit) {
			return nil, err
		}
		return nil, fmt.Errorf("the chip models stopped: %s", strings.TrimSpace(failure.String()))
	}

	var answers []Answer
	decoder := json.NewDecoder(&output)
	for decoder.More() {
		var answer Answer
		if err := decoder.Decode(&answer); err != nil {
			return nil, fmt.Errorf("the chip models answered badly: %w", err)
		}
		answers = append(answers, answer)
	}
	// A model can end the process without an error, as a power chip switching off does.
	if len(answers) < len(transactions) {
		next := transactions[len(answers)]
		return nil, fmt.Errorf("the chip models stopped at transaction #%d, on %s: %s", next.Seq, next.Chip, strings.TrimSpace(failure.String()))
	}
	return answers, nil
}

func withoutPreflash(env []string) []string {
	var kept []string
	for _, entry := range env {
		if !strings.HasPrefix(entry, "PREFLASH_") {
			kept = append(kept, entry)
		}
	}
	return kept
}
