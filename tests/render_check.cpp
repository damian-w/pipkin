#include "pipkin/render.h"
#include "../src/setup_qr_data.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <vector>

// Layout to panel pixels, as the renderer rounds them.
int panel(int v) {
    return (v * pipkin::kScaleNumerator * 2 + pipkin::kScaleDenominator) /
           (2 * pipkin::kScaleDenominator);
}
// Index of the panel pixel at layout (x, y).
int at(int x, int y) { return panel(y) * pipkin::kDisplayWidth + panel(x); }

int main() {
    constexpr std::size_t count = pipkin::kDisplayWidth * pipkin::kDisplayHeight;
    // Paired dial centres on provider pages.
    constexpr int dial_offset = 76 * pipkin::kLayoutWidth / 320;
    constexpr int dials[] = {pipkin::kLayoutWidth / 2 - dial_offset,
                             pipkin::kLayoutWidth / 2 + dial_offset};
    constexpr int right = pipkin::kLayoutWidth - 16;
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
    state.host = pipkin::HostState::Asleep;
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
    assert(full[at(dials[0], 78)] != empty[at(dials[0], 78)]);
    provider.session.used_tenths = provider.weekly.used_tenths = 0;
    state.page_entered_ms = 1000;
    state.view_revision = 1;
    assert(pipkin::animation_active(state, 1000));
    const auto start = frame(state, 1000);
    for (int y = 60; y < 100; ++y)
        for (int dial : dials)
            for (int x : {dial - 54, dial, dial + 54}) {
                const auto i = at(x, y);
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

    // Changed pixels inside and outside the layout rectangle from (left, top) to the right edge.
    auto changes = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, int top,
                      int bottom, int left) {
        top = panel(top);
        bottom = panel(bottom);
        left = panel(left);
        std::array<int, 2> changed{};
        for (int y = 0; y < pipkin::kDisplayHeight; ++y)
            for (int x = 0; x < pipkin::kDisplayWidth; ++x) {
                const auto i = y * pipkin::kDisplayWidth + x;
                if (a[i] != b[i])
                    ++changed[y >= top && y < bottom && x >= left ? 0 : 1];
            }
        return changed;
    };
    // The clock shows only on About and the sleep overlay.
    state.view_revision = 0;
    assert(frame(state, 0) == frame(state, 1000));
    assert(frame(state, 60000) == full);
    state.page = pipkin::Page::Overview;
    const auto overview = frame(state, 1000);
    assert(frame(state, 60000) == overview);
    state.about_open = true;
    assert(changes(frame(state, 1000), frame(state, 60000), 0, 30, right - 54)[0] > 0);
    state.about_open = false;

    // Task dots take the clock's corner on provider pages and join the overview heading.
    auto& tasks = state.tasks[0];
    tasks.coverage = pipkin::TaskCoverage::Partial;
    tasks.freshness.observed_unix = 1800000000;
    tasks.freshness.age_on_receipt_ms = 0;
    assert(frame(state, 1000) == overview);
    tasks.working = 2;
    tasks.attention = 1;
    const auto overview_dots = changes(overview, frame(state, 1000), 56, 72, right - 54);
    assert(overview_dots[0] > 0 && overview_dots[1] == 0);
    state.page = pipkin::Page::Codex;
    const auto dots = frame(state, 1000);
    const auto corner = changes(full, dots, 0, 30, right - 54);
    assert(corner[0] > 0 && corner[1] == 0);
    assert(frame(state, pipkin::kTaskFreshMs - 1) == dots);
    assert(frame(state, pipkin::kTaskFreshMs) == full);
    tasks.blocked = 1;
    assert(frame(state, 1000) != dots);
    tasks.working = 8;
    const auto counted = frame(state, 1000);
    const auto counted_corner = changes(full, counted, 0, 30, right - 54);
    assert(counted != dots && counted_corner[0] > 0 && counted_corner[1] == 0);
    tasks.coverage = pipkin::TaskCoverage::Unavailable;
    assert(frame(state, 1000) == full);
    tasks.coverage = pipkin::TaskCoverage::Partial;
    tasks.freshness.age_on_receipt_ms.reset();
    assert(frame(state, 1000) == full);
    tasks.freshness.age_on_receipt_ms = 0;
    tasks.working = 2;
    state.tasks[1] = tasks;
    state.tasks[1].working = 7;

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
        for (int rows : {1, 7, 16, pipkin::kDisplayHeight})
            assert(banded(state, 1000, rows) == frame(state, 1000));
    }
    state.tasks = {};
    state.providers[1] = {};
    state.page = pipkin::Page::Codex;
    pipkin::render(state, 0, guarded.data() + 1, pipkin::kDisplayHeight - 40, 41);
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
                const int column = (pipkin::kLayoutWidth - 48) / 2;
                for (int y = panel(top); y < panel(top + 12); ++y)
                    for (int x = panel(16); x < panel(16 + column); ++x)
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
    const int qr_corner = at((pipkin::kLayoutWidth - (pipkin::setup_qr::kSize + 8) * 3) / 2, 8);
    assert(frame(state, 17999)[qr_corner] == blank[qr_corner]);
    assert(frame(state, 18000)[qr_corner] == 0xffff);
    for (uint64_t now : {17999, 18000, 24000}) {
        pipkin::render(state, now, guarded.data() + 1);
        assert(guarded.front() == 0xbeef && guarded.back() == 0xbeef);
        for (int rows : {1, 7, 16, pipkin::kDisplayHeight})
            assert(banded(state, now, rows) == frame(state, now));
    }
    state.transport_connected = true;
    assert(frame(state, 18000)[qr_corner] == blank[qr_corner]);
    assert(frame(state, 9000) != frame(state, 6000) && frame(state, 3000) != blank);
    for (uint64_t now : {0, 1000, 2000, 3000, 9000, 18000})
        for (int rows : {1, 7, 16, pipkin::kDisplayHeight})
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
    state.host = pipkin::HostState::Asleep;
    assert(!pipkin::animation_active(state, 1000) && frame(state, 1000) == frame(state, 2000));
    std::puts("Render bounds, remaining, reset visibility, reveal, clock, task dot, About and boot "
              "checks passed");
}
