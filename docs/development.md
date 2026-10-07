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

CLI **1.4.0 or later** downloads the latest published stable firmware and the pinned
Espressif flashing tool, checks the connected board and current firmware, and shows
the plan before asking for yes/no confirmation. Enter means no. You do not need
Python or ESP-IDF for this route. The same command updates an existing Pipkin;
`pipkin update` updates the CLI itself.

The target is an **ESP32 CYD 2.8-inch touch board with 4 MB flash** matching the
existing [board profile](#board-profile). The tested physical variants are recorded
in the [board catalog](../boards/profiles.json). Equivalent ESP32-S and WROOM-style
boards use the same firmware when their wiring matches. Display and touch operation
have been confirmed on the recorded variants. USB checks can
identify the ESP chip and flash, but not the attached screen or touch hardware. A
board with unknown or other firmware needs your confirmation that it is the intended
CYD. Installing Pipkin replaces that firmware and its saved application settings.
Compatible Pipkin updates and recovery of recognized stored Pipkin firmware retain
the selected display page. A stored version is not proof that the firmware is running;
use `pipkin flash --reinstall` to repair the same version when it does not respond.
An incompatible existing Pipkin partition layout is rejected before writing.

Firmware releases are created as drafts for maintainer review and qualification.
The CLI uses only published stable releases with compatible flash artifacts. See
the [CLI firmware guide](https://github.com/damian-w/pipkin-cli/blob/main/docs/firmware.md)
for selecting a port or version, deliberate reinstalls and USB recovery. Firmware
has a single application slot and no automatic rollback; rerun `pipkin flash` after
an interrupted write.

### Build firmware from source

Pipkin builds with ESP-IDF **v5.4.2** (commit
`f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0`). Follow
[Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32/get-started/index.html),
install the `esp32` tools, activate the SDK environment, then from the repository root:

```sh
idf.py set-target esp32
idf.py build
idf.py -p PORT flash
```

Replace `PORT` with the board's serial port. The build checks the SDK version and
target, enables reproducible builds and path remapping, and strips debug sections
to keep build-machine paths out of the binary.

If the Pipkin helper is running, use `pipkin stop` before `idf.py -p PORT flash`
so it releases the serial port, then `pipkin start` afterward. If it was already
stopped, leave it stopped. This source-build route remains available for modified
firmware and development.

## Tests

The display logic, protocol and renderer are tested without hardware:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude src/model.cpp src/protocol.cpp tests/core_test.cpp -o /tmp/pipkin-core && /tmp/pipkin-core
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude src/model.cpp src/render.cpp tests/render_check.cpp -o /tmp/pipkin-render && /tmp/pipkin-render
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Imain tests/hardware_check.cpp -o /tmp/pipkin-hardware && /tmp/pipkin-hardware
```

`tests/fixtures/helper_packets.txt` is a copy of
`internal/pipkin/testdata/helper_packets.txt`, which the
[Pipkin CLI](https://github.com/damian-w/pipkin-cli)'s tests write. The core test must
accept every packet in it, so the two sides of the protocol cannot drift apart. After an
intended protocol change, regenerate the fixture in the CLI repository and copy it here.

CI runs these on every push, along with a firmware build.

## Layout

| Path | Contents |
| --- | --- |
| `src/`, `include/` | Display state, protocol parser and renderer (portable C++17) |
| `main/` | ESP32 entry point, display and touch drivers, board profile |
| `docs/protocol.md` | Serial protocol reference |

## Board profile

`main/board.h` holds the qualified `esp32-2432s028r` ("Cheap Yellow Display")
profile: ILI9341-compatible LCD, XPT2046-compatible resistive touch, 320 × 240
landscape, 4 MB flash. Pin assignments, touch calibration, display orientation,
SPI speed and backlight
polarity all live there. The [board catalog](../boards/profiles.json) records the
two tested dual-USB variants, their physical markings, electronic observations and
completed checks. Controller compatibility is inferred from working display and
touch drivers; controller silicon was not read back. Check an unfamiliar board's
wiring and components before flashing.

Equivalent 2.8-inch touch CYD boards with ESP32-S or WROOM modules use this same
profile when their wiring and screen hardware match [main/board.h](../main/board.h).
Module branding or a printed model label alone does not require a separate firmware
profile. The current drivers and pin assignments remain the compatibility boundary;
other screen hardware or ESP32-S3 boards need their own implementation and qualification.

See [Board profiles and identification](board-profiles.md) for the contributor
report workflow and adding another physical variant. Firmware 1.3.0 uses the
canonical profile ID and requires CLI 1.4.0; the old provisional ID remains an
alias for compatible updates.

## Display

The renderer draws 16-row bands into a 10 KB buffer and sends only the bands that
changed, using two DMA buffers so drawing and transfer overlap. The screen is
re-evaluated once a second; unchanged frames send nothing. Page changes grow the
gauges from empty over 650 ms, and a changed reading eases from the value on screen
over the same time. The selected page is saved to flash only when it changes.

Before Claude's first message, a confirmed idle session shows "Not started" and
"Starts on first message" with a neutral gauge; weekly allowance stays visible.
The CLI confirms this only for zero utilization and an explicit null reset.

At power-on, Pip rides the loading bar for a 1.4-second intro. The bar waits for
the first reading, adds setup guidance after eight seconds, and shows a QR code
for `https://pipkin.io/start` after eighteen seconds without the helper. On the
first reading, a four-second finish animation leads into Overview.

The PWM backlight follows explicit `kind=host state=asleep` and `awake` reports.
The CLI observes system sleep/resume and shutdown on macOS, Windows and Linux
(systemd-logind), plus display sleep/wake on macOS and Windows. It preserves display
sleep during a background system wake. Startup runs at sign-in; pre-login boot
waking is not provided by the current user services.

If host heartbeats stop, the screen turns off after 90 seconds, even before any
usage has arrived. This does not change the host's diagnostic power state. Explicit
disconnect also turns it off. An explicit wake or heartbeat recovery lights the
screen for a minute while fresh usage loads; regular heartbeats do not extend this
grace. While connected, stale readings dim to 20% after five stale minutes (ten
minutes without a fresh observation), then turn off after fifteen stale minutes.
Touch wakes a screen darkened by stale readings or missing helper traffic for a
minute without activating a page control; explicit host sleep ignores touch.
Serial reception remains active while dark, so a host report can wake the display
without resetting the ESP32. Tune `kHostPresenceMs`, `kHostWakeMs`,
`kDimAfterStaleMs`, `kOffAfterStaleMs` and `kTouchWakeMs` in `include/pipkin/model.h`.

Hold anywhere on the screen for three seconds to open the About/status screen,
which shows the firmware version, source revision, the connection to your computer
and how recent each reading is. The hold allows small finger movements and brief
touch dropouts. It opens while your finger is down and stays open when you release.
A later tap or swipe closes it; it also closes itself after a minute untouched.
For more detail, run `pipkin status` on the computer.

## Serial connection

The display listens on UART0 at 115200 baud, 8N1, for newline-delimited packets of at
most 768 bytes. [The protocol reference](protocol.md) defines every field. Send
`v=1 kind=identify` to receive the device's identity, firmware version, sequence and
clock state; this never resets or flashes the device.

Many CYD boards have an automatic reset circuit that restarts the ESP32 if DTR and RTS
change independently when a port is opened. The Pipkin CLI keeps the port open to avoid
repeated restarts, and resends everything if it detects one. This behaviour still
needs qualification on production boards.

## Remaining hardware work

- Application rollback: the flash layout has a single application slot.
- Additional qualification of touch accuracy, USB behavior across host platforms,
  first-time flashing, firmware updates and host sleep signalling; the catalog
  records which checks have been completed on each variant.
