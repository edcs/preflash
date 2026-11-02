# Capture

This firmware runs a fixed list of I2C calls against each chip on a stock board and prints what the chips answered as JSON lines. `preflash check` then sends the same calls to Preflash's chip models and reports each difference.

You run it, on a board you hold, with ESP-IDF 5.4. Preflash's tests never flash a board.

| Board | Chips |
| --- | --- |
| ESP32-S3-Touch-LCD-4B | TCA9554, ES8311, ES7210, GT911, AXP2101, PCF85063, QMI8658 |
| ESP32-S3-Touch-LCD-3.49-V2 | TCA9554, QMI8658, PCF85063, AXS15231B touch |

## What it does to the board

Flashing replaces the firmware, the bootloader and the partition table, so flash your own project back when you finish.

The capture leaves the RTC's time alone and sends the AXP2101 reads only. It writes to the TCA9554, the codecs, the GT911's status, the QMI8658 and the RTC's RAM byte. It resets the panel and touch through the expander, because touch won't answer until it has been reset. Registers that survive a power cut get their old values back. `main/chips.cpp` lists every call.

## Run it

Set `PORT` to your board's serial port, which `ls /dev/cu.usbmodem*` shows, and pick the board. Run these from this folder, with the 3.49 V2 shown in a comment:

```sh
. ~/esp/esp-idf/export.sh
export PORT=/dev/cu.usbmodem101
export BOARD=ESP32-S3-Touch-LCD-4B CONFIG=4b
# export BOARD=ESP32-S3-Touch-LCD-3.49-V2 CONFIG=349
```

**1. Build.**

```sh
idf.py -B build-$CONFIG -D SDKCONFIG=sdkconfig.$CONFIG \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$CONFIG" set-target esp32s3 build
```

**2. Flash.**

```sh
idf.py -B build-$CONFIG -D SDKCONFIG=sdkconfig.$CONFIG -p $PORT flash
```

**3. Cut the power.** Unplug the USB cable and any battery, wait ten seconds, then plug the USB cable back in. Leave the battery out. A reset only restarts the ESP32-S3, and the capture wants the other chips at their power-on values.

**4. Save the output.** Lay the board flat and keep your fingers off the screen. Start the monitor with a copy going to a file:

```sh
idf.py -B build-$CONFIG -D SDKCONFIG=sdkconfig.$CONFIG -p $PORT monitor | tee raw-$CONFIG.log
```

The firmware prints `Press Enter to start the capture.` every two seconds until you press Enter. It runs once and finishes with `Capture finished.` within a few seconds. Press Ctrl+] to leave the monitor.

If `tee` leaves the file empty, press Ctrl+T then Ctrl+L inside the monitor before you press Enter, and use the `log.*.txt` file it writes.

**5. Keep the JSON lines.**

```sh
mkdir -p ../captures
grep '^{' raw-$CONFIG.log > ../captures/$BOARD.jsonl
```

**6. Run the check.** Use the `preflash` you built from this checkout:

```sh
preflash check ../captures/$BOARD.jsonl --board $BOARD
```

The first run builds the chip models, which takes a few seconds. To go again, start from step 3.

## Read the result

`preflash check` prints a line for each chip and a line for each byte that differs:

```text
pcf85063 at 0x51, bus 0: 7 transactions, 5 match, 2 expected, 0 mismatched
  #41 register 0x04: real 0x37, model 0x12: expected (time)
  #41 register 0x05: real 0x09, model 0x41: expected (time)
```

`expected` marks a value no model can share: `time` on the RTC's clock, `sensor noise` on the accelerometer and the GT911's touch scan, and `power state` on the AXP2101's status and battery registers. `MISMATCH` marks everything else, and the command exits 1 if it finds one. A failed transaction, a chip the model refuses to answer for and a chip with no transactions count too.

Expect mismatches on the first run:

- The ES8311, ES7210 and GT911 models answer on the bus and hold no behaviour, so their ids read 0. They wait for this comparison to show what they need.
- The RTC keeps its RAM byte, offset and control registers on its cell, so firmware you ran before can show up here.
- The TCA9554's input register depends on what the board wires to each pin. The model reads an input as high.
- The AXP2101's charge current comes from an eFuse, and the model holds a stand-in.

A file that stops part way, loses a line or holds two runs fails with the line it stopped on. Run the capture again.

## The label

The first line of a capture has a `source`. Firmware built for the chip writes `board`. The same firmware run under Preflash writes `synthetic`, and so does every hand-written capture in `tests/fixtures/captures/`. `preflash check` refuses a file with no `source`, and prints a warning at both ends of its output for a synthetic one, because the models agreeing with themselves says nothing about a real chip. A test fails if a synthetic file lands in `hardware/captures/`.
