<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/preflash-mark-on-dark.svg">
    <img src=".github/assets/preflash-mark-on-light.svg" alt="Preflash" width="96" height="96">
  </picture>
</p>

<h1 align="center">Preflash</h1>

<p align="center">
  The Xcode Simulator experience, for ESP-IDF boards.
</p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#terminal-app">Terminal app</a> ·
  <a href="#how-it-compares">How it compares</a> ·
  <a href="#known-limitations">Known limitations</a> ·
  <a href="#development">Development</a>
</p>

---

You change one line of a screen on an ESP32 board, then wait for a build, wait for a flash and walk over to the device to look. Preflash cuts out the board. It builds your unmodified ESP-IDF project for your Mac and runs it in a window the shape of the board's screen, so your own drivers and UI run while ESP-IDF's services and the board's chips are simulated underneath them.

Use it for quick development and debugging, then test on real hardware before you ship.

<p align="center">
  <img src=".github/assets/hero.png" alt="A terminal running preflash run, showing firmware logs and console commands, beside the simulator window showing a pixel-art desert clock reading 11:13 am." width="100%">
</p>

## Why Preflash

- **No firmware changes.** Preflash takes an ESP-IDF project as it is. There's no simulator flag, no fake file and no `#ifdef` to keep in step.
- **Your real drivers run.** Stand-ins replace ESP-IDF's services, and chip models answer on the I2C bus at register level, so your driver code for an RTC, a motion sensor or a power chip runs against something that behaves like the chip.
- **Your own LVGL.** The project's LVGL version builds with the options from its `sdkconfig`, so rendering matches the firmware.
- **Boards are YAML.** A board using known chips needs no tool code. Extend a stock board to describe your own hardware change, or write one from scratch for a custom PCB.
- **It fails loudly.** A call Preflash doesn't implement stops the build or the run and names the call. Firmware never carries on as if it had worked.
- **Scriptable.** Run with no window for CI and coding agents, tap the screen from a script and save what the panel shows.
- **Real network, real saved state.** HTTPS goes out over your Mac's connection, so live APIs work, and what firmware saves survives a restart.

<p align="center">
  <img src=".github/assets/panels.png" alt="Four frames from the simulated panel: a desert clock by day, a settings menu with time and date pickers, a dark night-time wake screen, and the desert clock rotated by 90 degrees." width="100%">
</p>

### What it does today

| Area | Available now |
| --- | --- |
| Display | LVGL 9 through the board's BSP, or `esp_lcd` directly for a project with no BSP, rotation, backlight dimming on the window |
| Input | Mouse as touch, scripted taps |
| Firmware | FreeRTOS tasks, queues and notifications on host threads, `esp_timer`, the task watchdog, restart |
| Storage | NVS, saved per checkout |
| Console | The firmware's own serial console or its own USB serial driver, in your terminal, and a virtual serial port for a companion app |
| Terminal app | A status bar over the serial console, shortcut buttons per project, a board picker and rebuilds that keep the old app on failure |
| Scripting | A control socket and command line for console lines, status, the serial stream, screenshots and touch |
| Chips | PCF85063 RTC, QMI8658 motion sensor, AXP2101 power chip, TCA9554 expander and AXS15231B touch at register level. ES8311, ES7210 and GT911 answer on the bus without a behaviour model |
| Network | Simulated Wi-Fi, SNTP, and real HTTP and HTTPS through `esp_http_client` |
| Audio | The speaker path through `esp_codec_dev`, played on your Mac |
| Boards | ESP32-S3-Touch-LCD-4B and ESP32-S3-Touch-LCD-3.49-V2, plus your own YAML |

### Coming soon

- **Debug views.** Live I2C, GPIO, task, NVS and network traces, forced hardware failures, and sanitizer builds.
- **Hardware checks.** Chip models compared against captures from real boards.

## Install

You need a Mac on Apple Silicon, Go, CMake, Python 3 and SDL2.

```sh
brew install go cmake sdl2
git clone https://github.com/edcs/preflash
cd preflash
go build -o preflash ./cmd/preflash
export PATH="$PWD:$PATH"
```

Keep the checkout where it is, because the binary finds `runtime/` by the path it was built from. If you move the checkout, rebuild or set `PREFLASH_ROOT` to its new path. There are no packaged releases yet.

## Quick start

Your project needs ESP-IDF 5.x and, if it draws anything, LVGL 9. Run ESP-IDF's own setup once, for the `sdkconfig` and `managed_components` Preflash reads:

```sh
cd my-project
idf.py reconfigure
```

Then run it:

```sh
preflash run --board ESP32-S3-Touch-LCD-4B
```

The window opens with your firmware running, and your terminal becomes its serial console. Click the window to touch the panel. Preflash reads your project and doesn't change it. The one exception is a file you ask for, such as a `@sim shot` saved to a path inside it.

```sh
preflash run [--project dir] [--board file] [--headless]
```

`--project` defaults to the current folder. Each checkout gets its own folders, so two copies of one project keep separate saved state:

- `~/.preflash/build/<name>-<hash>` holds the build.
- `~/.preflash/state/<name>-<hash>` holds what firmware saves, such as NVS. Delete it for a factory-fresh device.

### Drive it from a script

A running app listens on a private Unix socket, and the `preflash` command talks to it. Run these from the project, or pass `--project dir` to any of them:

| Command | What it does |
| --- | --- |
| `preflash send <line...>` | Sends a line to the firmware's console and returns without waiting for it to run. Put `--project` before the line |
| `preflash status [--json]` | Shows the project, board, pid, uptime, backlight, rotation, frames, fps, Wi-Fi state and serial port |
| `preflash serial` | Prints the serial stream, starting with what this boot has kept (up to 256 KiB), and follows it through restarts until the app stops |
| `preflash port` | Prints the virtual serial port's path |
| `preflash shot <file.png\|file.bmp>` | Saves what the panel shows |
| `preflash touch x y [ms]` | Presses the panel, in the panel's own coordinates, and returns after the release |
| `preflash quit` | Stops the app and returns once it has gone |

With no app running, each of these says so and exits 1. `preflash run` works as before, with the socket running underneath it, so you can drive a windowed run from a second terminal.

```sh
preflash run --headless < /dev/null &
app=$!
until preflash status > /dev/null 2>&1; do
  kill -0 "$app" 2> /dev/null || exit 1  # the build failed
  sleep 0.2
done
sleep 2  # status answers before the firmware has booted
preflash touch 240 300
sleep 0.5  # touch returns before the firmware has redrawn
preflash shot after-tap.png
preflash quit
```

The first run includes the build, so that loop waits for it. For a boot you can see, use a line your firmware prints in `preflash serial` in place of the `sleep`s. Only one app can run per checkout: a second one stops and says which project and pid hold the socket.

Stdout and stderr are one stream, as a USB serial port is, so `preflash run` prints both to stdout. A client or terminal that stops reading loses its oldest output and hears how much it lost, and the firmware never waits for it. Lines from stdin and `preflash send` join one queue. A console command that never returns holds up the lines behind it, but `status`, `touch`, `shot` and `quit` still work. Quit lets queued lines run first, and stops waiting once a line has gone a second without finishing. The queue holds 1 MiB: past that, `preflash send` fails and a line from stdin is dropped with a message.

Stdin takes the same console lines. Lines starting `@sim ` go to the simulator, and everything else goes to the firmware:

| Line | What it does |
| --- | --- |
| `@sim touch x y [ms]` | Presses the panel, in the panel's own coordinates |
| `@sim shot file.bmp` | Saves what the panel shows |
| `@sim brightness` | Prints the backlight level, and the enable pin if the board has one |
| `@sim audio` | Prints how many sample frames firmware has written to the codec |
| `@sim quit` | Stops the simulator |

`--headless` runs with no window and without starting SDL at all, so it works in CI or on a locked Mac. A headless run ends on `@sim quit` or `preflash quit`, not at the end of stdin.

To boot firmware at a chosen moment, set `PREFLASH_RTC_START=2026-11-01T22:00:00Z`. The simulated RTC starts there and runs on, and it keeps the firmware's time across a restart.

### Connect a companion app

`preflash port` prints a path for your app to open as it would a board's serial port, such as `~/Library/Caches/preflash/<hash>.tty`. The path is a link to a pseudo-terminal, and it stays the same from one run to the next. The port carries the serial stream, so your app reads what `preflash serial` shows, and each line it writes joins the console queue, ending in `\n`, `\r` or `\r\n`. Preflash doesn't read your protocol.

If your firmware installs the USB serial driver, it owns the wire. Everything your app writes to the port reaches `usb_serial_jtag_read_bytes` byte for byte, `@sim ` lines and line endings included. Lines from stdin and `preflash send` reach it too, each with a `\n`, except `@sim ` lines, which stay with Preflash. Until the driver is in, these lines wait in the queue. Firmware reads serial input through that call, because stdin stays with Preflash.

The port starts raw at 115200 with no echo. Your app keeps its connection through `esp_restart`, and sees the port hang up on quit, on a crash and on a rebuild, as it would when you unplug a board. Open the path again to reconnect.

To put a link where your app's developer setting looks, add one to `.preflashrc`. Preflash removes both links when the app stops, and refuses to replace a file that isn't a link. Only `kill -9` leaves them behind, and the next run replaces them.

```yaml
serial_link: sim/my-board.tty
```

A relative path starts at the project, and `~/` at your home folder. The link's folder must exist already. If a link fails, the app prints why and runs on, and `preflash port` gives the device's own path if there's no other.

The port doesn't appear as a USB device, so an app that finds boards by USB identity needs that developer setting. Firmware never waits for the port. It holds about 65 KiB, and past that it loses the oldest output, with no notice in the stream, because a notice would land in your protocol. An app that opens the port late reads what it's holding first, so expect old lines ahead of live ones, with a gap between them.

### Describe your board

A board comes from `--board`, then from `board:` in the project's `.preflashrc`, then the default, `ESP32-S3-Touch-LCD-4B`. Each can be a stock board's name or a YAML file. A path in `.preflashrc` is relative to the project.

A board file can extend a stock board with your own hardware changes:

```yaml
name: my-modded-board
extends: ESP32-S3-Touch-LCD-4B

changes:
  backlight:
    enable: gpio43
```

Or it can stand alone, for a custom PCB whose project uses the same BSP calls or drives `esp_lcd` itself. Every chip it lists needs a model in `runtime/src/chips/`, and the run stops and names any that doesn't have one.

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

A few keys cover wiring that differs between boards. The stock `ESP32-S3-Touch-LCD-3.49-V2` uses all but the last:

| Key | What it does |
| --- | --- |
| `display.byte_order` | How `esp_lcd_panel_draw_bitmap` reads RGB565: `le`, the default, or `be` |
| `i2c[].bus` | The `i2c_port` a chip sits on, 0 by default. Firmware on the other bus gets no answer |
| `backlight.enable` | What cuts the backlight when low: a pin, `gpio43`, or a TCA9554 bit, `exio1` |
| `power_amp` | The expander bit behind `BSP_POWER_AMP_IO`, such as `exio3` |

A channel set up with `ledc_channel_config` on the backlight pin sets the brightness, so firmware dims the panel through LEDC as it would on the board. On a stock 3.49 V2 the window shows the bare portrait panel, so firmware that draws in landscape appears on its side. `esp_lcd_new_panel_axs15231b` is the one panel driver Preflash has, and any other stops the build at the missing call.

## Terminal app

Run `preflash` with no arguments in your project, or `preflash tui`, or pass `--project dir`. `--board` and `--headless` work as they do for `preflash run`.

The launch screen lists the board files in your project and the stock boards, with each board's chips and changes. Enter builds and runs. A project whose `.preflashrc` names a board skips this screen and goes straight to running.

The running screen has a toolbar, the serial console, your shortcuts and an input line. Press a number to run a shortcut while the input is empty, or click its button. The wheel and PgUp/PgDn scroll the console, and up and down walk your history.

| Key | What it does |
| --- | --- |
| `^R` | Rebuilds and restarts, with the build's output in the console. If the build fails, the old app keeps running |
| `^B` | Picks another board |
| `^L` | Clears the console |
| `^C` | Quits and stops the app |

The app runs as a child with its stdin closed and its stdout and stderr in `app.log` in the build folder. The screen reads the serial stream over the control socket, so every `preflash` command still works from a second terminal. If the app stops or crashes, the toolbar says how and the console keeps its last output. The screen takes the mouse, so selecting text in the console needs your terminal's selection modifier.

The toolbar shows the serial port last, so a narrow window cuts it off. `preflash port` prints it in full.

A `.preflashrc` in the project sets up the rest:

```yaml
board: sim/my-board.yaml
serial_link: sim/my-board.tty

console:
  prompt: "my-app>"

shortcuts:
  - label: Status
    send: status
  - label: Evening
    send:
      - fake {today}T22:00:00Z
      - speed 600
  - label: Timer
    send: timer {seconds}
    ask:
      seconds: 120

status:
  - name: screen
    pattern: 'screen=(\w+)'
  - name: backlight
  - name: wifi
```

`send` is one line or a list. `ask` prompts for each value first, with the number given as its default, and `{today}` fills in the local date. A status field shows the last value its pattern found in the serial stream: the first capture group, or the whole match with no group. Backlight, rotation, Wi-Fi and the serial port (`port`) are built in. Naming one with no pattern places it in the toolbar, and naming one with a pattern replaces it. An unknown key stops with an error naming the line, for `preflash run` too.

## How it compares

- **Flashing a real board** is the only test that counts before you ship, and Preflash doesn't replace it. It saves the round trips while you work.
- **Wokwi and QEMU** emulate the chip and run a flashed image, so they get closer to the hardware. Preflash builds native code, so you can step through firmware in your Mac's debugger.
- **ESP-IDF's `linux` target** runs a subset of components natively. It has no `esp_lcd`, I2C, GPIO or Wi-Fi drivers, so a project with a screen and chips doesn't run on it unchanged.
- **LVGL's PC simulators** preview a UI, and you split that UI out of the firmware and write a host `main` yourself. Preflash runs the whole app, including its tasks, storage and drivers.
- **Esprite** is the closest relative. It simulates Arduino projects, where Preflash stands in for ESP-IDF.

## Known limitations

A clean run doesn't prove the hardware will work. Preflash can't show you:

- Real timing, including I2C and task scheduling.
- Stack limits and memory limits, such as internal RAM running out under TLS.
- Tearing, frame rate and real colour on the panel.
- How touch feels.
- A driver bug hidden by a chip model that's wrong. Chip models need checking against the real board.

OTA isn't simulated either. The running slot is always valid, and starting a download fails with a message saying so, so update code stays untested.

With `CONFIG_ESP_TASK_WDT_INIT` set, the task watchdog watches subscribed tasks and stops the process on a panic, where a device would restart. It doesn't watch idle tasks, so an idle task starved of CPU won't trip it. A task can delete itself with `vTaskDelete`, but deleting another task stops the process.

If your firmware stops on a call Preflash doesn't implement, that call needs a stand-in.

## Development

### Test

```sh
make test
```

That runs the Go tests, the runtime suite and the integration scripts. `tests/integration/tui.sh` runs the terminal app in a pseudo-terminal and reads its screen through a virtual terminal, including a mouse click on a shortcut. `tests/integration/lcd.sh` runs a project with no BSP from `tests/fixtures/lcd_project`. `tests/integration/snoozel.sh` runs a real project end to end and needs a checkout of it at `../snoozel`, or `SNOOZEL` pointing at one, with a current `sdkconfig`. `tests/integration/nyx.sh` does the same on the 3.49 V2 with `NYX` pointing at a checkout, and skips without it. Both are private projects, so without Snoozel run the steps before that script in the `Makefile`.

### Layout

```text
cmd/preflash/     command line
internal/app/     supervising a build and its app for the terminal app
internal/board/   board files: extends, changes and validation
internal/client/  talking to a running app over its socket
internal/project/ finding, building and running a project
internal/rc/      reading .preflashrc
internal/tui/     the terminal app's screens
boards/           stock boards
runtime/idf/      the CMake layer that stands in for IDF_PATH
runtime/include/  ESP-IDF headers the firmware compiles against
runtime/src/      stand-ins, chip models, the window and the host
tests/            runtime suite, fixtures, integration scripts and terminal tests
.github/assets/   README images, redrawn by render.sh
```

### Contributing

Commits use conventional commit titles. A new chip means a new model in `runtime/src/chips/`, and a new ESP-IDF call means a stand-in that follows the device's return codes, or that stops and names the call.
