#pragma once

#include <cstdint>

constexpr int kDisplayBandRows = 16;

void display_init();
// Queues rows [y, y + rows) in native RGB565 order; rows must not exceed kDisplayBandRows.
void display_rows(int y, int rows, const uint16_t* pixels);
// Sets the backlight in thousandths of full brightness, on a perceptual curve.
void display_backlight(int permille);
bool touch_read(int& x, int& y);
