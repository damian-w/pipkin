#include "pipkin/model.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace pipkin {

std::optional<uint64_t> Freshness::observation_age_ms(uint64_t now_ms) const {
    if (!age_on_receipt_ms || now_ms < received_ms)
        return std::nullopt;
    const uint64_t elapsed = now_ms - received_ms;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return elapsed > maximum - *age_on_receipt_ms ? maximum : elapsed + *age_on_receipt_ms;
}

bool Freshness::stale(uint64_t now_ms) const {
    const auto age = observation_age_ms(now_ms);
    return !age || *age >= kStaleAfterMs;
}

std::optional<int64_t> unix_now(const State& state, uint64_t now_ms) {
    if (!state.clock.anchor_unix || now_ms < state.clock.anchor_ms)
        return std::nullopt;
    const uint64_t elapsed = (now_ms - state.clock.anchor_ms) / 1000;
    constexpr int64_t last_supported_second = 4102444800;
    if (elapsed > static_cast<uint64_t>(last_supported_second - *state.clock.anchor_unix))
        return std::nullopt;
    return *state.clock.anchor_unix + static_cast<int64_t>(elapsed);
}

double transition(uint64_t start_ms, uint64_t now_ms) {
    if (now_ms < start_ms || now_ms - start_ms >= kTransitionMs)
        return 1;
    const double left = 1.0 - static_cast<double>(now_ms - start_ms) / kTransitionMs;
    return 1.0 - left * left * left;
}

bool changing(const Window& window, uint64_t now_ms) {
    return window.from_used_tenths && window.used_tenths && now_ms >= window.changed_ms &&
           now_ms - window.changed_ms < kTransitionMs;
}

uint16_t shown_used(const Window& window, uint64_t now_ms) {
    if (!window.used_tenths)
        return 0;
    if (!changing(window, now_ms))
        return *window.used_tenths;
    const double from = *window.from_used_tenths;
    return static_cast<uint16_t>(
        from + (*window.used_tenths - from) * transition(window.changed_ms, now_ms) + 0.5);
}

bool host_alive(const State& state, uint64_t now_ms) {
    return state.transport_connected && state.host == HostState::Awake && state.host_received_ms &&
           now_ms >= *state.host_received_ms && now_ms - *state.host_received_ms < kHostLivenessMs;
}

bool provider_visible(const State& state, Provider provider) {
    const auto index = static_cast<std::size_t>(provider);
    if (index >= state.providers.size())
        return false;
    const auto& data = state.providers[index];
    const auto usable = [](const Window& window) {
        return (window.state == AllowanceState::Metered && window.used_tenths.has_value()) ||
               window.state == AllowanceState::NoCap || window.state == AllowanceState::NotStarted;
    };
    return usable(data.session) || usable(data.weekly) || data.banked_resets.value_or(0) > 0;
}

VisiblePages visible_pages(const State& state) {
    VisiblePages pages{{{Page::Overview}}, 1};
    if (provider_visible(state, Provider::Codex))
        pages.pages[pages.count++] = Page::Codex;
    if (provider_visible(state, Provider::Claude))
        pages.pages[pages.count++] = Page::Claude;
    return pages;
}

bool page_visible(const State& state, Page page) {
    const auto pages = visible_pages(state);
    for (uint8_t i = 0; i < pages.count; ++i)
        if (pages.pages[i] == page)
            return true;
    return false;
}

Page effective_page(const State& state) {
    return state.about_open ? Page::About : selected_page(state);
}

void restore_page(State& state, Page page, uint64_t now_ms) {
    if (page != Page::Overview && page != Page::Codex && page != Page::Claude)
        return;
    const auto previous = effective_page(state);
    state.page = page;
    if (effective_page(state) != previous) {
        state.page_entered_ms = now_ms;
        ++state.view_revision;
    }
}

Page selected_page(const State& state) {
    return page_visible(state, state.page) ? state.page : visible_pages(state).pages[0];
}

namespace {

constexpr int kTapMovementPx = 12;
constexpr int kHoldMovementPx = 25;

void show_about(State& state, bool open, uint64_t now_ms) {
    if (state.about_open != open) {
        state.about_open = open;
        state.page_entered_ms = now_ms;
        ++state.view_revision;
    }
}

void finish_touch(State& state, uint64_t now_ms) {
    const auto& gesture = state.gesture;
    if (!gesture.consumed) {
        const int dx = gesture.last_x - gesture.start_x;
        const int dy = gesture.last_y - gesture.start_y;
        if (std::abs(dx) >= 60 && std::abs(dx) >= 2 * std::abs(dy))
            swipe(state, dx < 0 ? 1 : -1, now_ms);
        else if (!gesture.moved)
            tap(state, gesture.last_x, gesture.last_y, now_ms);
    }
    state.gesture = TouchGesture{};
}

} // namespace

void tap(State& state, int x, int y, uint64_t now_ms) {
    if (state.overlay != Overlay::None || x < 0 || x >= 320 || y < 0 || y >= 240)
        return;
    if (state.about_open) {
        show_about(state, false, now_ms);
        return;
    }
    const auto page = effective_page(state);
    const auto pages = visible_pages(state);
    if (pages.count == 1)
        return;
    if (y >= 208)
        swipe(state, x < 160 ? -1 : 1, now_ms);
    else if (page == Page::Overview && y >= 32 && y < (pages.count == 2 ? 208 : 123))
        restore_page(state, pages.pages[1], now_ms);
    else if (page == Page::Overview && pages.count == 3 && y >= 123 && y < 208)
        restore_page(state, pages.pages[2], now_ms);
}

void swipe(State& state, int direction, uint64_t now_ms) {
    if (state.overlay != Overlay::None || (direction != -1 && direction != 1))
        return;
    if (state.about_open) {
        show_about(state, false, now_ms);
        return;
    }
    const auto pages = visible_pages(state);
    if (pages.count == 1)
        return;
    const auto page = selected_page(state);
    for (int i = 0; i < pages.count; ++i) {
        if (pages.pages[static_cast<std::size_t>(i)] == page) {
            const auto next = static_cast<std::size_t>((i + direction + pages.count) % pages.count);
            restore_page(state, pages.pages[next], now_ms);
            return;
        }
    }
}

void touch(State& state, bool pressed, int x, int y, uint64_t now_ms) {
    auto& gesture = state.gesture;
    if (state.overlay != Overlay::None || (pressed && (x < 0 || x >= 320 || y < 0 || y >= 240)) ||
        (gesture.active && now_ms < gesture.started_ms) ||
        (gesture.release_started_ms && now_ms < *gesture.release_started_ms)) {
        gesture = TouchGesture{};
        return;
    }
    // Confirm release before handling a new press, so a longer gap starts a fresh hold.
    if (gesture.release_started_ms &&
        now_ms - *gesture.release_started_ms >= kTouchReleaseGraceMs)
        finish_touch(state, now_ms);
    if (pressed && !gesture.active) {
        // Consume the first touch when waking a dark screen.
        const bool dark = backlight_level(state, now_ms) == 0;
        gesture = TouchGesture{};
        gesture.active = true;
        gesture.consumed = dark;
        gesture.start_x = gesture.last_x = static_cast<int16_t>(x);
        gesture.start_y = gesture.last_y = static_cast<int16_t>(y);
        gesture.started_ms = now_ms;
        state.touched_ms = now_ms;
        return;
    }
    if (!gesture.active)
        return;
    if (!pressed) {
        // A brief contact dropout keeps the gesture; idle time starts at the first release.
        if (!gesture.release_started_ms) {
            gesture.release_started_ms = now_ms;
            state.touched_ms = now_ms;
        }
        return;
    }

    gesture.release_started_ms.reset();
    state.touched_ms = now_ms;
    gesture.last_x = static_cast<int16_t>(x);
    gesture.last_y = static_cast<int16_t>(y);
    const int dx = std::abs(x - gesture.start_x);
    const int dy = std::abs(y - gesture.start_y);
    gesture.moved = gesture.moved || dx > kTapMovementPx || dy > kTapMovementPx;
    gesture.hold_moved = gesture.hold_moved || dx > kHoldMovementPx || dy > kHoldMovementPx;
    // Only open on confirmed contact, never while waiting for release confirmation.
    if (!gesture.consumed && !gesture.hold_moved && !state.about_open &&
        now_ms - gesture.started_ms >= kAboutHoldMs) {
        show_about(state, true, now_ms);
        gesture.consumed = true;
    }
}

void start_boot(State& state, uint64_t now_ms) { state.boot = {true, now_ms, std::nullopt}; }

std::optional<uint64_t> boot_finish_ms(const Boot& boot) {
    if (!boot.ready_ms)
        return std::nullopt;
    return std::max(*boot.ready_ms, boot.started_ms + kBootIntroMs);
}

void idle(State& state, uint64_t now_ms) {
    if (state.about_open && !state.gesture.active && now_ms >= state.touched_ms + kStatusIdleMs)
        show_about(state, false, now_ms);
    auto& boot = state.boot;
    if (!boot.active)
        return;
    if (!boot.ready_ms &&
        (provider_visible(state, Provider::Codex) || provider_visible(state, Provider::Claude)))
        boot.ready_ms = now_ms;
    if (const auto finish = boot_finish_ms(boot); finish && now_ms >= *finish + kBootFinishMs) {
        boot.active = false;
        state.page_entered_ms = now_ms;
        ++state.view_revision;
    }
}

std::optional<uint64_t> newest_reading_age_ms(const State& state, uint64_t now_ms) {
    std::optional<uint64_t> newest;
    for (const auto& provider : state.providers)
        for (const auto* window : {&provider.session, &provider.weekly}) {
            const auto& freshness = window->freshness;
            if (window->state == AllowanceState::Unknown || now_ms < freshness.received_ms)
                continue;
            const uint64_t age =
                freshness.observation_age_ms(now_ms).value_or(now_ms - freshness.received_ms);
            if (!newest || age < *newest)
                newest = age;
        }
    return newest;
}

uint8_t backlight_level(const State& state, uint64_t now_ms) {
    if (state.overlay == Overlay::Sleep)
        return 0;
    if (state.touched_ms && now_ms >= state.touched_ms && now_ms - state.touched_ms < kTouchWakeMs)
        return 100;
    const auto age = newest_reading_age_ms(state, now_ms);
    if (!age || *age < kStaleAfterMs + kDimAfterStaleMs)
        return 100;
    return *age < kStaleAfterMs + kOffAfterStaleMs ? kDimLevel : 0;
}

} // namespace pipkin
