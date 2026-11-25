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
  <a href="#install">Install</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#terminal-app">Terminal app</a> ·
  <a href="#browser">Browser</a> ·
  <a href="#drive-it-from-a-script">Scripting</a> ·
  <a href="#debug-firmware">Debugging</a> ·
  <a href="#how-it-compares">How it compares</a> ·
  <a href="#known-limitations">Known limitations</a> ·
  <a href="#development">Development</a>
</p>

---

You change one line of a screen on an ESP32 board, then wait for a build, wait for a flash and walk over to the device to look. Preflash cuts out the board. It builds your unmodified ESP-IDF project for your Mac and runs it in a window the shape of the board's screen, so your own drivers and UI run while ESP-IDF's services and the board's chips are simulated underneath them.

Use it for quick development and debugging, then test on real hardware before you ship.

<p align="center">
  <img src=".github/assets/hero.png" alt="The Preflash terminal app running the bundled demo, with boot logs and a status command in its console, beside the demo's panel on the 480 by 480 4B and on the tall, narrow 3.49 V2, each showing a clock at 09:41 above tiles for time, motion, power, sound, network, update, settings and faults." width="100%">
</p>

Every capture here comes from [`examples/demo`](examples/demo), an ordinary ESP-IDF project in this repo, so you can run what you see. Its [README](examples/demo/README.md) maps each feature to the source that uses it.

## Why Preflash

- **No firmware changes.** Preflash takes an ESP-IDF project as it is. There's no simulator flag, no fake file and no `#ifdef` to keep in step.
- **Your real drivers run.** Stand-ins replace ESP-IDF's services, and chip models answer on the I2C bus at register level, so your driver code for an RTC, a motion sensor or a power chip runs against something that behaves like the chip.
- **Your own LVGL.** The project's LVGL version builds with the options from its `sdkconfig`, so rendering matches the firmware.
- **Boards are YAML.** A board using known chips needs no tool code. Extend a stock board to describe your own hardware change, or write one from scratch for a custom PCB.
- **It fails loudly.** A call Preflash doesn't implement stops the build or the run and names the call. Firmware never carries on as if it had worked.
- **Scriptable.** Run with no window for CI and coding agents, tap the screen from a script and save what the panel shows.
- **Real network, real saved state.** HTTPS goes out over your Mac's connection, so live APIs work, and what firmware saves survives a restart.

<p align="center">
  <img src=".github/assets/panels.png" alt="Five frames from the demo: on the 4B, the Faults page with the RTC not answering, the home page turned a quarter, and the Power page with the battery forced to 15 percent; on the 3.49 V2, the Faults and Network pages." width="100%">
</p>

### What it does

| Area | What you get |
| --- | --- |
| Display | LVGL 9 through the board's BSP, or `esp_lcd` directly for a project with no BSP, rotation, backlight dimming on the window |
| Input | Mouse as touch, scripted taps |
| Firmware | FreeRTOS tasks, queues and notifications on host threads, `esp_timer`, the task watchdog, restart |
| Storage | NVS, saved per checkout. Flash partitions registered at run time are kept in the board's state folder |
| Network | Simulated Wi-Fi, SNTP, and real HTTP and HTTPS through `esp_http_client` |
| Audio | The speaker path through `esp_codec_dev`, played on your Mac |
| Chips | PCF85063 RTC, QMI8658 motion sensor, AXP2101 power chip, TCA9554 expander and AXS15231B touch at register level. ES8311, ES7210 and GT911 answer on the bus without a behaviour model |
| [Terminal app](#terminal-app) | The firmware's serial console under a status bar, shortcut buttons per project, a board picker, rebuilds that keep the old app on failure, debug views and a hardware panel |
| [Scripting](#drive-it-from-a-script) | A control socket and command line for console lines, status, the serial stream, traces, hardware changes, screenshots and touch |
| [Debugging](#debug-firmware) | Live I2C, GPIO, task, NVS and network traces you can filter and save, forced hardware failures, crash reports tied to source, and address and thread sanitizer builds |
| [Companion apps](#connect-a-companion-app) | A virtual serial port, which also reaches firmware's own USB serial driver |
| [Boards](#describe-your-board) | ESP32-S3-Touch-LCD-4B and ESP32-S3-Touch-LCD-3.49-V2, plus your own YAML |
| [Hardware checks](#check-the-models-against-a-board) | Capture firmware and `preflash check`, which replays a capture against the chip models. No chip has yet been compared with a real board |

## Install

Preflash runs on Apple Silicon Macs. It builds your firmware with CMake and Python 3 and draws the window with SDL2, so install those first:

```sh
brew install cmake sdl2
```

Then install Preflash:

```sh
curl -fsSL https://raw.githubusercontent.com/edcs/preflash/main/install.sh | sh
```

The script downloads the latest release from GitHub and checks its SHA-256. It puts `preflash` in `~/.local/bin` and the runtime it builds firmware against in `~/.local/share/preflash`. If `~/.local/bin` isn't on your `PATH`, add it to your shell's profile:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Check it worked:

```sh
preflash version
```

### Install options

Set these on the `sh` side of the pipe:

| Variable | Default | Sets |
| --- | --- | --- |
| `PREFLASH_VERSION` | the latest release | which release to install, such as `0.1.0` |
| `PREFLASH_PREFIX` | `~/.local` | where `bin/preflash` and `share/preflash` go |

```sh
curl -fsSL https://raw.githubusercontent.com/edcs/preflash/main/install.sh | PREFLASH_VERSION=0.1.0 sh
```

### Upgrade or uninstall

Run the install command again to upgrade. It replaces the binary and runtime together. Your builds, each project's saved state, such as NVS, and downloaded components stay in `~/.preflash`.

To uninstall, delete what the script installed, and `~/.preflash` too if you want those gone:

```sh
rm -rf ~/.local/bin/preflash ~/.local/share/preflash
rm -rf ~/.preflash
```

### Get the demo

The [quick start](#quick-start) runs the demo app in this repo, so clone it:

```sh
git clone https://github.com/edcs/preflash
cd preflash
```

### Build from source

Building needs Go as well:

```sh
brew install go
go build -o preflash ./cmd/preflash
export PATH="$PWD:$PATH"
```

A binary built this way finds `runtime/` in the checkout it came from. If you move the checkout, rebuild or set `PREFLASH_ROOT` to its new path.

## Quick start

Start with the demo. It needs no ESP-IDF, because the first build downloads the LVGL that its `dependencies.lock` pins, into `~/.preflash/components`:

```sh
preflash --project examples/demo
```

The window opens with the firmware running, and your terminal shows the [terminal app](#terminal-app). Click a tile in the window to touch the panel, and press `1` in the terminal to run the demo's Status shortcut. From a second terminal, take the RTC off the bus and put it back, and watch the Faults page change:

```sh
preflash send --project examples/demo page faults
preflash hardware --project examples/demo i2c pcf85063
preflash hardware --project examples/demo i2c healthy
```

### Run your own project

Your project needs ESP-IDF 5.x and, if it draws anything, LVGL 9. Run ESP-IDF's own setup once, for the `sdkconfig` and `managed_components` Preflash reads:

```sh
cd my-project
idf.py reconfigure
```

A project that commits `sdkconfig` and `dependencies.lock`, as the demo does, skips that step. With no `managed_components/` folder, Preflash downloads what the lock pins and checks each component's hash.

Run `preflash` for the terminal app, or `preflash run` to make your terminal the plain serial console:

```sh
preflash run [--project dir] [--board file] [--headless] [--sanitize address|thread]
```

`--project` defaults to the current folder. Preflash reads your project and doesn't change it. The one exception is a file you ask for, such as a `@sim shot` saved to a path inside it. Each checkout gets its own folders, so two copies of one project keep separate saved state:

- `~/.preflash/build/<name>-<hash>` holds the build.
- `~/.preflash/state/<name>-<hash>` holds what firmware saves, such as NVS. Delete it for a factory-fresh device.

### The window, buttons and power

The window is a process of its own, `preflash-window`, built beside the app. It stays where it is while the firmware restarts, whether by `esp_restart`, RST, a power change or a crash, and keeps the last frame up until the new boot draws. Power-off and download mode run no firmware, so they black the panel out, and the boot after them starts from black as the board does. Under `preflash run`, closing the window quits the app and quitting the app closes the window. The terminal app keeps the window up while it rebuilds or restarts the app, and closes it when you quit the terminal app. If the window process dies, the app starts a new one. The window writes its errors to a log beside the control socket, `~/Library/Caches/preflash/<hash>.window.log`. If no window stays open, the app stops with `preflash: couldn't open the window: ` and the log's last line. `--headless` runs with no window and without starting SDL at all, so it works in CI or on a locked Mac, and buttons, rotation and power still work from the command line and the socket.

The title bar shows the project's name. Under it, a strip holds the board's buttons on the left and the rotate button on the right, and a status bar under the panel shows the board's full name and how far it's turned. A narrow panel, such as the 3.49 V2's, sits centred on black in a window at least 250 points wide. Hovering a button shows what it's wired to in the status bar.

#### Buttons

A board button goes down when you press the mouse on it and comes up when you let go, or when the pointer leaves it. ⌥-click a button to latch it: it stays down, through the pointer leaving and the window losing focus, until you click it again, and its hover text says "latched". A held button shows filled, whether you hold it or a script does. The mouse and scripts hold buttons separately: a button stays down until everyone holding it lets go, so a script's `tap` doesn't release a button you're holding. A rebuild in the terminal app starts a new app, which lets go of what scripts held and keeps what the mouse holds.

BOOT, and PWR on a board without a power chip, move the pin the real button moves, so firmware reads them as it would on the device and Preflash adds nothing. Firmware that reads a key by interrupt sees the press too: `gpio_install_isr_service`, `gpio_isr_handler_add`, `gpio_intr_enable` and `gpio_set_intr_type` work with every edge and level type, and the handler runs on a thread that stands in for the ISR. A level type fires once as the pin enters its level, not for as long as it stays there, and also once when it's armed (`gpio_intr_enable` or `gpio_isr_handler_add`) while the pin is already at its level, so a key held through a restart still reaches the new boot. `gpio_isr_register` stops the firmware as unsupported.

A button held while the firmware restarts is still held when it boots again, so holding BOOT through a restart reaches the new boot. A script's `tap` that restarts the app, such as `tap PWR 1000` while the board is off, answers as done once the restart is under way. The hold carries into the new run until the tap's time is up, so a second tap inside that time does nothing.

The [demo](examples/demo) shows firmware reading these keys. Its `buttons` console command prints BOOT's level and how many presses its pin interrupt has counted, and holding PWR for 1.5 seconds, the AXP2101's long-press time, makes the firmware switch the board off.

#### Reset and download mode

Letting go of RST resets the chip as its EN pin does. The firmware boots again with NVS and the rest of the board's state kept, and `esp_reset_reason` answers `ESP_RST_POWERON`, as an EN reset reads on an ESP32-S3. The window, the socket and the serial port see the reset as they see `esp_restart`. Holding BOOT while you let go of RST enters download mode, as on the chip. In the window, ⌥-click BOOT to latch it, click RST, then click BOOT to let it go. In download mode no firmware runs, the panel goes dark, the serial stream prints the ROM's `waiting for download` line, and the status bar says "download mode". The serial port hangs up in download mode, as it would if you unplugged the board, and comes back when the firmware boots. The next RST with BOOT up boots normally. `preflash status` shows `power` as `on`, `off`, or `download` in download mode.

#### Power

The board can be off with the window open. Off, no firmware runs, the panel is dark, the status bar says "off", and the serial port hangs up as it does in download mode, while the window, the socket and the terminal app stay up. A press of PWR while off boots the firmware from cold, with NVS kept and `esp_reset_reason` answering `ESP_RST_POWERON`, and, on a power-key board, the key still down for the new boot to read until you let go. An AXP2101 starts with no press recorded, so the new boot's chip reports nothing of the press that woke it. The board goes off when:

- firmware sets the AXP2101's soft power-off bit,
- PWR on the AXP2101 is held past its OFFLEVEL, 6 seconds unless firmware sets register 0x27 otherwise. Past IRQLEVEL, 1.5 seconds, the chip raises its long-press interrupt first, and a shorter press raises the short-press one when it ends, as register 0x49 shows. A PWRON interrupt is raised only if firmware enabled it in register 0x41,
- or a board with a power key, such as the 3.49 V2, loses power: on battery, its circuit keeps it on only while PWR is down or the firmware drives the key's hold output high.

On USB, a power key board stays on whatever its hold does, as the real board does. On battery it follows its circuit. Launching Preflash on battery starts it off, waiting for PWR, and plugging USB back in turns it on. If the firmware hasn't driven the hold high by the time PWR comes up, the board goes off again, which is how a late latch shows. The hold reads as low until firmware makes it an output and drives it high, including at boot. The TCA9554 keeps its pins through RST and `esp_restart`, as the chip keeps its own power, so a hold on the expander survives a reset on battery. A hold on a GPIO doesn't. USB is plugged in by default, so a normal run starts on. The RTC keeps time while the board is off, as it runs from its own cell. `preflash hardware usb unplugged` with the hold low and PWR up switches a power key board off.

#### Turning

The rotate button and ⌘→ turn the board a quarter clockwise, and ⌘← turns it back. The window turns with it: it swaps between portrait and landscape and draws the panel's picture turned the same way, so firmware that ignores the motion sensor shows sideways. A click lands on the panel pixel under the pointer however it's turned, and the strip and status bar stay upright. This is the `rotation` [hardware setting](#break-the-hardware), so it lasts through restarts, and boards with no motion sensor can turn too. `shot`, `touch` and the frames stream keep the panel's own orientation and coordinates.

#### Starting time

To boot firmware at a chosen moment, set `PREFLASH_RTC_START=2026-11-01T22:00:00Z`. The simulated RTC starts there and runs on, and it keeps the firmware's time across a restart.

## Terminal app

Run `preflash` with no arguments in your project, or `preflash tui`, or pass `--project dir`. `--board`, `--headless` and `--sanitize` work as they do for `preflash run`.

The launch screen lists the board files in your project and the stock boards, with each board's chips and changes. Enter builds and runs, and `s` moves through a plain build and each sanitizer. A project whose `.preflashrc` names a board skips this screen and goes straight to running, as the demo does.

The running screen has a toolbar, the serial console, your shortcuts and an input line. Press a number to run a shortcut while the input is empty, or click its button. The wheel and PgUp/PgDn scroll the console, and up and down walk your history.

| Key | What it does |
| --- | --- |
| `^R` | Rebuilds and restarts, with the build's output in the console, in the same window. If the build fails, the old app keeps running |
| `^D` | Opens the debug views |
| `^H` | Opens the hardware panel |
| `^B` | Picks another board |
| `^L` | Clears the console |
| `^C` | Quits and stops the app |

<p align="center">
  <img src=".github/assets/terminal.png" alt="Two terminal app screens running the demo. Left, the I2C debug view, with reads of the PCF85063 in amber as NACK (injected). Right, the crash screen, with a null pointer read in mishaps::read_sensor at main/mishaps.cpp line 15 and its call stack." width="100%">
</p>

The debug views have a tab each for the console, I2C, GPIO, tasks, NVS and network. Tab moves between them, `/` filters, space pauses, `s` saves the trace to the state folder and esc returns to the console. Tasks and NVS also show a table of what's there now, and a task's stack is the size firmware declared, not what it uses. Rows a forced failure caused show in amber, such as `NACK (injected)`.

The hardware panel changes what [`preflash hardware`](#break-the-hardware) changes, with up and down to choose and left and right to change. It says whether a change applied now or waits for the next boot, and `^R` restarts from the panel.

When firmware [crashes](#when-firmware-crashes), the screen shows the console, the report and these keys:

| Key | What it does |
| --- | --- |
| `g` | Opens `lldb` attached to the frozen app, in this terminal, and returns here when you quit it |
| `e` | Opens `$EDITOR` at the crash's line |
| `y` | Copies the report |
| `^R` | Rebuilds and restarts. If the build fails, the crashed app stays frozen |
| `^C` | Quits and stops the app |

The app runs as a child with its stdin closed and its stdout and stderr in `app.log` in the build folder. The screen reads the serial stream over the control socket, so every `preflash` command still works from a second terminal. If the app stops or crashes, the toolbar says how and the console keeps its last output. The screen takes the mouse, so selecting text in the console needs your terminal's selection modifier. The toolbar shows the serial port last, so a narrow window cuts it off, and `preflash port` prints it in full.

### Set up a project

A `.preflashrc` in the project sets the board, the shortcuts and the status fields. This comes from the demo's [`.preflashrc`](examples/demo/.preflashrc):

```yaml
board: sim/demo-4b.yaml

console:
  prompt: "demo>"

shortcuts:
  - label: Status
    send: status
  - label: Morning
    send:
      - time {today}T07:30:00Z
      - page home
  - label: Brightness
    send: brightness {percent}
    ask:
      percent: 60

status:
  - name: screen
    pattern: 'screen=(\w+)'
  - name: backlight
  - name: rotation
  - name: wifi
```

`send` is one line or a list. `ask` prompts for each value first, with the number given as its default, and `{today}` fills in the local date. A status field shows the last value its pattern found in the serial stream: the first capture group, or the whole match with no group. Backlight, rotation, Wi-Fi and the serial port (`port`) are built in. Naming one with no pattern places it in the toolbar, and naming one with a pattern replaces it. An unknown key stops with an error naming the line, for `preflash run` too.

A project that builds differently per board can name an `sdkconfig` for each one, and any board it doesn't list uses the project's `sdkconfig`. The demo builds different sources for each board:

```yaml
sdkconfig:
  ESP32-S3-Touch-LCD-3.49-V2: sdkconfig.349
```

## Browser

`preflash web` runs the terminal app and the panel in a browser tab, so you can use Preflash from a machine with no window of its own, or from an iPad.

```sh
preflash web
```

It prints a URL, `http://localhost:7480` unless you pass `--port`. The page shows the panel on one side and the terminal app on the other, and the app runs headless, so there is no separate window. Touch or drag on the panel to tap it, and hold a button in the strip below to hold that board button. Both go through the app's control socket, as [`preflash hardware`](#break-the-hardware) does. `--board`, `--sanitize` and `--project` work as they do for `preflash tui`.

The server listens on `127.0.0.1` only. To reach it from another machine, forward the port. [Nearhost](https://nearhost.dev) can do that for you. The plain way is an SSH tunnel:

```sh
ssh -L 7480:localhost:7480 <mac>
```

Then open `http://localhost:7480` on the machine you're sitting at. A page is refused unless its address is `localhost`, `127.0.0.1` or `[::1]`, on any port, so a forward onto another port works. For a name of your own, pass `--allow-host name`, once for each name. A refused request gets a page that names the host and the flag.

The toolbar has a speaker button, off in every tab. One tap turns sound on, and the device's sound plays in that tab. A tab streams nothing until you tap, and turning sound off stops the data. Safari wants that tap again after a reload. The sound travels as Opus, at about 12 KB/s for stereo, so a cellular link copes. The Mac stays silent during `web`, because the sound goes to the browser and not to the Mac's speakers.

The key row under the terminal sends the keys a touch screen lacks: Esc, Tab, `^C`, `^R`, the arrows and `/`. `^C` typed in the page ends only the terminal app, and the page offers to start it again. `^C` to `preflash web` itself stops the terminal app and the firmware, and leaves nothing running.

### Check it on an iPad

The tests cover the server and the page's protocol, but not Safari on a touch screen. After a change to the page, check these on a real iPad, reaching the Mac through a forwarded port:

- Drag on the panel, and see the drag land where your finger is.
- Hold a button, then lift your finger, and see the button let go.
- Use the key row, including a sideways swipe on a narrow screen that sends no key.
- Tap `^B`, `^D` and `^H` in the terminal app's footer, and see the board picker, the debug views and the hardware panel open. The on-screen keyboard has no Control key, so these taps are the only way to reach them, and they work only if xterm.js passes them to the app as mouse reports.
- Tap the speaker button, press `2` for Beep in the terminal app, and hear the beep arrive promptly, on cellular as well as on Wi-Fi.
- Play a project that streams music for several minutes, and hear no gaps.
- Let the iPad sleep, wake it, and hear the sound come back once the page reconnects and you tap the speaker again if Safari asks.
- Turn sound off, and see the data stop in the bandwidth.
- Turn the iPad upright, and see the panel and terminal stack to fit.
- Let the iPad sleep, wake it, and see the terminal redraw with its colours, with nothing left held down.

## Drive it from a script

A running app listens on a private Unix socket, and the `preflash` command talks to it, whether the app has a window or not. Run these from the project, or pass `--project dir` to any of them:

| Command | What it does |
| --- | --- |
| `preflash send <line...>` | Sends a line to the firmware's console and returns without waiting for it to run. Put `--project` before the line |
| `preflash status [--json]` | Shows the project, board, pid, uptime, backlight, rotation (LVGL's), board rotation (the way the board is turned), frames, fps, Wi-Fi state, power (`on`, `off` or `download`), the board's buttons and which are down, and serial port |
| `preflash serial` | Prints the serial stream, starting with what this boot has kept (up to 256 KiB), and follows it through restarts until the app stops |
| `preflash port` | Prints the virtual serial port's path |
| `preflash shot <file.png\|file.bmp>` | Saves what the panel shows |
| `preflash touch x y [ms]` | Presses the panel, in the panel's own coordinates, and returns after the release |
| `preflash trace [--stream x] [--json]` | Prints what firmware does to the hardware, starting with what the app has kept, and follows it through restarts. `x` is `i2c`, `gpio`, `task`, `nvs`, `net` or `button` |
| `preflash trace --save file` | Saves what the app has kept as JSON lines |
| `preflash hardware [name value] [--json]` | Shows the board's physical state and forced failures, or changes one |
| `preflash hardware button <name> press\|release\|tap [ms]` | Presses one of the board's buttons, such as `BOOT`, lets go of it, or taps it: presses, waits `ms` (100 unless given) and lets go, so `tap 3000` is a three-second hold. `press` holds it until a `release`, and a `tap` of a button scripts already hold waits its time and leaves the button down. The trace's `button` stream shows each press beside what the firmware did |
| `preflash quit` | Stops the app and returns once it has gone |
| `preflash crash [--json]` | Prints the last crash report |
| `preflash bench [--board file]` | Times a clean build, a rebuild, the boot, the frame rate, the socket and the serial stream on a copy of the project |
| `preflash check <capture.jsonl> --board name` | Compares a capture from a real board with the chip models |

`preflash crash`, `preflash bench` and `preflash check` need no running app. With no app running, the others say so and exit 1.

A client that talks to the socket directly can do two things the command line can't. It can send `pointer` with `x`, `y` and `down`, in the panel's own coordinates, for each press, move and release of a drag. And it can `subscribe` to `frames` to hear when each frame finishes: the event carries the frame's number, and the `frame` op then fetches its pixels. A client that falls behind skips frames instead of queuing them.

This script opens the demo's Power page and saves it. The numbers are the middle of the Power tile, from the layout in [`page_home.cpp`](examples/demo/main/ui/page_home.cpp):

```sh
cd examples/demo
preflash run --headless < /dev/null &
app=$!
until preflash status > /dev/null 2>&1; do
  kill -0 "$app" 2> /dev/null || exit 1  # the build failed
  sleep 0.2
done
sleep 2  # status answers before the firmware has booted
preflash touch 297 288
sleep 0.5  # touch returns before the firmware has redrawn
preflash shot /tmp/power.png
preflash quit
```

The first run includes the build, so that loop waits for it. For a boot you can see, wait in `preflash serial` for a line your firmware prints, such as the demo's `demo: ready on`, in place of the `sleep`s. Only one app can run per checkout: a second one stops and says which project and pid hold the socket. A headless run ends on `@sim quit` or `preflash quit`, not at the end of stdin.

Stdout and stderr are one stream, as a USB serial port is, so `preflash run` prints both to stdout. A client or terminal that stops reading loses its oldest output and hears how much it lost, and the firmware never waits for it.

`preflash run` reads console lines from stdin. Lines starting `@sim ` go to the simulator, and everything else goes to the firmware:

| Line | What it does |
| --- | --- |
| `@sim touch x y [ms]` | Presses the panel, in the panel's own coordinates |
| `@sim shot file.bmp` | Saves what the panel shows |
| `@sim brightness` | Prints the backlight level, and the enable pin if the board has one |
| `@sim audio` | Prints how many sample frames firmware has written to the codec |
| `@sim quit` | Stops the simulator |

Lines from stdin and `preflash send` join one queue. A console command that never returns holds up the lines behind it, but `status`, `touch`, `shot` and `quit` still work. Quit lets queued lines run first, and stops waiting once a line has gone a second without finishing. The queue holds 1 MiB: past that, `preflash send` fails and a line from stdin is dropped with a message.

## Debug firmware

### Trace what firmware does

`preflash trace` shows what firmware does to the hardware. The demo's [`monitor.cpp`](examples/demo/main/monitor.cpp) reads each chip on a timer:

```text
$ preflash trace --project examples/demo --stream i2c
00:03.024    i2c   0x51 PCF85063            read         04: 31 10 16 02 01 11 26
00:03.024    i2c   0x6b QMI8658             read         35: 00 00 00 40
00:03.024    i2c   0x34 AXP2101             read         00: 20
```

Tracing starts when a client asks for it and stops when the last client leaves, so it costs a running app one relaxed load until then. The app keeps the newest 8192 records in a ring. A client that stops reading loses its oldest records, and firmware never waits for it. `preflash trace` warns of the gap.

NVS rows show keys and sizes and leave the values out, because a value can be a Wi-Fi password. Network rows show whole URLs, query strings included, so check a saved trace before you share it.

### Break the hardware

`preflash hardware` lists each name, its current setting and its choices. `preflash hardware rotation 90` turns the board a quarter clockwise, the window turns with it, and your driver reads it from the motion sensor. `preflash hardware i2c pcf85063` makes that chip stop answering, and `preflash hardware i2c all` does the same for the whole bus. The demo's [Faults page](examples/demo/main/ui/page_faults.cpp) shows what firmware sees of each one.

| Name | Choices | Applies |
| --- | --- | --- |
| `rotation` | `0`, `90`, `180`, `270` on every board: the window turns, and a QMI8658 reports gravity | Now |
| `usb` | `plugged`, `unplugged`, on a board with PWR or an AXP2101, and firmware reads it through the AXP2101 | Now |
| `battery` | `none`, `100`, `50`, `15`, `0`, likewise | Now |
| `internet` | `real`, `offline` | Now |
| `i2c` | `healthy`, `all`, or a chip on the board | Now |
| `wifi_join` | `succeeds`, `fails` | Now |
| `rtc` | `healthy`, `flat`, which sets the PCF85063's OS bit and loses the time | Next boot |
| `motion` | `healthy`, `gone` | Next boot |

The list also has a `wifi` row, which says that the simulated Wi-Fi uses your Mac's connection, and has no other choice. A board shows only the rows its chips can act on. The 3.49 V2 has `usb` and `battery` because its PWR runs from a battery, but with no ADC or VBUS pin modelled, they only decide whether its power stays on: its firmware can't read them. Preflash refuses to leave the board with no USB and no battery, so set `battery` before you unplug USB.

Everything you set stays in `faults.json` in the state folder, across restarts, rebuilds and new runs, until you set it back. A boot with anything forced prints what it forced. A flat cell comes back on every boot, including after `esp_restart`, which a real cell wouldn't do.

Firmware that writes the AXP2101's soft power-off bit switches the board off, and PWR turns it back on. Forced NACKs happen on the I2C bus, so `esp_io_expander` calls still reach the expander.

### When firmware crashes

Preflash catches a segfault, a bus error, an illegal instruction, an arithmetic fault, a trap and an abort, including `abort()`, a failed `ESP_ERROR_CHECK`, `esp_system_abort`, a watchdog panic and a call Preflash doesn't implement. The failed check prints as it does on a device.

The app saves a report to `crash.json` in the build folder and prints a line on the serial stream. `preflash crash` turns the report into a title, a source line and a call stack, using `dsymutil` and `atos`. The first look at a crash takes a few seconds, while `dsymutil` gathers the debug information. After `preflash send crash null`, which runs the demo's [`mishaps.cpp`](examples/demo/main/mishaps.cpp):

```text
$ preflash crash
Null pointer read in mishaps::read_sensor
main/mishaps.cpp:15
memory error on the console task, SIGSEGV at 0x0

› mishaps::read_sensor      mishaps.cpp:15
  mishaps::null_read        mishaps.cpp:20
  console::crash_command    console.cpp:154
```

With a window, the app stops in place. Firmware's tasks freeze where they were, the window keeps its last frame, and `preflash status`, `shot` and `quit` still work, so you can attach `lldb -p <pid>`. `preflash send` refuses, and ^C or `preflash quit` ends the app. A `--headless` `preflash run` has no frame to keep, so it exits with the signal and a script sees the failure. Under the terminal app, a headless run holds too, so `g` has something to attach to.

Rebuilding replaces the app, so a report older than the build prints addresses in place of names. Only the newest report is kept for each kind of build, and a report stays until the next crash replaces it.

### Run under a sanitizer

`preflash run --sanitize address` finds memory errors and `--sanitize thread` finds data races. Each kind gets its own build folder, so switching costs one build and the plain one stays as it was. Saved state and the socket stay shared.

The first report stops the firmware and counts as a crash, so you read it through `preflash crash` or the crash screen, with the sanitizer's full text at the end. It also goes to the serial stream. Your project's own `ASAN_OPTIONS` and `TSAN_OPTIONS` stay in force, with Preflash's added after them.

A sanitizer build always uses `malloc` and libc's string calls for LVGL, whatever your `sdkconfig` chose, because LVGL's own allocator hides overflows inside its pool. Firmware tasks are real threads, so a race report describes your code running on your Mac, and its timing differs from the chip's.

[`runtime/tsan.supp`](runtime/tsan.supp) lists what the thread sanitizer ignores, with the reason beside each entry. Preflash's own code runs with no suppressions.

## Connect a companion app

`preflash port` prints a path for your app to open as it would a board's serial port, such as `~/Library/Caches/preflash/<hash>.tty`. The path is a link to a pseudo-terminal, and it stays the same from one run to the next. The port carries the serial stream, so your app reads what `preflash serial` shows, and each line it writes joins the console queue, ending in `\n`, `\r` or `\r\n`. Preflash doesn't read your protocol. With the demo running, write `status` to the port and read the answer back.

If your firmware installs the USB serial driver, it owns the wire. Everything your app writes to the port reaches `usb_serial_jtag_read_bytes` byte for byte, `@sim ` lines and line endings included. Lines from stdin and `preflash send` reach it too, each with a `\n`, except `@sim ` lines, which stay with Preflash. Until the driver is in, these lines wait in the queue. Firmware reads serial input through that call, because stdin stays with Preflash.

The port starts raw at 115200 with no echo. Your app keeps its connection through `esp_restart` and RST, and sees the port hang up on quit, on a crash, on a rebuild and in download mode, as it would when you unplug a board. Open the path again to reconnect.

To put a link where your app's developer setting looks, add one to `.preflashrc`. Preflash removes both links when the app stops, and refuses to replace a file that isn't a link. Only `kill -9` leaves them behind, and the next run replaces them.

```yaml
serial_link: sim/my-board.tty
```

A relative path starts at the project, and `~/` at your home folder. The link's folder must exist already. If a link fails, the app prints why and runs on, and `preflash port` gives the device's own path if there's no other.

The port doesn't appear as a USB device, so an app that finds boards by USB identity needs that developer setting. Firmware never waits for the port. It holds about 65 KiB, and past that it loses the oldest output, with no notice in the stream, because a notice would land in your protocol. An app that opens the port late reads what it's holding first, so expect old lines ahead of live ones, with a gap between them.

The port has no DTR or RTS lines, so nothing your app does with them resets the firmware, and setting them fails with `ENOTTY`. If your app drops DTR and RTS to keep a real board from resetting, have it ignore that error, as pyserial does when it opens a port.

A link that `kill -9` left behind points at a pseudo-terminal number macOS can give to a new Terminal tab. Before your app opens a link on its own, such as on a timer, check that Preflash is running: the control socket beside the default link, `<hash>.sock`, accepts a connection only while the app runs.

## Describe your board

A board comes from `--board`, then from `board:` in the project's `.preflashrc`, then the default, `ESP32-S3-Touch-LCD-4B`. Each can be a stock board's name or a YAML file. A path in `.preflashrc` is relative to the project.

A board file can extend a stock board with your own hardware changes. The demo's [`sim/demo-4b.yaml`](examples/demo/sim/demo-4b.yaml) describes a small rework, and the firmware doesn't change:

```yaml
name: demo-4b
extends: ESP32-S3-Touch-LCD-4B

changes:
  power_amp: exio5
```

Or it can stand alone, for a custom PCB whose project uses the same BSP calls or drives `esp_lcd` itself. Every chip it lists needs a model in [`runtime/src/chips/`](runtime/src/chips), and the run stops and names any that doesn't have one.

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
| `buttons` | The board's hardware buttons, in the order the window's button strip shows them. Each has a `name` and one of `gpio: gpio0` or `expander: tca9554.3` (both with `active_low`), `axp2101: pwrkey`, `reset: en`, or `power: {key: gpio16, active_low: true, hold: tca9554.6}` |

The stock boards list the buttons their schematics show, and the window names each one with where it connects. A list under `changes` replaces the stock one. A button on `axp2101` needs an `axp2101` chip under `i2c`. Names must be unique, and 1 to 5 letters, digits, `-` or `_`, which fit the strip's buttons. Press them in the window or with [`preflash hardware button`](#drive-it-from-a-script).

A channel set up with `ledc_channel_config` on the backlight pin sets the brightness, so firmware dims the panel through LEDC as it would on the board. On a stock 3.49 V2 the window shows the bare portrait panel, so firmware that draws in landscape appears on its side. `esp_lcd_new_panel_axs15231b` is the one panel driver Preflash has, and any other stops the build at the missing call. The demo's [`board_349.cpp`](examples/demo/main/board_349.cpp) drives that panel with no BSP, and [`board_4b.cpp`](examples/demo/main/board_4b.cpp) goes through the board package.

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
- A driver bug hidden by a chip model that's wrong. See [Check the models against a board](#check-the-models-against-a-board).

OTA isn't simulated either. The running slot is always valid, and starting a download fails with a message saying so, so update code stays untested.

With `CONFIG_ESP_TASK_WDT_INIT` set, the task watchdog watches subscribed tasks and a panic counts as a crash, where a device would restart. It doesn't watch idle tasks, so an idle task starved of CPU won't trip it. A task can delete itself with `vTaskDelete`, but deleting another task stops the process.

If your firmware stops on a call Preflash doesn't implement, that call needs a stand-in.

### Checks that need a person

`make test` can't do these, so try them yourself with the demo:

- **A mouse click in the window.** The tests press the panel through the socket. Click a tile with a real mouse and watch the page open.
- **Buttons with a mouse.** The tests press buttons through the socket. Run the demo on both boards and check that the strip is dark with the board's buttons on the left and rotate on the right, and that a button draws filled while the mouse holds it and while `preflash hardware button BOOT press` does. Hover text shows what a button is wired to, and the rotate button shows an accent border on hover and fills while pressed. ⌥-click BOOT: it stays down, through the pointer leaving the window and switching to another app and back, and its hover text says "latched". A plain click, or another ⌥-click, lets it go. Press and hold a button, then leave the window or lose focus: it lets go.
- **The status bar.** It shows the board's full name in light text and the turn angle dimmer, wraps the 3.49 V2's long name onto two lines, and draws `°` and `·` properly. The text is crisp at 1x.
- **Turning.** Click rotate four times, then use ⌘→ and ⌘←. The window swaps between portrait and landscape where it should, the picture turns clockwise, the strip and status bar stay upright, and a click at each angle lands on the pixel under the pointer, and `preflash status --json` shows `board_rotation` following the turns while `rotation` stays LVGL's. Drag from the panel out of the window and release: the touch ends at the panel's edge.
- **Download mode in the window.** Latch BOOT with ⌥-click and click RST. The window stays, the panel goes dark and the status bar adds "download mode". With a companion app on the serial port, it sees a hang-up on entering download mode and can reopen the same link after the next boot. Click BOOT to let it go, then click RST: the firmware boots on a black panel. A click on RST alone keeps the last frame until the new boot draws.
- **Power in the window.** On the 4B, hold PWR with the mouse for 6 seconds (with the demo's `buttons pwr-hold off`, so the chip's own hold acts): the window stays, the panel goes dark and the status bar says "off". Let go, then click PWR and the firmware boots. In the demo, hold PWR for 1.5 seconds and the firmware switches off on its own, and the Power page's soft switch also makes the status bar say "off", with a click on PWR booting it again. On the 3.49 V2, run with `preflash hardware battery 50` and `usb unplugged`: the board goes off at once. Hold PWR with the mouse and it boots, then goes off when you let go, unless the firmware drives the hold. ⌥-click PWR keeps it on.
- **The window through a restart.** The tests run the window with SDL's dummy driver. Run the demo and check the window stays put with its last frame until the new boot draws after each of: `restart`, `^R`, `crash null` followed by `^R`, RST, and `^B` to the other board (which reshapes the window in place). After power off and on, the window stays put and the panel stays black until the new boot draws. Kill the `preflash-window` process with `kill -9`: a new window appears within about 2 seconds and the firmware keeps running. Close the window with its button, press `^C` in the terminal app, or run `preflash quit`: the app and the window both go. Hold the mouse down on the panel, send `restart`, and check the new boot shows no stuck press.
- **Mouse touch.** Click a tile and watch the page open. Drag on a page that scrolls. The panel dims as the firmware lowers the backlight, and the title stays the project's name.
- **PWR on battery with a real key circuit.** Run a board that has a power key, such as the 3.49 V2, on battery and check the PWR hold behaves as the schematic says: GPIO16 is the key, and TCA9554 bit 6 is its hold.
- **The audible chime.** The tests count the sample frames firmware writes. Press the Beep shortcut and listen.
- **The debug and crash screens with a window open.** The tests run these screens headless. Run the demo with a window, press `^D`, then send `crash null`.
- **`g` in the crash screen.** The tests use a stand-in for `lldb`. Press `g` with the real one and check it attaches.
- **Hardware captures.** These need a board on your desk, as the next section says.

### Check the models against a board

No chip has yet been compared with a real board. The comparison tools exist, and nobody has run them on a chip, so `hardware/captures/` stays empty until someone with the boards does. Everything Preflash has checked so far comes from datasheets, Waveshare's material and synthetic captures, and none of those say a model matches its chip.

[`hardware/capture`](hardware/capture) is an ESP-IDF project with a config for each stock board. It runs a fixed list of driver calls against each chip and prints JSON lines. Its README has the steps to build, flash and save a capture. `preflash check` then sends the same transactions to the models and marks each difference as expected, such as the time or sensor noise, or as a mismatch, and exits 1 on a mismatch. A capture that didn't come from a real chip says `synthetic` in the file, and `preflash check` says so at both ends of its output.

The ES8311, ES7210 and GT911 models answer on the bus and hold no behaviour. Each stays that way until real captures show what it needs, so expect mismatches from them.

## Development

### Test

```sh
make test
```

That runs the Go tests, the runtime suite and the integration scripts, and it needs no other project and no ESP-IDF. It does need bash 4 or later first on your `PATH` (`brew install bash`), because macOS's own bash 3.2 can let a failing script pass. `tests/integration/demo.sh` is the main end-to-end test: it runs [`examples/demo`](examples/demo) on both stock boards, drives each page, shortcut, forced failure and crash through the socket, and compares the panel with the frames in `tests/integration/golden/demo/`. Its first lines say how to regenerate them. The test holds the internet offline, so it never reaches a public site, though a clean checkout downloads its dependencies once.

`tests/integration/debug.sh` checks traces and forced failures through a running project. `tests/integration/crash.sh` crashes a project each way, under each sanitizer too, and reads the reports back. `tests/integration/check.sh` runs `preflash check` on hand-written captures and runs the capture firmware under Preflash on both stock boards, so all of it is synthetic. `tests/integration/tui.sh` runs the terminal app in a pseudo-terminal and reads its screen through a virtual terminal, including a mouse click on a shortcut. `tests/integration/lcd.sh` runs a project with no BSP from `tests/fixtures/lcd_project`. The suite also has optional scripts for private projects, and each skips unless its variable points at a checkout.

GitHub Actions runs `make test` on each pull request and each push to `main`. An integration script gives the app 10 seconds to start, and that includes a build. On a slow or cold machine, set `PREFLASH_TEST_WAIT` to allow more, in seconds, as CI does.

`make bench` times the demo on a copy of it. `preflash bench --project dir` does the same for a project whose console answers `spew <bytes>` by printing that many bytes and then `spew done`, and prints a table on stderr and JSON on stdout.

### Layout

```text
cmd/preflash/     command line
internal/app/     supervising a build and its app for the terminal app
internal/board/   board files: extends, changes and validation
internal/check/   reading captures and comparing them with the chip models
internal/client/  talking to a running app over its socket
internal/crash/   reading crash reports and tying them to source
internal/project/ finding, building and running a project
internal/rc/      reading .preflashrc
internal/tui/     the terminal app's screens
internal/bench/   timing a project for preflash bench
boards/           stock boards
examples/demo/    a reference ESP-IDF project for both stock boards
hardware/capture/ firmware that records what real chips answer
runtime/idf/      the CMake layer that stands in for IDF_PATH
runtime/include/  ESP-IDF headers the firmware compiles against
runtime/src/      stand-ins, chip models, the app's end of the window link and the host
runtime/window/   the window process, preflash-window
runtime/replay/   the chip models with a main of their own, for preflash check
tests/            runtime suite, fixtures, integration scripts and terminal tests
.github/assets/   README images, from source/capture.sh and render.sh
```

### Contributing

Commits use conventional commit titles. A new chip means a new model in `runtime/src/chips/`, and a new ESP-IDF call means a stand-in that follows the device's return codes, or that stops and names the call.

## License

Preflash is under the [MIT License](LICENSE), and so are the demo and capture firmware, so you can copy from them into your own projects.
