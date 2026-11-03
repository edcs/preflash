.PHONY: test bench bash

# macOS's own bash 3.2 exits 0 after an unbound variable when a script has an EXIT trap,
# which would pass a failing integration script.
bash:
	@bash -c '[ "$${BASH_VERSINFO[0]}" -ge 4 ]' || { echo "make test needs bash 4 or later first on PATH: brew install bash" >&2; exit 1; }

test: bash
	go test ./...
	cmake -S tests/runtime -B build/tests
	cmake --build build/tests --parallel
	ctest --test-dir build/tests --output-on-failure
	tests/integration/build_fixture.sh
	tests/integration/install.sh
	tests/integration/display.sh
	tests/integration/control.sh
	tests/integration/port.sh
	tests/integration/lcd.sh
	tests/integration/debug.sh
	tests/integration/crash.sh
	tests/integration/check.sh
	tests/integration/tui.sh
	tests/integration/demo.sh
	tests/integration/snoozel.sh
	tests/integration/nyx.sh

bench:
	go run ./cmd/preflash bench --project examples/demo
