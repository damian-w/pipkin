#include "pipkin/demo.h"

#include <string_view>

namespace pipkin {
namespace {

// The reel's sample readings at 09:41 on Thursday 8 October 2026 in Perth (1791423660).
// Sessions reset in 1h 29m and 1h 9m, weeks on Monday and Sunday at 09:41.
constexpr std::string_view kDemoPackets[] = {
    "v=2 kind=clock unix=1791423660 tz=480",
    "v=2 kind=host state=awake",
    "v=2 kind=usage provider=codex mode=full account=demo-codex observed=1791423660 "
    "session=metered session_id=demo-session session_used=480 session_reset=1791429059 "
    "session_seconds=18000 weekly=metered weekly_id=demo-week weekly_used=820 "
    "weekly_reset=1791769260 weekly_seconds=604800 banked=2",
    "v=2 kind=usage provider=claude mode=full account=demo-claude observed=1791423660 "
    "session=metered session_id=demo-session session_used=370 session_reset=1791427859 "
    "session_seconds=18000 weekly=metered weekly_id=demo-week weekly_used=550 "
    "weekly_reset=1791682860 weekly_seconds=604800",
    "v=2 kind=app provider=codex state=available",
    "v=2 kind=app provider=claude state=available",
    "v=2 kind=tasks provider=codex coverage=partial observed=1791423660 working=2 attention=1 "
    "blocked=0",
    "v=2 kind=tasks provider=claude coverage=partial observed=1791423660 working=1 attention=0 "
    "blocked=0",
};
// As in the website's loading reel, the computer connects 3.2 s into the boot.
constexpr uint64_t kDemoReadingsMs = 3200;
// Each page shows for 4 s, including its reveal.
constexpr uint64_t kDemoPageMs = 4000;

} // namespace

void play_demo(State& state, uint64_t now_ms) {
    if (!state.clock.anchor_unix) {
        if (now_ms < state.boot.started_ms + kDemoReadingsMs)
            return;
        for (const auto packet : kDemoPackets)
            ingest(state, packet, now_ms);
    }
    state.clock.anchor_ms = now_ms;
    state.host_received_ms = now_ms;
    for (auto& provider : state.providers)
        for (auto* freshness : {&provider.session.freshness, &provider.weekly.freshness,
                                &provider.banked_freshness})
            freshness->received_ms = now_ms;
    for (auto& tasks : state.tasks)
        tasks.freshness.received_ms = now_ms;
    if (!state.boot.active && !state.about_open && !state.gesture.active &&
        now_ms - state.page_entered_ms >= kDemoPageMs)
        swipe(state, 1, now_ms);
}

} // namespace pipkin
