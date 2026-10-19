#include "board.h"
#include "display.h"
#include "line_reader.h"
#include "link.h"
#include "pipkin/demo.h"
#include "pipkin/model.h"
#include "pipkin/render.h"

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

namespace {

// The demo reel build plays the product tour and only answers identify.
constexpr bool kDemo = PIPKIN_DEMO;

constexpr int kBands = pipkin::kDisplayHeight / kDisplayBandRows;
static_assert(pipkin::kDisplayHeight % kDisplayBandRows == 0);

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

uint32_t band_hash(const uint16_t* pixels) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < pipkin::kDisplayWidth * kDisplayBandRows; ++i)
        hash = (hash ^ pixels[i]) * 16777619u;
    return hash;
}

// Renders band by band and transmits only bands whose content changed since the last frame.
struct Presenter {
    uint16_t* band = nullptr;
    std::array<uint32_t, kBands> hashes{};
    bool initialized = false;

    void present(const pipkin::State& state, uint64_t now) {
        for (int i = 0; i < kBands; ++i) {
            const int top = i * kDisplayBandRows;
            pipkin::render(state, now, band, top, kDisplayBandRows);
            const auto hash = band_hash(band);
            if (initialized && hash == hashes[i])
                continue;
            hashes[i] = hash;
            display_rows(top, band);
        }
        initialized = true;
    }
};

void identity(const pipkin::State& state, uint64_t now) {
    char unix_time[24] = "null";
    if (const auto time = pipkin::unix_now(state, now))
        std::snprintf(unix_time, sizeof(unix_time), "%" PRId64, *time);
    char reply[256];
    const int count = std::snprintf(
        reply, sizeof(reply),
        "v=" PIPKIN_PROTOCOL " kind=identity product=pipkin firmware=%s board=%s seq=%" PRIu64
        " clock_epoch=%" PRIu64 " unix=%s\n",
        esp_app_get_description()->version, board::kProfile, state.last_sequence,
        state.clock.observation_epoch, unix_time);
    if (count > 0 && count < static_cast<int>(sizeof(reply)))
        link_write(reply, count);
}

} // namespace

extern "C" void app_main() {
    Presenter presenter;
    presenter.band = static_cast<uint16_t*>(heap_caps_malloc(
        pipkin::kDisplayWidth * kDisplayBandRows * sizeof(uint16_t), MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK(presenter.band ? ESP_OK : ESP_ERR_NO_MEM);
    pipkin::State state;
    state.device_info.firmware_version = esp_app_get_description()->version;
    state.device_info.git_revision = PIPKIN_GIT_REVISION;
    nvs_handle_t settings = 0;
    // A damaged or newer settings partition is preserved for recovery. The demo saves nothing.
    if (!kDemo && nvs_flash_init() == ESP_OK &&
        nvs_open("pipkin", NVS_READWRITE, &settings) == ESP_OK) {
        uint8_t page = 0;
        if (nvs_get_u8(settings, "page", &page) == ESP_OK) {
            pipkin::restore_page(state, static_cast<pipkin::Page>(page));
        }
    }
    auto saved_page = state.page;

    link_init();

    display_init();
    pipkin::start_boot(state, now_ms());
    pipkin::LineReader reader;
    uint64_t rendered_ms = 0;
    bool dirty = true;
    int backlight = 0;
    bool panel_on = true;
    uint64_t faded_ms = now_ms();

    for (;;) {
        const uint64_t now = now_ms();
        char bytes[512];
        bool lost = false;
        const int count = link_read(bytes, sizeof(bytes), pdMS_TO_TICKS(20), lost);
        if (lost)
            reader.overflow = true;
        for (int i = 0; i < count; ++i) {
            reader.push(bytes[i], [&](std::string_view line) {
                if (line == pipkin::kIdentifyRequest) {
                    identity(state, now);
                } else if (!kDemo && pipkin::ingest(state, line, now)) {
                    dirty = true;
                }
            });
        }

        int x = 0;
        int y = 0;
        const bool pressed = touch_read(x, y);
        const auto previous_view = pipkin::effective_page(state);
        pipkin::touch(state, pressed, x, y, now);
        if (kDemo)
            pipkin::play_demo(state, now);
        pipkin::idle(state, now);
        dirty = dirty || pipkin::effective_page(state) != previous_view;
        const auto page = state.page;
        if (page != saved_page) {
            if (settings != 0 &&
                (nvs_set_u8(settings, "page", static_cast<uint8_t>(page)) != ESP_OK ||
                 nvs_commit(settings) != ESP_OK)) {
                nvs_close(settings);
                settings = 0;
            }
            saved_page = page;
        }

        const int target = pipkin::backlight_level(state, now) * 10;
        const bool awake = target > 0;
        if (awake && !panel_on) {
            display_power(true);
            panel_on = true;
        }
        const bool frame_due =
            pipkin::animation_active(state, now)
                ? now - rendered_ms >= 33
                : pipkin::animation_active(state, rendered_ms) || now / 1000 != rendered_ms / 1000;
        if (awake && (dirty || frame_due)) {
            presenter.present(state, now);
            rendered_ms = now;
            dirty = false;
        }
        // Fades toward the model's level at a full-range sweep per 400 ms.
        if (backlight != target) {
            const int step = std::max(1, static_cast<int>((now - faded_ms) * 1000 / 400));
            backlight = target > backlight ? std::min(target, backlight + step)
                                           : std::max(target, backlight - step);
            display_backlight(backlight);
        }
        // Once the backlight is off, stop showing the frame until it wakes.
        if (!awake && backlight == 0 && panel_on) {
            display_power(false);
            panel_on = false;
        }
        faded_ms = now;
    }
}
