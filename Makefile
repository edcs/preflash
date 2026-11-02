.PHONY: test bench

test:
	go test ./...
	cmake -S tests/runtime -B build/tests
	cmake --build build/tests --parallel
	ctest --test-dir build/tests --output-on-failure
	tests/integration/build_fixture.sh
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
