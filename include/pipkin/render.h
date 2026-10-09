#pragma once

#include "pipkin/model.h"

namespace pipkin {
bool animation_active(const State& state, uint64_t now_ms);
// Renders rows [first_row, first_row + rows) into pixels, which holds rows * kDisplayWidth values.
void render(const State& state, uint64_t now_ms, uint16_t* pixels, int first_row = 0,
            int rows = kDisplayHeight);
} // namespace pipkin
