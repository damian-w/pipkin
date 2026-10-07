// https://pipkin.io/start; QR version 3, correction H, generated with macOS CoreImage.
#pragma once
#include <cstdint>
namespace pipkin::setup_qr {
constexpr int kSize = 29;
// Rows run top to bottom; bit x is column x, 1 is dark. Renderer supplies the quiet zone.
inline constexpr uint32_t kRows[] = {
    0x1FDF407Fu,
    0x1041C541u,
    0x174E5B5Du,
    0x1757B75Du,
    0x17457D5Du,
    0x10574B41u,
    0x1FD5557Fu,
    0x0003E000u,
    0x0FA375E4u,
    0x1EF83CBDu,
    0x123DB258u,
    0x01CE6F88u,
    0x0A7B6DD6u,
    0x12586820u,
    0x1679CBE7u,
    0x0A60E02Eu,
    0x08CFD4CAu,
    0x167CD9B2u,
    0x1209A5F3u,
    0x1BE7001Cu,
    0x13F87B5Bu,
    0x1318F700u,
    0x1751657Fu,
    0x1B1E0541u,
    0x09FDFA5Du,
    0x0D9D2C5Du,
    0x1D27015Du,
    0x02346041u,
    0x13F4927Fu,
};
} // namespace pipkin::setup_qr
