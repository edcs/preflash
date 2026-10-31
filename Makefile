.PHONY: test

test:
	go test ./...
	cmake -S tests/runtime -B build/tests
	cmake --build build/tests --parallel
	ctest --test-dir build/tests --output-on-failure
	tests/integration/build_fixture.sh
	tests/integration/display.sh
	tests/integration/snoozel.sh
