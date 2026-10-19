#pragma once

#include "pipkin/model.h"

namespace pipkin {

// Plays the product tour shown on GitHub and pipkin.io, for the demo reel firmware
// (built with -DPIPKIN_DEMO=ON and flashed by `pipkin flash --demo`). Sample readings
// arrive during the boot, then the pages take turns. Time stands still at 09:41, so the
// readings stay fresh and their resets never change. Call it every loop, after touch.
void play_demo(State& state, uint64_t now_ms);

} // namespace pipkin
