# Building and developing Pipkin

This guide is for DIY builders and contributors. If you have a Pipkin kit, start
with the [README](../README.md).

## Firmware

### Install a firmware release

Install the [Pipkin CLI](https://github.com/damian-w/pipkin-cli), connect a supported
board with a USB data cable, then run:

```sh
pipkin flash
```

The CLI installs the latest published stable firmware for the connected board after
showing its plan and asking for confirmation; the same command updates an existing
Pipkin, and `pipkin update` updates the CLI itself. The firmware and CLI share a major
version, which is the protocol version they speak, so CLI 2.x installs and talks to
firmware 2.x. Both supported boards are described under [board profiles](#board-profiles).
Firmware has a single application slot and no automatic rollback; rerun `pipkin flash`
after an interrupted write. See the
[CLI firmware guide](https://github.com/damian-w/pipkin-cli/blob/main/docs/firmware.md)
for board detection, selecting a port or version, reinstalling and USB recovery.

### Build firmware from source

Pipkin builds with ESP-IDF **v5.4.2** (commit
`f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0`). Follow
[Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32/get-started/index.html),
install the `esp32` and `esp32s3` tools, activate the SDK environment, then run these
from the repository root; the ESP-IDF target selects the board. For the 2.8-inch CYD:

```sh
idf.py set-target esp32
idf.py build
idf.py -p PORT flash
```

For the 4-inch ES3C40P, in its own build directory and configuration:

```sh
idf.py -B build-es3c40p -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-es3c40p/sdkconfig build
idf.py -B build-es3c40p -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-es3c40p/sdkconfig -p PORT flash
```

Replace `PORT` with the board's serial port. A board that has never run Pipkin
may have other settings where Pipkin keeps its own; `pipkin flash` clears them on a
first installation, and a source build can do the same with
`esptool.py --port PORT erase_region 0x9000 0x6000`. The build checks the SDK version and
target, enables reproducible builds and path remapping, and strips debug sections
to keep build-machine paths out of the binary.

If the Pipkin helper is running, use `pipkin stop` before `idf.py -p PORT flash`
so it releases the serial port, then `pipkin start` afterward. If it was already
stopped, leave it stopped.

## Tests

The display logic, protocol, renderer and release packaging are tested without
hardware:

```sh
scripts/check.sh
```

`tests/fixtures/helper_packets.txt` is a copy of
`internal/pipkin/testdata/helper_packets.txt`, which the
[Pipkin CLI](https://github.com/damian-w/pipkin-cli)'s tests write. The core test must
accept every packet in it, so the two sides of the protocol cannot drift apart. After an
intended protocol change, regenerate the fixture in the CLI repository and copy it here.

The checks run the renderer and board helpers for both boards. CI also builds the
firmware for both boards on every push and pull request. Pushing a `v*` tag that
matches `PROJECT_VER` in `CMakeLists.txt` also packages the firmware and creates a
draft release. The CYD keeps the release's original asset names, `firmware.json`
and its images, which every 2.x CLI reads; the ES3C40P's are
`firmware-es3c40p.json` and images ending `-es3c40p`, which CLI 2.1 and later use.

## Layout

| Path | Contents |
| --- | --- |
| `src/`, `include/` | Display state, protocol parser and renderer (portable C++17) |
| `main/` | ESP32 entry point, serial link, display and touch drivers, board profiles |
| `docs/protocol.md` | Serial protocol reference |

## Board profiles

`main/board.h` holds both board profiles, and the ESP-IDF target picks one: `esp32`
builds the CYD and `esp32s3` the ES3C40P. Pin assignments, the panel's start-up
commands and orientation, touch, SPI speed and backlight polarity all live there;
`main/link.cpp` chooses the serial connection.

| | `esp32-2432s028r` (2.8-inch CYD) | `es3c40p` (4-inch LCDWIKI ES3C40P) |
| --- | --- | --- |
| Chip and flash | ESP32, 4 MB | ESP32-S3, 16 MB, 8 MB PSRAM (unused) |
| LCD | ILI9341-compatible, 320 × 240, SPI | ST7796S IPS, 480 × 320, SPI |
| Touch | XPT2046-compatible resistive, SPI | FT6336U capacitive, I2C |
| Computer link | UART0 through a USB serial bridge | The ESP32-S3's own USB serial port |

The CYD profile has been tested on two dual-USB variants (USB-C and micro-USB),
marked "ESP32-2432S028R" and "Bruce CYD 2432S028". Equivalent 2.8-inch touch CYD
boards with ESP32-S or WROOM modules use this same profile when their wiring and
screen hardware match [main/board.h](../main/board.h). Module branding or a printed
model label alone does not require a separate firmware profile.

The ES3C40P profile follows LCDWIKI's pin table. Its landscape orientation is the
vendor firmware's turned 180 degrees, so the USB-C port is on the left. Its speaker, microphone, SD card slot,
RGB LED and battery input are unused, so both the speaker and no-speaker variants
work. Other screen hardware needs its own profile.

## Display

What the screen shows, and how it responds to touch, host reports and task records,
is specified in the protocol reference under
[host status](protocol.md#host-and-application-status),
[allowance groups](protocol.md#allowance-groups),
[local tasks](protocol.md#local-tasks) and
[display behaviour](protocol.md#display-behaviour); this section covers how the
firmware draws it and drives the backlight.

The layout is designed for 320 × 240. The 4-inch build (`PIPKIN_DISPLAY_480X320`)
draws the same layout 4/3 larger, so text and gauges are larger on its screen, and
the extra width goes to the columns: the layout is 360 × 240 there, and touch
positions are scaled to match. Its fonts are Inter rasterized at 4/3 of each size
(`src/font_data_480x320.h`); the 2.8-inch display's pixels are unchanged.

The renderer draws 16-row bands into a buffer (10 KB, or 15 KB on the 4-inch
display) and sends only the bands that changed, using two DMA buffers so drawing
and transfer overlap. The screen is
re-evaluated once a second; unchanged frames send nothing. Page changes grow the
gauges from empty over 650 ms, and a changed reading eases from the value on screen
over the same time. The selected page is saved to flash only when it changes.

At power-on, Pip rides the loading bar for a 1.4-second intro. The bar waits for
the first reading, adds setup guidance after eight seconds, and shows a QR code
for `https://pipkin.io/start` after eighteen seconds without the helper. On the
first reading, a four-second finish animation leads into Overview.

The PWM backlight follows explicit `kind=host state=asleep` and `awake` reports.
The CLI observes system sleep/resume and shutdown on macOS, Windows and Linux
(systemd-logind), plus display sleep/wake on macOS and Windows. It preserves display
sleep during a background system wake. Startup runs at sign-in; pre-login boot
waking is not provided by the current user services.

While connected, stale readings dim to 20% after five stale minutes (ten minutes
without a fresh observation), then turn off after fifteen stale minutes. Once the
backlight is off, the panel stops showing its frame until it wakes, so a
still image such as the setup QR code can't leave a faint ghost on the LCD.
Serial reception remains active while dark, so a host report can wake the display
without resetting the ESP32. Tune `kHostPresenceMs`, `kHostWakeMs`,
`kDimAfterStaleMs`, `kOffAfterStaleMs`, `kTouchWakeMs` and `kTaskFreshMs` in
`include/pipkin/model.h`.

## Serial connection

The display listens for newline-delimited packets of at most 768 bytes: on the CYD,
UART0 at 115200 baud, 8N1, through its USB serial bridge; on the ES3C40P, the
ESP32-S3's own USB serial port, where the baud rate doesn't matter. [The protocol reference](protocol.md) defines every field. Send
`v=2 kind=identify` to receive the device's firmware version, board, sequence and
clock state; this never resets or flashes the device.

Many CYD boards have an automatic reset circuit that restarts the ESP32 if DTR and RTS
change independently when a port is opened. The ESP32-S3's USB port restarts it when
RTS is set while DTR is clear. The Pipkin CLI keeps the port open to avoid repeated
restarts, and resends everything if it detects one.

## Remaining hardware work

- Application rollback: the flash layout has a single application slot.
- More testing of touch accuracy, USB behaviour across host platforms, first-time
  flashing, firmware updates and host sleep signalling.
