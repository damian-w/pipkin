#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pipkin {

constexpr std::size_t kMaxPacketBytes = 768;
constexpr uint64_t kStaleAfterMs = 5 * 60 * 1000;
constexpr uint64_t kHostLivenessMs = 60 * 1000;
constexpr uint64_t kTransitionMs = 650;
constexpr uint64_t kStatusIdleMs = 60 * 1000;
constexpr uint64_t kBootIntroMs = 1440;
constexpr uint64_t kBootFinishMs = 4000;
// Dim/off delays start when the latest reading becomes stale.
constexpr uint64_t kDimAfterStaleMs = 5 * 60 * 1000;
constexpr uint64_t kOffAfterStaleMs = 15 * 60 * 1000;
constexpr uint64_t kTouchWakeMs = 60 * 1000;
constexpr uint8_t kDimLevel = 20;

enum class Provider : uint8_t { Codex, Claude };
enum class Page : uint8_t { Overview, Codex, Claude, About };
enum class AllowanceState : uint8_t { Unknown, Metered, NoCap, NotStarted, Unsupported };
enum class AppState : uint8_t { Unknown, Available, Unavailable, Unsupported };
enum class HostState : uint8_t { Unknown, Awake, Asleep, Disconnected };
enum class Overlay : uint8_t { None, Sleep };

struct Freshness {
    std::optional<int64_t> observed_unix;
    uint64_t received_ms = 0;
    std::optional<uint64_t> age_on_receipt_ms;
    uint64_t observation_epoch = 0;
    // Preserve ordering and age across an explicitly unknown observation.
    std::optional<int64_t> last_observed_unix;
    std::optional<uint64_t> last_observed_age_ms;

    std::optional<uint64_t> observation_age_ms(uint64_t now_ms) const;
    bool stale(uint64_t now_ms) const;
};

struct Window {
    AllowanceState state = AllowanceState::Unknown;
    std::array<char, 33> id{};
    std::optional<uint16_t> used_tenths;
    std::optional<int64_t> reset_unix;
    std::optional<uint32_t> duration_seconds;
    Freshness freshness;
    // Animate from the value on screen when an update arrives.
    std::optional<uint16_t> from_used_tenths;
    uint64_t changed_ms = 0;
};

struct ProviderData {
    std::array<char, 33> account{};
    AppState app = AppState::Unknown;
    uint64_t app_received_ms = 0;
    Window session;
    Window weekly;
    std::optional<uint32_t> banked_resets;
    Freshness banked_freshness;
};

struct Clock {
    std::optional<int64_t> anchor_unix;
    uint64_t anchor_ms = 0;
    int16_t utc_offset_minutes = 0;
    uint64_t observation_epoch = 0;
};

struct DeviceInfo {
    // Static firmware metadata; never populated from usage packets.
    std::string_view firmware_version = "Unknown";
    std::string_view git_revision = "Unknown";
};

struct TouchGesture {
    bool active = false;
    bool moved = false;
    bool consumed = false;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;
    int16_t last_y = 0;
    uint64_t started_ms = 0;
};

struct Boot {
    bool active = false;
    uint64_t started_ms = 0;
    std::optional<uint64_t> ready_ms;
};

struct State {
    std::array<ProviderData, 2> providers{};
    Boot boot;
    Clock clock;
    HostState host = HostState::Unknown;
    std::optional<uint64_t> host_received_ms;
    bool transport_connected = false;
    Overlay overlay = Overlay::None;
    Page page = Page::Overview;
    bool about_open = false;
    DeviceInfo device_info;
    TouchGesture gesture;
    uint64_t page_entered_ms = 0;
    uint64_t touched_ms = 0;
    uint64_t view_revision = 0;
    uint64_t last_sequence = 0;
    uint64_t last_received_ms = 0;
};

struct VisiblePages {
    std::array<Page, 3> pages{};
    uint8_t count = 0;
};

std::optional<int64_t> unix_now(const State& state, uint64_t now_ms);
bool host_alive(const State& state, uint64_t now_ms);
double transition(uint64_t start_ms, uint64_t now_ms);
uint16_t shown_used(const Window& window, uint64_t now_ms);
bool changing(const Window& window, uint64_t now_ms);
bool ingest(State& state, std::string_view line, uint64_t now_ms);
bool provider_visible(const State& state, Provider provider);
VisiblePages visible_pages(const State& state);
bool page_visible(const State& state, Page page);
Page effective_page(const State& state);
void restore_page(State& state, Page page, uint64_t now_ms = 0);
Page selected_page(const State& state);
void tap(State& state, int x, int y, uint64_t now_ms);
void swipe(State& state, int direction, uint64_t now_ms);
void touch(State& state, bool pressed, int x, int y, uint64_t now_ms);
void start_boot(State& state, uint64_t now_ms);
// Finish after both the intro and the first reading.
std::optional<uint64_t> boot_finish_ms(const Boot& boot);
void idle(State& state, uint64_t now_ms);
// Observation age when known, otherwise time since receipt; none before the first reading.
std::optional<uint64_t> newest_reading_age_ms(const State& state, uint64_t now_ms);
uint8_t backlight_level(const State& state, uint64_t now_ms);

} // namespace pipkin
