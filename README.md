<h1 align="center">Pipkin</h1>

<p align="center"><strong>Your agentic AI allowance, at a glance.</strong></p>

<p align="center">
  <img src="docs/images/badge-works-with-codex.svg" alt="Works with Codex" height="28">
  <img src="docs/images/badge-works-with-claude-code.svg" alt="Works with Claude Code" height="28">
</p>

<p align="center">
  <img src="docs/images/pipkin.png" alt="Pipkin, with Pip peeking over its top edge, moving from its overview to the Codex and Claude Code gauges" width="640">
</p>

Pipkin is a little screen for your desk that shows how much of your Claude and Codex
usage you have left, and when it resets. One glance tells you whether there's room
for another big task, so you're not digging through settings or hitting a limit
halfway through something.

## What you'll see

- **Overview:** Claude and Codex side by side, with your current session and your
  week.
- **A page for each:** big, easy-to-read dials with a countdown to the next reset.
- **The time**, up in the corner.

The numbers show what you have **left**. A gauge turns amber when you're running low
and red once it's used up. If a reading is out of date, or an app hasn't checked in,
Pipkin says so rather than guessing. Claude's page is labelled Claude Code, but it
shows your Claude plan's usage whichever Claude app you use.

<p align="center">
  <img src="docs/images/low.png" alt="The overview with allowances running low" width="400">
  <img src="docs/images/status.png" alt="The status page" width="400">
</p>

## Using it

Swipe left or right, or tap the bottom edge of the screen, to move between pages. Tap
Claude or Codex on the overview to open its page. Pipkin remembers where you left it.

Hold your finger anywhere on the screen for three seconds to open the status page.
It shows whether Pipkin can see your computer and how recent each reading is. Lift
your finger, then tap to go back. It also closes by itself after a minute.

Pipkin sleeps when your computer sleeps and wakes up with it. If it stops hearing
from your computer, or the readings get old, the screen dims or switches off. Touch
it to light it up again for a minute.

## Getting started

1. Install the Pipkin helper on your Mac, Windows or Linux computer. It's a single
   command pasted into Terminal (or PowerShell on Windows), and
   [pipkin.io/start](https://pipkin.io/start) walks you through it.
2. Plug Pipkin in with its USB cable.

That's it. The helper runs quietly in the background and starts whenever you sign in
to your computer. It uses the sign-ins from the Claude and Codex apps you already
have, so there's no new account or password. Those sign-ins are only ever used to ask
Claude and Codex for your usage, and Pipkin doesn't track you.

<p align="center">
  <img src="docs/images/boot.png" alt="Pip riding the loading bar while Pipkin starts up, then hopping as the overview appears" width="400">
</p>

For the details, including exactly what the helper reads, see the
[Pipkin CLI](https://github.com/damian-w/pipkin-cli) and its
[data sources and privacy](https://github.com/damian-w/pipkin-cli/blob/main/docs/data-sources.md)
guide.

## Keeping it up to date

Every so often there's new software for the screen itself. To install it, open
Terminal and run:

```sh
pipkin flash
```

Pipkin shows you what it's about to install and waits for you to say yes.

## Build your own

Pipkin runs on an ESP32 "Cheap Yellow Display" (CYD), a small touchscreen board that
costs around US$17 / A$25 online. [This AliExpress listing](https://www.aliexpress.com/item/1005009383089648.html)
is one example. Install the helper, plug the board in with a USB data cable and run
`pipkin flash`.

To build the firmware yourself, see the [developer guide](docs/development.md).

<p>
  <img src="docs/images/badge-licence.svg" alt="Licence: noncommercial" height="28">
  <img src="docs/images/badge-board.svg" alt="Board: ESP32 CYD" height="28">
  <img src="docs/images/badge-runs-on.svg" alt="Runs on macOS, Linux and Windows" height="28">
</p>

## Licence

Pipkin's firmware, CLI, installers and documentation are source-available under the
[PolyForm Noncommercial License 1.0.0](LICENSE). You may use, build, modify and share
them for noncommercial purposes, including personal use, study and hobby projects.
Include the licence and its `Required Notice` line when sharing. Selling Pipkin or
products built from it needs separate permission.

If you own a Pipkin, whether a kit or one you've built yourself, you may also use this
software with it for any purpose, including paid work. See the
[terms of use](https://pipkin.io/terms).

Third-party components keep their own licences; see the
[firmware notices](https://github.com/damian-w/pipkin/blob/main/assets/NOTICE.md) and
[CLI notices](https://github.com/damian-w/pipkin-cli/blob/main/NOTICE.md).

Codex is a trademark of OpenAI. Claude and Claude Code are trademarks of Anthropic.
Pipkin is an independent product and is not affiliated with or endorsed by either
company.
