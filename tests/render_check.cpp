#include "pipkin/render.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>

int main() {
    constexpr std::size_t count = pipkin::kDisplayWidth * pipkin::kDisplayHeight;
    std::array<uint16_t, count + 2> guarded{};
    guarded.front() = guarded.back() = 0xbeef;
    pipkin::State state;
    for (int page = 0; page < 3; ++page) {
        pipkin::restore_page(state, static_cast<pipkin::Page>(page));
        for (int status = 0; status < 5; ++status) {
            for (auto& provider : state.providers) {
                provider.session.state = static_cast<pipkin::AllowanceState>(status);
                provider.weekly.state = static_cast<pipkin::AllowanceState>(status);
                provider.session.used_tenths = provider.weekly.used_tenths = 1000;
                provider.banked_resets = 1000000;
                provider.session.duration_seconds = 31622400;
                provider.weekly.duration_seconds = 604800;
            }
            pipkin::render(state, 0, guarded.data() + 1);
            assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);
            assert(std::adjacent_find(guarded.begin() + 1, guarded.end() - 1,
                                      std::not_equal_to<uint16_t>()) != guarded.end() - 1);
        }
    }
    state.overlay = pipkin::Overlay::Sleep;
    pipkin::render(state, 0, guarded.data() + 1);
    assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);

    auto frame = [&](const pipkin::State& reading, uint64_t now) {
        std::vector<uint16_t> pixels(count);
        pipkin::render(reading, now, pixels.data());
        return pixels;
    };
    state = {};
    state.clock.anchor_unix = 1800000000;
    state.transport_connected = true;
    state.host = pipkin::HostState::Awake;
    state.host_received_ms = 0;
    state.page = pipkin::Page::Codex;
    auto& provider = state.providers[0];
    for (auto* window : {&provider.session, &provider.weekly}) {
        window->state = pipkin::AllowanceState::Metered;
        window->used_tenths = 0;
        window->freshness.observed_unix = 1800000000;
        window->freshness.age_on_receipt_ms = 0;
    }
    const auto full = frame(state, 1000);
    provider.banked_resets = 0;
    assert(frame(state, 1000) == full);
    provider.banked_resets = 1;
    assert(frame(state, 1000) != full);
    provider.banked_resets = 0;
    provider.session.used_tenths = provider.weekly.used_tenths = 1000;
    const auto empty = frame(state, 1000);
    assert(full[78 * pipkin::kDisplayWidth + 84] != empty[78 * pipkin::kDisplayWidth + 84]);
    provider.session.used_tenths = provider.weekly.used_tenths = 0;
    state.page_entered_ms = 1000;
    state.view_revision = 1;
    assert(pipkin::animation_active(state, 1000));
    const auto start = frame(state, 1000);
    for (int y = 60; y < 100; ++y)
        for (int x : {30, 84, 138, 182, 236, 290}) {
            const auto i = y * pipkin::kDisplayWidth + x;
            assert(start[i] == empty[i]);
        }
    assert(start != empty);
    assert(frame(state, 1300) != empty && frame(state, 1300) != full);
    assert(!pipkin::animation_active(state, 1650));
    assert(frame(state, 1650) == full);
    assert(*provider.session.used_tenths == 0);

    provider.weekly.from_used_tenths = 1000;
    provider.weekly.changed_ms = 2000;
    assert(pipkin::animation_active(state, 2300) && !pipkin::animation_active(state, 2650));
    assert(frame(state, 2300) != full && frame(state, 2650) == full);
    provider.weekly.from_used_tenths.reset();

    const auto unlabelled = frame(state, 1000);
    provider.session.duration_seconds = 5 * 3600;
    assert(frame(state, 1000) == unlabelled);
    provider.session.duration_seconds = 3 * 3600;
    assert(frame(state, 1000) != unlabelled);
    provider.session.duration_seconds.reset();

    state.view_revision = 0;
    assert(frame(state, 0) == frame(state, 1000));
    const auto next_minute = frame(state, 60000);
    for (int y = 40; y < pipkin::kDisplayHeight; ++y)
        for (int x = 0; x < pipkin::kDisplayWidth; ++x) {
            const auto i = y * pipkin::kDisplayWidth + x;
            assert(next_minute[i] == full[i]);
        }
    assert(next_minute != full);

    auto banded = [&](const pipkin::State& reading, uint64_t now, int rows) {
        std::vector<uint16_t> pixels(count);
        for (int top = 0; top < pipkin::kDisplayHeight; top += rows)
            pipkin::render(reading, now, pixels.data() + top * pipkin::kDisplayWidth, top,
                           std::min(rows, pipkin::kDisplayHeight - top));
        return pixels;
    };
    state.providers[1] = provider;
    state.providers[1].weekly.used_tenths = 870;
    for (auto page : {pipkin::Page::Overview, pipkin::Page::Codex, pipkin::Page::Claude}) {
        state.page = page;
        for (int rows : {1, 7, 16, 240})
            assert(banded(state, 1000, rows) == frame(state, 1000));
    }
    state.providers[1] = {};
    state.page = pipkin::Page::Codex;
    pipkin::render(state, 0, guarded.data() + 1, 200, 41);
    assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);

    provider.session = {};
    provider.weekly.used_tenths = 250;
    const auto weekly_only = frame(state, 1000);
    provider.session.state = pipkin::AllowanceState::NoCap;
    const auto no_cap = frame(state, 1000);
    assert(no_cap != weekly_only);
    provider.session.state = pipkin::AllowanceState::Unsupported;
    assert(frame(state, 1000) != weekly_only && frame(state, 1000) != no_cap);
    provider.session.state = pipkin::AllowanceState::Unknown;
    state.about_open = true;
    state.device_info = {"0.1.0", "Uncommitted"};
    const auto unknown_session_about = frame(state, 1000);
    provider.session.state = pipkin::AllowanceState::NoCap;
    assert(frame(state, 1000) != unknown_session_about);
    assert(!pipkin::animation_active(state, 1000));
    const auto observed_about = frame(state, 1000);
    state.about_open = false;
    provider.weekly.freshness.observed_unix.reset();
    provider.weekly.freshness.age_on_receipt_ms.reset();
    assert(frame(state, 1000) == no_cap);
    state.about_open = true;
    assert(frame(state, 1000) != observed_about);
    provider.weekly.state = pipkin::AllowanceState::Unsupported;
    provider.session.state = pipkin::AllowanceState::Metered;
    provider.session.used_tenths = 500;
    provider.session.freshness.observed_unix = 1800000000;
    provider.session.freshness.age_on_receipt_ms = 0;
    const auto observed_session = frame(state, 1000);
    provider.session.freshness.observed_unix.reset();
    provider.session.freshness.age_on_receipt_ms.reset();
    assert(frame(state, 1000) != observed_session);
    const auto without_claude = frame(state, 1000);
    state.providers[1].app = pipkin::AppState::Unsupported;
    assert(frame(state, 1000) != without_claude);
    pipkin::render(state, 1000, guarded.data() + 1);
    assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);

    pipkin::State idle;
    idle.clock.anchor_unix = 1800000000;
    idle.transport_connected = true;
    auto& session = idle.providers[1].session;
    idle.providers[1].weekly.state = pipkin::AllowanceState::Metered;
    idle.providers[1].weekly.used_tenths = 10;
    for (bool paired : {false, true}) {
        if (paired)
            idle.providers[0].weekly = idle.providers[1].weekly;
        for (auto page : {pipkin::Page::Overview, pipkin::Page::Claude}) {
            idle.page = page;
            session = {};
            const auto unknown = frame(idle, 1000);
            session.state = pipkin::AllowanceState::NotStarted;
            const auto not_started = frame(idle, 1000);
            if (page == pipkin::Page::Overview) {
                const int top = paired ? 208 : 149;
                for (int y = top; y < top + 12; ++y)
                    for (int x = 16; x < 152; ++x)
                        assert(not_started[y * pipkin::kDisplayWidth + x] == not_started[0]);
            }
            session.state = pipkin::AllowanceState::Metered;
            session.used_tenths = 0;
            assert(not_started != unknown && not_started != frame(idle, 1000));
        }
    }
    idle.about_open = true;
    const auto started_about = frame(idle, 1000);
    session = {};
    session.state = pipkin::AllowanceState::NotStarted;
    assert(frame(idle, 1000) != started_about);

    state = {};
    pipkin::start_boot(state, 0);
    const auto blank = frame(state, 0);
    assert(pipkin::animation_active(state, 0) && pipkin::animation_active(state, 3000));
    assert(frame(state, 1000) != blank && frame(state, 2000) != frame(state, 1000));
    assert(!pipkin::animation_active(state, 5000) && frame(state, 5000) == frame(state, 6000));
    assert(pipkin::animation_active(state, 7200) && frame(state, 7200) != frame(state, 6000));
    assert(frame(state, 9000) != frame(state, 6000));
    constexpr int qr_corner = 8 * pipkin::kDisplayWidth + 104;
    assert(frame(state, 17999)[qr_corner] == blank[qr_corner]);
    assert(frame(state, 18000)[qr_corner] == 0xffff);
    for (uint64_t now : {17999, 18000, 24000}) {
        pipkin::render(state, now, guarded.data() + 1);
        assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);
        for (int rows : {1, 7, 16, 240})
            assert(banded(state, now, rows) == frame(state, now));
    }
    state.transport_connected = true;
    assert(frame(state, 18000)[qr_corner] == blank[qr_corner]);
    assert(frame(state, 9000) != frame(state, 6000) && frame(state, 3000) != blank);
    for (uint64_t now : {0, 1000, 2000, 3000, 9000, 18000})
        for (int rows : {1, 7, 16, 240})
            assert(banded(state, now, rows) == frame(state, now));
    state.transport_connected = false;
    state.boot.ready_ms = 18500;
    assert(frame(state, 18500)[qr_corner] == blank[qr_corner]);
    assert(pipkin::animation_active(state, 18500));
    for (uint64_t now = 18500; now < 18500 + pipkin::kBootFinishMs; now += 97) {
        pipkin::render(state, now, guarded.data() + 1);
        assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);
        assert(banded(state, now, 16) == frame(state, now));
    }
    assert(frame(state, 18500 + pipkin::kBootFinishMs) == blank);
    state.overlay = pipkin::Overlay::Sleep;
    assert(!pipkin::animation_active(state, 1000) && frame(state, 1000) == frame(state, 2000));
    std::puts("Render bounds, remaining, reset visibility, reveal, clock, About and boot checks "
              "passed");
}
