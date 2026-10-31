# Preflash

Run ESP-IDF firmware on your Mac, against simulated boards.

Preflash builds an unmodified ESP-IDF project for the Mac and runs it in a window the shape of the board's screen. The project's own drivers and UI run. ESP-IDF's services and the board's chips are simulated underneath them, so nothing in the firmware changes.

It's a quick development and debugging tool. It doesn't replace testing on real hardware: see [Known limitations](#known-limitations).

## Install

You need a Mac on Apple Silicon, Go, CMake, Python 3 and SDL2.

```sh
brew install go cmake sdl2
git clone https://github.com/edcs/preflash
cd preflash
go build -o preflash ./cmd/preflash
```

Keep the checkout: the binary finds `runtime/` beside its source. Set `PREFLASH_ROOT` to the checkout if you move it.

## Run a project

```sh
preflash run [--project dir] [--board file] [--headless]
```

The project needs ESP-IDF 5.x and, if it draws anything, LVGL 9. It also needs a `sdkconfig` and its `managed_components` folder, so run `idf.py reconfigure` once with the real ESP-IDF. Preflash reads both and never writes inside the project.

`--project` defaults to the current folder. Each checkout gets its own folders, so two copies of one project keep separate saved state:

- `~/.preflash/build/<name>-<hash>` holds the build.
- `~/.preflash/state/<name>-<hash>` holds what firmware saves, such as NVS. Delete it for a factory-fresh device.

`--headless` runs with no window and without starting SDL at all.

The firmware's serial console is your terminal. Lines starting `@sim ` go to the simulator instead of the firmware:

| Line | What it does |
| --- | --- |
| `@sim touch x y [ms]` | Presses the panel, in the panel's own coordinates |
| `@sim shot file.bmp` | Saves what the panel shows |
| `@sim brightness` | Prints the backlight level, and the enable pin if the board has one |
| `@sim audio` | Prints how many sample frames firmware has written to the codec |
| `@sim quit` | Stops the simulator |

## Board files

A board comes from `--board`, then from `board:` in the project's `.preflashrc`, then the default, `ESP32-S3-Touch-LCD-4B`. Each can be a stock board's name or a YAML file. A path in `.preflashrc` is relative to the project.

A board file can extend a stock board and describe your own hardware changes:

```yaml
name: my-modded-board
extends: ESP32-S3-Touch-LCD-4B

changes:
  backlight:
    enable: gpio43
```

Or it can stand alone. Every chip it lists needs a model in `runtime/src/chips/`, and the run stops and names any that doesn't have one.

```yaml
name: my-board
display:
  controller: st7701
  width: 480
  height: 480
i2c:
  - {chip: pcf85063, address: 0x51}
backlight:
  pin: gpio4
  active_low: true
audio_codec: es8311
```

## Known limitations

A clean run doesn't prove the hardware will work. Preflash can't show you:

- Real timing, including I2C and task scheduling.
- Stack limits and memory limits, such as internal RAM running out under TLS.
- Tearing, frame rate and real colour on the panel.
- How touch feels.
- A driver bug hidden by a chip model that's wrong. Chip models need checking against the real board.

OTA isn't simulated either. The running slot is always valid, and starting a download fails with a message saying so, so update code stays untested.

A call Preflash doesn't implement fails loudly and names the call. If your firmware stops on one, it needs a stand-in.

## Tests

```sh
go test ./...
cmake -S tests/runtime -B build/tests && cmake --build build/tests --parallel
ctest --test-dir build/tests
tests/integration/build_fixture.sh
tests/integration/display.sh
tests/integration/snoozel.sh
```

`snoozel.sh` needs a checkout of Snoozel beside this one, or `SNOOZEL` pointing at one.
