#pragma once

#include <algorithm>
#include <cstdint>

namespace board {

// One panel command with up to four parameter bytes, then a pause.
struct LcdCommand {
    uint8_t command;
    uint8_t length;
    uint8_t data[4];
    uint16_t delay_ms;
};

#if PIPKIN_BOARD_ES3C40P
#if !PIPKIN_DISPLAY_480X320
#error "The ES3C40P has a 480 x 320 panel"
#endif

// LCDWIKI ES3C40P: ESP32-S3 with native USB, 4.0-inch ST7796S IPS LCD, FT6336U capacitive touch.
constexpr char kProfile[] = "es3c40p";
constexpr int kLcdClock = 12;
constexpr int kLcdMosi = 11;
constexpr int kLcdMiso = 13;
constexpr int kLcdSelect = 10;
constexpr int kLcdDataCommand = 46;
constexpr int kBacklight = 45;
constexpr bool kBacklightActiveHigh = true;
constexpr int kLcdClockHz = 40'000'000;
// The panel resets with the ESP32-S3. MADCTL MY | MX | MV | BGR turns the vendor's landscape
// (MV | BGR) 180 degrees, putting the USB-C port on the left; IPS needs inversion on.
constexpr LcdCommand kLcdInit[] = {
    {0x01, 0, {}, 150},       // Software reset
    {0x11, 0, {}, 120},       // Sleep out
    {0x3A, 1, {0x55}, 0},     // 16-bit colour
    {0x36, 1, {0xE8}, 0},     // MADCTL
    {0x21, 0, {}, 0},         // Inversion on
    {0x29, 0, {}, 0},         // Display on
};

// FT6336U on I2C, shared with the audio codec. It reports portrait coordinates.
constexpr int kTouchSda = 16;
constexpr int kTouchScl = 15;
constexpr int kTouchReset = 18;
constexpr uint8_t kTouchAddress = 0x38;
constexpr int kTouchWidth = 320;
constexpr int kTouchHeight = 480;

// Portrait touch (x, y) to the landscape panel set by MADCTL above.
inline void touch_to_panel(int raw_x, int raw_y, int& x, int& y) {
    x = kTouchHeight - 1 - std::clamp(raw_y, 0, kTouchHeight - 1);
    y = std::clamp(raw_x, 0, kTouchWidth - 1);
}

#else
#if PIPKIN_DISPLAY_480X320
#error "The ESP32-2432S028R has a 320 x 240 panel"
#endif

// ESP32-2432S028R ("Cheap Yellow Display") pins, panel and touch calibration.
constexpr char kProfile[] = "esp32-2432s028r";
constexpr int kLcdClock = 14;
constexpr int kLcdMosi = 13;
constexpr int kLcdMiso = 12;
constexpr int kLcdSelect = 15;
constexpr int kLcdDataCommand = 2;
constexpr int kBacklight = 21;
constexpr bool kBacklightActiveHigh = true;
constexpr int kLcdClockHz = 20'000'000;
constexpr LcdCommand kLcdInit[] = {
    {0x01, 0, {}, 150},       // Software reset
    {0x11, 0, {}, 120},       // Sleep out
    {0x3A, 1, {0x55}, 0},     // 16-bit colour
    {0x36, 1, {0xA0}, 0},     // MADCTL: MY | MV, RGB; USB on the left.
    {0x29, 0, {}, 0},         // Display on
};

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
#endif

} // namespace board
