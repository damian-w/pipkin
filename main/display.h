#pragma once

#include <cstdint>

constexpr int kDisplayBandRows = 16;

void display_init();
// Queues rows [y, y + kDisplayBandRows) in native RGB565 order.
void display_rows(int y, const uint16_t* pixels);
// Sets the backlight in thousandths of full brightness, on a perceptual curve.
void display_backlight(int permille);
// Stops or resumes the panel showing its frame; the frame is kept for when it resumes.
void display_power(bool on);
// Reports a touch in layout coordinates (pipkin::kLayoutWidth x kLayoutHeight).
bool touch_read(int& x, int& y);
