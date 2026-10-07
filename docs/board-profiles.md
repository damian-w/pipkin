# Board profiles and identification

This guide is for contributors collecting evidence for another CYD board. The
catalog is [boards/profiles.json](../boards/profiles.json). Firmware uses the
profile selected in [main/board.h](../main/board.h); release packaging checks that
its identity, qualification and flash settings agree with the catalog.

## What a profile identifies

A profile describes the firmware's display, touch and flash compatibility. A
physical board variant records PCB markings, module markings, USB connectors and
the evidence collected from a particular model. Several variants can share one
firmware profile when their screen, touch controller and wiring match.

USB VID/PID, chip revision, crystal frequency and flash identification are useful
observations. They do not uniquely identify a CYD PCB or its display and touch
controllers. A running Pipkin's `board` field identifies the profile compiled into
that firmware; it does not measure the PCB. Keep these distinctions in CLI output
and when reviewing a proposed catalog entry.

## Collect a board report

With CLI 1.4.0 or later, connect one board and run `pipkin identify --issue`, using
`--port PORT` when several serial devices are connected. The command collects
read-only chip, flash, USB and firmware observations, pauses the helper while it
owns the port, and restores its original service state. Entering the ROM loader
can briefly restart the board; the command does not flash firmware or write flash
or eFuses.

Open a [board support issue](https://github.com/damian-w/pipkin/issues/new?template=board-support.yml) and
include the command's shareable report. Add clear photographs of the front and
back, all PCB labels and module markings, the USB connector types, and what works
on the display and touch screen. Record the firmware version used for checks.
The `--issue` output omits MAC addresses, USB serial numbers and local paths;
check any additional screenshots or attachments for unit identifiers and account
details before sharing. The report's electronics observations are supporting evidence; board
markings and functional checks are still needed.

Do not erase or replace an owner's firmware merely to identify a board. If a
different LCD or touch controller is suspected, establish its wiring and driver
requirements before attempting firmware for an existing profile.

## Catalog format

The top-level `schema_version` is 1. `profiles` contains firmware compatibility
profiles with these fields:

| Field | Meaning |
| --- | --- |
| `id` | Canonical firmware profile identifier |
| `aliases` | Older firmware identifiers with the same implementation and flash layout |
| `name` | Human-readable family name |
| `hardware` | `unconfirmed` until hardware qualification is recorded; then `confirmed` |
| `chip`, `flash_size` | Required chip family and flash bytes |
| `flash_mode`, `flash_freq`, `layout` | Release flash settings and partition layout |
| `display` | Controller and landscape width/height |
| `touch` | Controller and touch type |
| `variants` | Evidence for individual physical board models |

A variant should contain an `id`, `name`, `pcb_markings`, `module_markings`,
`usb_connectors`, `observations`, `qualification` and `notes`. Use arrays for
markings, connector types and notes. Record a display label and its stated native
dimensions in `observations`; the profile's dimensions describe Pipkin's landscape
orientation. A panel marking alone does not establish its controller. Keep
non-unique electronics observations in `observations`; never store unit MAC
addresses or USB serial numbers in the
catalog. `qualification` records observed results for `display`, `touch`,
`flash`, `update` and `usb_reset`: `true` for a completed check, `false` for a
failed check, and `null` when a check has not been performed. Do not infer a
successful check from another unit or a seller's listing.

## Qualify and add a board

Compare the submitted markings and components with existing variants. If the
board shares the current ILI9341/XPT2046 wiring, add its evidence to the existing
profile. Otherwise add a separate profile and implement its driver and pin
configuration before advertising support. A new catalog row alone does not make
the firmware support different hardware.

At minimum, qualify display orientation, colors and backlight, plus touch response
and page selection on physical hardware before marking a profile `confirmed`.
Record first installation, settings-preserving updates and USB reset checks as
they are completed. Keep any untested behavior explicit in the variant's notes.

The qualified profile uses canonical ID `esp32-2432s028r` and retains
`esp32-2432s028r-provisional` in `aliases`. Firmware 1.3.0 emits the canonical ID;
its manifest requires CLI 1.4.0 or later. The CLI resolves both identities to the
same firmware profile to permit updates from older releases. Preserve the
existing flash layout and settings; identifier migration does not authorize
moving or erasing NVS.

The CLI embeds a copy of this catalog so board reports work without a firmware
release download. Update that copy in the same change and verify that the files
are identical. Keep catalog details outside `firmware.json`: older CLIs reject
unknown manifest fields. Before opening the PR, run the firmware packager tests,
hardware checks and CLI tests, then describe the physical checks and remaining
unknowns in the PR.
