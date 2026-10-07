#pragma once

#include <algorithm>
#include <cstdint>

namespace board {

// Provisional ESP32-2432S028R profile; confirm the actual PCB before flashing.
constexpr char kProfile[] = "esp32-2432s028r-provisional";
constexpr int kLcdClock = 14;
constexpr int kLcdMosi = 13;
constexpr int kLcdMiso = 12;
constexpr int kLcdSelect = 15;
constexpr int kLcdDataCommand = 2;
constexpr int kBacklight = 21;
constexpr bool kBacklightActiveHigh = true;
constexpr uint8_t kLcdMemoryAccess = 0xA0; // MADCTL: MY | MV, RGB; USB on the left.
constexpr int kLcdClockHz = 20'000'000;

constexpr int kTouchClock = 25;
constexpr int kTouchMosi = 32;
constexpr int kTouchMiso = 39;
constexpr int kTouchSelect = 33;
constexpr int kTouchInterrupt = 36;
constexpr bool kTouchSwapXY = true;
constexpr int kTouchXStart = 3800;
constexpr int kTouchXEnd = 200;
constexpr int kTouchYStart = 200;
constexpr int kTouchYEnd = 3800;
static_assert(kTouchXStart != kTouchXEnd && kTouchYStart != kTouchYEnd);

inline int touch_coordinate(int raw, int start, int end, int size) {
    return std::clamp((raw - start) * (size - 1) / (end - start), 0, size - 1);
}

} // namespace board
