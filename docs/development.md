# Building and developing Pipkin

This guide is for DIY builders and contributors. If you have a Pipkin kit, the
[README](../README.md) is all you need.

## Firmware

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

`main/board.h` holds a **provisional** ESP32-2432S028R ("Cheap Yellow Display")
profile: ILI9341 LCD, XPT2046 resistive touch, 320 × 240 landscape, 4 MB flash. Pin
assignments, touch calibration, display orientation, SPI speed and backlight
polarity all live there. They have not yet been confirmed on production hardware,
so check them against your board before flashing.

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

The PWM backlight stays bright while readings are fresh. It dims to 20% after
five stale minutes (ten minutes without a fresh observation), then turns off after
fifteen stale minutes. A fresh reading restores brightness; touch wakes it for a
minute without activating a page control. Explicit `kind=host state=asleep`
also turns it off; the CLI currently sends awake heartbeats only. Tune
`kDimAfterStaleMs`, `kOffAfterStaleMs` and `kTouchWakeMs` in `include/pipkin/model.h`.

Hold the clock for 1.2 seconds to open the status screen, which shows the firmware
version, source revision, the connection to your computer and how recent each
reading is. For more detail, run `pipkin status` on the computer. Tap or swipe to
close it; it also closes itself after a minute untouched.

## Serial connection

The display listens on UART0 at 115200 baud, 8N1, for newline-delimited packets of at
most 768 bytes. [The protocol reference](protocol.md) defines every field. Send
`v=1 kind=identify` to receive the device's identity, firmware version, sequence and
clock state; this never resets or flashes the device.

Many CYD boards have an automatic reset circuit that restarts the ESP32 if DTR and RTS
change independently when a port is opened. The Pipkin CLI keeps the port open to avoid
repeated restarts, and resends everything if it detects one. This behaviour still
needs qualification on production boards.

## Not yet available

- Firmware updates through the CLI. Firmware is currently flashed with ESP-IDF.
- Application rollback: the flash layout has a single application slot.
- Hardware qualification of touch accuracy, USB behaviour and host sleep signalling.
