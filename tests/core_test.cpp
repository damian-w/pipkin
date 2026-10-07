#include "pipkin/model.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using namespace pipkin;

namespace {

constexpr int64_t epoch = 1800000000;

struct Feed {
    State state;
    uint64_t sequence = 0;
    bool send(const std::string& fields, uint64_t now = 0) {
        return ingest(state, "v=1 seq=" + std::to_string(++sequence) + " " + fields, now);
    }
    void clock() { assert(send("kind=clock unix=1800000000 tz=480")); }
};

const std::string snapshot =
    "kind=usage provider=codex mode=full account=sample-a observed=1800000000 "
    "session=metered session_id=period-a session_used=640 session_reset=1800003600 "
    "session_seconds=18000 weekly=metered weekly_id=week-a weekly_used=220 "
    "weekly_reset=1800604800 weekly_seconds=604800 banked=0";

const std::string patch = "kind=usage provider=codex mode=patch account=sample-a observed=";

void release_touch(State& state, uint64_t now_ms) {
    touch(state, false, 0, 0, now_ms);
    touch(state, false, 0, 0, now_ms + kTouchReleaseGraceMs);
}

void state_and_merge() {
    Feed feed;
    assert(feed.state.page == Page::Overview);
    assert(!unix_now(feed.state, 0));
    assert(!host_alive(feed.state, 0));
    assert(feed.send(snapshot));
    auto& data = feed.state.providers[0];
    assert(data.session.used_tenths == 640 && data.weekly.used_tenths == 220);
    assert(data.banked_resets == 0);
    assert(data.session.freshness.stale(0));
    assert(feed.state.host == HostState::Unknown && feed.state.transport_connected);
    assert(feed.send("kind=clock unix=1800000600 tz=480", 600000));
    assert(feed.send(snapshot, 600000));
    assert(data.session.freshness.observation_age_ms(600000) == 600000);
    assert(data.session.freshness.stale(600000));
    assert(feed.send(patch + "1800000610 weekly=metered weekly_id=week-a weekly_used=330", 610000));
    assert(data.session.freshness.received_ms == 600000);
    assert(data.weekly.freshness.received_ms == 610000);
    assert(!data.weekly.reset_unix && !data.weekly.duration_seconds);
    assert(data.banked_resets == 0);
    assert(feed.send(patch + "1800000620 banked=null", 620000));
    assert(!data.banked_resets);
    assert(!feed.send(patch + "1800000000 banked=1", 620000));
    assert(!feed.send(patch + "1800000000 weekly=null", 620000));
    assert(!feed.send("kind=usage provider=codex mode=full account=sample-a observed=1800000000",
                      620000));
    assert(data.weekly.used_tenths == 330);
    assert(feed.send(patch + "1800000630 session=no_cap", 630000));
    assert(data.session.state == AllowanceState::NoCap && !data.session.used_tenths);
    assert(feed.send(patch + "1800000640 session=null", 640000));
    assert(data.session.state == AllowanceState::Unknown);
    assert(!feed.send(patch + "1800000630 session=no_cap", 640000));
    assert(feed.send(
        "kind=usage provider=codex mode=full account=sample-a observed=1800000650 banked=7",
        650000));
    assert(data.session.state == AllowanceState::Unknown &&
           data.weekly.state == AllowanceState::Unknown);
    assert(data.banked_resets == 7);
    assert(!feed.send(patch + "1800000640 session=no_cap", 650000));
}

void accounts_and_ordering() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot));
    const auto old_sequence = feed.state.last_sequence;
    assert(!ingest(feed.state, "v=1 seq=" + std::to_string(old_sequence) + " " + snapshot, 0));
    assert(!ingest(feed.state, "v=1 seq=1 " + snapshot, 0));
    assert(!feed.send(
        "kind=usage provider=codex mode=patch account=sample-b observed=1800000000 banked=9"));
    assert(feed.send("kind=app provider=codex state=available"));
    assert(feed.send("kind=usage provider=codex mode=full account=sample-b observed=1800000000 "
                     "weekly=unsupported"));
    const auto& data = feed.state.providers[0];
    assert(data.session.state == AllowanceState::Unknown && !data.banked_resets);
    assert(data.weekly.state == AllowanceState::Unsupported && data.app == AppState::Unknown);
    assert(std::string(data.account.data()) == "sample-b");
    assert(!ingest(feed.state, "v=1 seq=" + std::to_string(old_sequence) + " " + snapshot, 0));
    assert(feed.send("kind=app provider=codex state=signed_out"));
    assert(data.account[0] == 0 && data.weekly.state == AllowanceState::Unknown);
    assert(!feed.send(
        "kind=usage provider=codex mode=patch account=sample-b observed=1800000000 banked=1"));
    assert(feed.send(snapshot));
    assert(feed.send("kind=usage provider=codex mode=full account=null"));
    assert(data.account[0] == 0 && data.app == AppState::Unavailable && !data.banked_resets);
    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=1800000000 "
                     "session=not_started session_id=idle weekly=unsupported"));
    assert(feed.state.providers[1].session.state == AllowanceState::NotStarted);
}

void reject_invalid_packets() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot));
    const auto good_sequence = feed.state.last_sequence;
    const std::string usage = patch + "1800000000 ";
    for (const std::string& fields :
         {usage + "session=metered session_id=period-a session_used=1001",
          usage + "session=metered session_id=period-a session_used=-1",
          usage + "session=metered session_id=period-a session_used=63.5",
          usage + "session=metered session_id=period-a",
          usage + "session=metered session_id=period-a session_used=12 session_reset=0",
          usage + "session=metered session_id=period-a session_used=12 session_seconds=0",
          usage + "session=metered session_id=period-a session_used=12 session_seconds=31622401",
          usage + "session=not_started",
          usage + "session=not_started session_id=idle session_used=0",
          usage + "session=not-started session_id=idle",
          usage + "session=unsupported session_used=12",
          usage + "weekly=no_cap",
          usage + "session_id=period-a",
          usage + "banked=-1",
          usage + "banked=1000001",
          usage + "banked=0 banked=1",
          usage + "session=metered session_id=period-a session_used=120 weekly=invalid",
          usage + "unknown=1",
          usage + "session=metered session_id=abcdefghijklmnopqrstuvwxyz1234567 session_used=1",
          std::string("kind=clock unix=9999999999999999999999 tz=0"),
          std::string("kind=clock unix=1800000000 tz=841"),
          std::string("kind=clock unix=1800000000 tz=-841"),
          std::string("kind=clock unix=1800000000 tz=0 rebase=0"),
          std::string("kind=app provider=other state=available"),
          std::string("kind=host state=awake\t"),
          std::string("kind=host  state=awake"),
          std::string("kind=host state=awake "),
          std::string("kind=host state=awake\nkind=host state=asleep"),
          std::string("kind=host state=awake extra=")}) {
        assert(!feed.send(fields));
        assert(feed.state.last_sequence == good_sequence);
        assert(feed.state.providers[0].session.used_tenths == 640);
        assert(feed.state.providers[0].banked_resets == 0);
    }
    assert(!ingest(feed.state, std::string(kMaxPacketBytes + 1, 'a'), 0));
    assert(!ingest(feed.state, "v=2 seq=999 kind=host state=awake", 0));
    assert(!ingest(feed.state, "v=1 seq=0 kind=host state=awake", 0));
    assert(!ingest(feed.state, "v=1 seq=18446744073709551616 kind=host state=awake", 0));
    assert(!ingest(feed.state, "v=1 seq=999 kind=host state=aw\xc3\xa1ke", 0));
    assert(!feed.send(patch + "1800000031 banked=0"));
    assert(feed.send(usage + "session=metered session_id=next session_used=1000"));
    assert(feed.state.providers[0].session.used_tenths == 1000);
    assert(feed.send("kind=host state=awake\r\n", 1));
    assert(!feed.send("kind=host state=awake", 0));
}

void clock_freshness_and_liveness() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot));
    auto& data = feed.state.providers[0];
    assert(unix_now(feed.state, 3600000) == epoch + 3600);
    assert(data.session.used_tenths == 640);
    assert(!feed.send("kind=clock unix=1799999000 tz=0", 1000));
    assert(feed.send("kind=clock unix=1800000010 tz=480", 310000));
    assert(feed.send(snapshot, 310000));
    assert(data.session.freshness.observation_age_ms(310000) == 310000);
    assert(data.session.freshness.stale(310000));
    assert(feed.send("kind=clock unix=1799999000 tz=-300 rebase=1", 311000));
    assert(feed.state.clock.utc_offset_minutes == -300);
    assert(data.session.freshness.observation_age_ms(311000) == 311000);
    const auto clock_epoch = feed.state.clock.observation_epoch;
    assert(clock_epoch == feed.state.last_sequence);
    const std::string after_rebase = " clock_epoch=" + std::to_string(clock_epoch);
    assert(!feed.send(snapshot, 311000));
    assert(!feed.send(snapshot + after_rebase, 311000));
    assert(feed.send(patch + "1799999000" + after_rebase +
                         " session=metered session_id=period-a session_used=650",
                     311000));
    assert(data.session.freshness.observation_age_ms(311000) == 0);
    assert(data.weekly.freshness.observation_age_ms(311000) == 311000);
    assert(feed.state.host == HostState::Unknown);
    assert(feed.send("kind=host state=awake", 311000));
    assert(host_alive(feed.state, 370999));
    assert(!host_alive(feed.state, 371000));
    assert(feed.state.host == HostState::Awake && feed.state.overlay == Overlay::None);
    assert(feed.send("kind=app provider=codex state=unavailable", 371000));
    assert(data.session.used_tenths == 650);
    assert(!host_alive(feed.state, 371000));
    assert(feed.send("kind=host state=disconnected", 371000));
    assert(!feed.state.transport_connected && feed.state.host == HostState::Disconnected);
    assert(feed.send(patch + "1799999060" + after_rebase + " banked=2", 371000));
    assert(feed.state.transport_connected && feed.state.host == HostState::Unknown);
    assert(!host_alive(feed.state, 371000));
    assert(!unix_now(feed.state, 310999));
    Freshness maximum;
    maximum.age_on_receipt_ms = std::numeric_limits<uint64_t>::max() - 1;
    assert(maximum.observation_age_ms(2) == std::numeric_limits<uint64_t>::max());
    assert(maximum.observation_age_ms(0).has_value());
}

void unknown_observation_age() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot));
    assert(feed.send("kind=clock unix=1800000010 tz=480", 310000));
    assert(feed.send(
        patch + "null session=metered session_id=period-a session_used=650 banked=null", 310000));
    const auto& data = feed.state.providers[0];
    assert(data.session.used_tenths == 650 && !data.session.freshness.observed_unix);
    assert(!data.session.freshness.observation_age_ms(310000));
    assert(data.session.freshness.stale(310000));
    assert(!feed.send(patch + "1799999999 session=null", 310000));
    assert(!feed.send(patch + "1799999999 banked=0", 310000));
    assert(feed.send(snapshot, 310000));
    assert(data.session.freshness.observation_age_ms(310000) == 310000);
    assert(data.session.freshness.stale(310000));
    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=null "
                     "session=no_cap weekly=unsupported",
                     310000));
    assert(!feed.state.providers[1].session.freshness.observation_age_ms(310000));
}

void navigation_and_overlays() {
    Feed feed;
    feed.clock();
    assert(visible_pages(feed.state).count == 1);
    assert(page_visible(feed.state, Page::Overview));
    assert(!page_visible(feed.state, Page::Codex));
    restore_page(feed.state, Page::Claude, 20);
    assert(feed.state.page == Page::Claude && selected_page(feed.state) == Page::Overview);
    tap(feed.state, 160, 60, 30);
    assert(selected_page(feed.state) == Page::Overview && feed.state.view_revision == 0);

    assert(feed.send(snapshot, 1000));
    assert(provider_visible(feed.state, Provider::Codex));
    assert(!provider_visible(feed.state, Provider::Claude));
    assert(page_visible(feed.state, Page::Overview));
    assert(visible_pages(feed.state).count == 2);
    assert(visible_pages(feed.state).pages[1] == Page::Codex);
    assert(selected_page(feed.state) == Page::Overview);
    assert(feed.state.page == Page::Claude);
    assert(feed.state.view_revision == 0);
    tap(feed.state, 20, 20, 1500);
    assert(selected_page(feed.state) == Page::Overview && feed.state.view_revision == 0);

    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=1800000002 "
                     "session=metered session_id=period-c session_used=1000", 2000));
    assert(visible_pages(feed.state).count == 3);
    assert(selected_page(feed.state) == Page::Claude);
    assert(feed.state.page_entered_ms == 2000 && feed.state.view_revision == 1);
    tap(feed.state, 20, 20, 2100);
    assert(selected_page(feed.state) == Page::Claude);
    tap(feed.state, 20, 220, 2100);
    assert(selected_page(feed.state) == Page::Codex);
    tap(feed.state, 20, 220, 2150);
    assert(selected_page(feed.state) == Page::Overview);
    tap(feed.state, 160, 60, 2200);
    assert(selected_page(feed.state) == Page::Codex);
    assert(feed.state.page_entered_ms == 2200);
    tap(feed.state, 20, 220, 2300);
    assert(selected_page(feed.state) == Page::Overview);
    tap(feed.state, 160, 150, 2400);
    assert(selected_page(feed.state) == Page::Claude);
    tap(feed.state, 300, 215, 2500);
    assert(selected_page(feed.state) == Page::Overview);
    tap(feed.state, 160, 215, 2600);
    assert(selected_page(feed.state) == Page::Codex);
    tap(feed.state, 319, 239, 2700);
    assert(selected_page(feed.state) == Page::Claude);
    const auto revision = feed.state.view_revision;
    tap(feed.state, 320, 239, 2800);
    tap(feed.state, -1, 215, 2800);
    tap(feed.state, 159, 207, 2800);
    assert(selected_page(feed.state) == Page::Claude);
    assert(feed.state.view_revision == revision && feed.state.page_entered_ms == 2700);
    assert(feed.send("kind=host state=asleep", 3000));
    assert(feed.state.overlay == Overlay::Sleep);
    tap(feed.state, 5, 215, 3100);
    assert(selected_page(feed.state) == Page::Claude);
    assert(feed.send("kind=host state=awake", 3200));
    assert(feed.state.overlay == Overlay::None && selected_page(feed.state) == Page::Claude);
    assert(feed.state.view_revision == revision);
    assert(feed.send("kind=app provider=claude state=unavailable", 3500));
    assert(selected_page(feed.state) == Page::Claude && feed.state.view_revision == revision);
    assert(feed.send("kind=usage provider=claude mode=patch account=sample-c observed=1800000004 "
                     "session=metered session_id=period-c session_used=950", 4000));
    assert(feed.state.view_revision == revision && feed.state.page_entered_ms == 2700);
    assert(feed.send("kind=app provider=claude state=signed_out", 5000));
    assert(selected_page(feed.state) == Page::Overview && feed.state.page == Page::Overview);
    assert(feed.state.page_entered_ms == 5000 && feed.state.view_revision == revision + 1);
    restore_page(feed.state, static_cast<Page>(99), 5500);
    assert(selected_page(feed.state) == Page::Overview);
    tap(feed.state, 200, 150, 5500);
    assert(selected_page(feed.state) == Page::Codex);
    assert(feed.send("kind=usage provider=codex mode=full account=null", 6000));
    assert(selected_page(feed.state) == Page::Overview && feed.state.page == Page::Overview);
    assert(feed.state.page_entered_ms == 6000 && visible_pages(feed.state).count == 1);
}

void conditional_provider_visibility() {
    Feed feed;
    feed.clock();
    assert(!provider_visible(feed.state, static_cast<Provider>(99)));
    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=1800000000 "
                     "session=unsupported weekly=unsupported banked=0"));
    assert(!provider_visible(feed.state, Provider::Claude));
    assert(selected_page(feed.state) == Page::Overview);
    const std::string claude_patch = "kind=usage provider=claude mode=patch account=sample-c ";
    assert(feed.send(claude_patch + "observed=null banked=2"));
    assert(provider_visible(feed.state, Provider::Claude));
    assert(selected_page(feed.state) == Page::Overview && visible_pages(feed.state).count == 2);
    tap(feed.state, 160, 150, 0);
    assert(selected_page(feed.state) == Page::Claude);
    assert(feed.send(claude_patch + "observed=null banked=0"));
    assert(!provider_visible(feed.state, Provider::Claude));
    assert(feed.send(claude_patch + "observed=null session=no_cap"));
    assert(provider_visible(feed.state, Provider::Claude));
    tap(feed.state, 319, 220, 0);
    assert(selected_page(feed.state) == Page::Claude);
    assert(feed.send("kind=usage provider=claude mode=patch account=sample-c observed=null "
                     "session=not_started session_id=idle"));
    assert(provider_visible(feed.state, Provider::Claude));
    assert(feed.send("kind=usage provider=claude mode=patch account=sample-c observed=1800000000 "
                     "session=metered session_id=period-c session_used=0"));
    const auto revision = feed.state.view_revision;
    assert(feed.send("kind=app provider=claude state=unavailable", 600000));
    assert(feed.state.providers[1].session.freshness.stale(600000));
    assert(provider_visible(feed.state, Provider::Claude));
    assert(selected_page(feed.state) == Page::Claude && feed.state.view_revision == revision);
    assert(feed.send(
        "kind=usage provider=claude mode=full account=sample-c observed=1800000600", 600000));
    assert(!provider_visible(feed.state, Provider::Claude));
    assert(selected_page(feed.state) == Page::Overview);
}

void session_not_started() {
    Feed feed;
    feed.clock();
    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=1800000000 "
                     "session=metered session_id=old session_used=950 session_reset=1800003600 "
                     "session_seconds=18000 weekly=metered weekly_id=week-c weekly_used=100 "
                     "weekly_reset=1800604800 weekly_seconds=604800"));
    auto& data = feed.state.providers[1];
    restore_page(feed.state, Page::Claude);
    const auto revision = feed.state.view_revision;
    const std::string update = "kind=usage provider=claude mode=patch account=sample-c ";
    assert(feed.send(update + "observed=1800000001 session=not_started session_id=current", 1000));
    assert(data.session.state == AllowanceState::NotStarted);
    assert(!data.session.used_tenths && !data.session.reset_unix &&
           !data.session.duration_seconds && !data.session.from_used_tenths);
    assert(data.weekly.state == AllowanceState::Metered && data.weekly.used_tenths == 100);
    assert(data.weekly.reset_unix == 1800604800 && data.weekly.duration_seconds == 604800);
    assert(data.weekly.freshness.received_ms == 0);
    assert(provider_visible(feed.state, Provider::Claude) && selected_page(feed.state) == Page::Claude);
    assert(feed.state.view_revision == revision);
    assert(feed.send(update + "observed=1800000002 session=metered session_id=new session_used=0 "
                             "session_reset=1800018002 session_seconds=18000", 2000));
    assert(data.session.state == AllowanceState::Metered && data.session.used_tenths == 0);
    assert(data.session.reset_unix == 1800018002 && data.session.duration_seconds == 18000);
    assert(!data.session.from_used_tenths);
    assert(data.weekly.used_tenths == 100 && data.weekly.freshness.received_ms == 0);
    assert(provider_visible(feed.state, Provider::Claude) && selected_page(feed.state) == Page::Claude);
    assert(feed.state.view_revision == revision);
}

void gestures_and_about() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot));
    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=null "
                     "weekly=metered weekly_id=week-c weekly_used=400"));
    assert(visible_pages(feed.state).count == 3);
    swipe(feed.state, 1, 100);
    assert(effective_page(feed.state) == Page::Codex);
    swipe(feed.state, -1, 200);
    assert(effective_page(feed.state) == Page::Overview);
    swipe(feed.state, -1, 300);
    assert(effective_page(feed.state) == Page::Claude);
    swipe(feed.state, 1, 400);
    assert(effective_page(feed.state) == Page::Overview);
    touch(feed.state, true, 270, 100, 500);
    touch(feed.state, true, 80, 110, 600);
    release_touch(feed.state, 650);
    assert(effective_page(feed.state) == Page::Codex);
    touch(feed.state, true, 80, 100, 800);
    touch(feed.state, true, 270, 100, 900);
    release_touch(feed.state, 950);
    assert(effective_page(feed.state) == Page::Overview);
    const auto revision = feed.state.view_revision;
    touch(feed.state, true, 160, 50, 1100);
    touch(feed.state, true, 170, 180, 1200);
    release_touch(feed.state, 1250);
    assert(effective_page(feed.state) == Page::Overview && feed.state.view_revision == revision);

    restore_page(feed.state, Page::Codex, 1400);
    touch(feed.state, true, 160, 15, 1500);
    touch(feed.state, true, 164, 17, 4499);
    assert(!feed.state.about_open);
    touch(feed.state, true, 164, 17, 4500);
    assert(effective_page(feed.state) == Page::About && feed.state.about_open);
    assert(feed.state.page == Page::Codex && selected_page(feed.state) == Page::Codex);
    assert(!page_visible(feed.state, Page::About) && visible_pages(feed.state).count == 3);
    const auto about_revision = feed.state.view_revision;
    release_touch(feed.state, 4550);
    assert(feed.state.about_open && feed.state.view_revision == about_revision);
    assert(feed.state.device_info.firmware_version == "Unknown");
    assert(feed.state.device_info.git_revision == "Unknown");
    restore_page(feed.state, Page::About, 4700);
    assert(feed.state.page == Page::Codex);
    tap(feed.state, 300, 180, 4800);
    assert(effective_page(feed.state) == Page::Codex && !feed.state.about_open);

    touch(feed.state, true, 160, 15, 5000);
    touch(feed.state, true, 160, 15, 8000);
    release_touch(feed.state, 8050);
    assert(feed.state.about_open);
    touch(feed.state, true, 270, 90, 8200);
    touch(feed.state, true, 70, 90, 8300);
    release_touch(feed.state, 8400);
    assert(effective_page(feed.state) == Page::Codex && !feed.state.about_open);
    touch(feed.state, true, 160, 15, 8600);
    touch(feed.state, true, 186, 15, 11600);
    release_touch(feed.state, 11700);
    assert(!feed.state.about_open);
    touch(feed.state, true, 160, 15, 12000);
    assert(feed.send("kind=host state=asleep", 12100));
    touch(feed.state, true, 160, 15, 15000);
    assert(feed.send("kind=host state=awake", 15050));
    release_touch(feed.state, 15100);
    assert(!feed.state.about_open);

    touch(feed.state, true, 290, 15, 16000);
    touch(feed.state, true, 290, 15, 19000);
    release_touch(feed.state, 19050);
    assert(feed.state.about_open);
    assert(feed.send("kind=app provider=codex state=signed_out", 19200));
    assert(feed.state.about_open && feed.state.page == Page::Overview);
    assert(visible_pages(feed.state).count == 2);
    swipe(feed.state, -1, 19300);
    assert(effective_page(feed.state) == Page::Overview);
    swipe(feed.state, 1, 19400);
    assert(effective_page(feed.state) == Page::Claude);
    swipe(feed.state, 1, 19500);
    assert(effective_page(feed.state) == Page::Overview);
}

void about_hold_anywhere() {
    assert(kAboutHoldMs == 3000 && kTouchReleaseGraceMs == 100);
    // Header, both cards, footer, and the display's extreme corners are all targets.
    constexpr int positions[][2] = {{160, 15}, {160, 70}, {160, 170},
                                  {300, 225}, {0, 0}, {319, 239}};
    for (Page page : {Page::Overview, Page::Codex, Page::Claude}) {
        for (const auto& position : positions) {
            Feed feed;
            feed.clock();
            assert(feed.send(snapshot));
            assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=null "
                             "session=no_cap"));
            restore_page(feed.state, page, 50);
            touch(feed.state, true, position[0], position[1], 100);
            touch(feed.state, true, position[0], position[1], 3099);
            assert(!feed.state.about_open && effective_page(feed.state) == page);
            touch(feed.state, true, position[0], position[1], 3100);
            assert(feed.state.about_open && effective_page(feed.state) == Page::About);
            assert(selected_page(feed.state) == page && visible_pages(feed.state).count == 3);
            release_touch(feed.state, 3150);
            assert(feed.state.about_open && selected_page(feed.state) == page);
        }
    }
    // About is also available before either provider has a usable reading.
    State empty;
    touch(empty, true, 160, 170, 100);
    touch(empty, true, 160, 170, 3100);
    assert(empty.about_open && visible_pages(empty).count == 1);
}

void about_hold_movement() {
    for (int drift : {13, 25}) {
        State state;
        touch(state, true, 160, 100, 1000);
        touch(state, true, 160 + drift, 100 - drift, 2000);
        touch(state, true, 160 - drift, 100 + drift, 3999);
        assert(!state.about_open);
        touch(state, true, 160, 100, 4000);
        assert(state.about_open);
    }
    for (bool vertical : {false, true}) {
        State state;
        touch(state, true, 160, 100, 1000);
        touch(state, true, vertical ? 160 : 186, vertical ? 126 : 100, 2000);
        touch(state, true, 160, 100, 4000);
        assert(!state.about_open); // Returning to the origin cannot rescue excessive drift.
        release_touch(state, 4100);
        assert(!state.about_open);
    }
    // Wider hold tolerance must not make card taps more permissive.
    for (int drift : {12, 13, 25}) {
        Feed feed;
        assert(feed.send(snapshot));
        touch(feed.state, true, 160, 100, 1000);
        touch(feed.state, true, 160 + drift, 100, 1050);
        release_touch(feed.state, 1100);
        assert(!feed.state.about_open);
        assert(selected_page(feed.state) == (drift == 12 ? Page::Codex : Page::Overview));
    }
}

void about_hold_contact_dropouts() {
    State repeated;
    touch(repeated, true, 160, 170, 1000);
    touch(repeated, false, 0, 0, 1500);
    touch(repeated, false, 0, 0, 1550);
    touch(repeated, true, 175, 170, 1599);
    touch(repeated, false, 0, 0, 2400);
    touch(repeated, true, 160, 170, 2499);
    touch(repeated, true, 160, 170, 3999);
    assert(!repeated.about_open);
    touch(repeated, true, 160, 170, 4000);
    assert(repeated.about_open); // Each brief dropout preserves the original hold timer.

    State threshold;
    touch(threshold, true, 160, 15, 1000);
    touch(threshold, false, 0, 0, 3999);
    touch(threshold, false, 0, 0, 4000);
    assert(!threshold.about_open); // Missing contact never opens About at the deadline.
    touch(threshold, true, 160, 15, 4098);
    assert(threshold.about_open); // A valid sample after a 99 ms gap can still open it.

    State released;
    touch(released, true, 160, 15, 1000);
    touch(released, false, 0, 0, 3999);
    touch(released, false, 0, 0, 4098);
    assert(!released.about_open);
    touch(released, false, 0, 0, 4099);
    assert(!released.about_open);
    touch(released, true, 160, 15, 4100);
    touch(released, true, 160, 15, 7099);
    assert(!released.about_open);
    touch(released, true, 160, 15, 7100);
    assert(released.about_open); // A confirmed release requires a fresh three-second hold.

    for (uint64_t gap : {uint64_t{100}, uint64_t{500}}) {
        State restarted;
        touch(restarted, true, 160, 15, 1000);
        touch(restarted, false, 0, 0, 3950);
        const auto restart = 3950 + gap;
        // The next sample can be pressed: no intermediate false poll is required.
        touch(restarted, true, 160, 15, restart);
        assert(!restarted.about_open);
        touch(restarted, true, 160, 15, restart + 2999);
        assert(!restarted.about_open);
        touch(restarted, true, 160, 15, restart + 3000);
        assert(restarted.about_open);
    }
}

void about_requires_separate_exit_gesture() {
    Feed feed;
    assert(feed.send(snapshot));
    restore_page(feed.state, Page::Codex, 100);
    touch(feed.state, true, 160, 170, 1000);
    touch(feed.state, true, 160, 170, 4000);
    assert(feed.state.about_open);
    const auto revision = feed.state.view_revision;
    touch(feed.state, false, 0, 0, 4050);
    touch(feed.state, true, 160, 170, 4149);
    touch(feed.state, true, 300, 170, 4200);
    touch(feed.state, false, 0, 0, 4250);
    touch(feed.state, false, 0, 0, 4349);
    assert(feed.state.about_open);
    touch(feed.state, false, 0, 0, 4350);
    assert(feed.state.about_open && feed.state.view_revision == revision);
    // Continued holding, brief recovery, movement, and the opener's release are consumed.
    touch(feed.state, true, 160, 100, 4400);
    touch(feed.state, false, 0, 0, 4450);
    touch(feed.state, false, 0, 0, 4549);
    assert(feed.state.about_open);
    touch(feed.state, false, 0, 0, 4550);
    assert(!feed.state.about_open && effective_page(feed.state) == Page::Codex);
    assert(feed.state.view_revision == revision + 1);

    touch(feed.state, true, 160, 170, 5000);
    touch(feed.state, true, 160, 170, 8000);
    touch(feed.state, false, 0, 0, 8050);
    // A pressed sample after the grace period both confirms release and starts a new swipe.
    touch(feed.state, true, 270, 90, 8150);
    assert(feed.state.about_open);
    touch(feed.state, true, 70, 90, 8200);
    release_touch(feed.state, 8250);
    assert(!feed.state.about_open && effective_page(feed.state) == Page::Codex);
}

void status_closes_when_idle() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot));
    touch(feed.state, true, 290, 15, 1000);
    touch(feed.state, true, 290, 15, 4000);
    idle(feed.state, 4000 + kStatusIdleMs);
    assert(feed.state.about_open); // Still held: the finger counts as activity.
    touch(feed.state, true, 290, 15, 64000);
    release_touch(feed.state, 64050);
    touch(feed.state, false, 0, 0, 65000);
    idle(feed.state, 64050 + kStatusIdleMs - 1);
    assert(feed.state.about_open);
    idle(feed.state, 64050 + kStatusIdleMs);
    assert(!feed.state.about_open && effective_page(feed.state) == Page::Overview);
}

void boot_sequence() {
    Feed feed;
    start_boot(feed.state, 500);
    assert(feed.state.boot.active && !boot_finish_ms(feed.state.boot));
    idle(feed.state, 600000);
    assert(feed.state.boot.active && !feed.state.boot.ready_ms);
    feed.clock();
    idle(feed.state, 600100);
    assert(!feed.state.boot.ready_ms); // A clock alone is not a reading.
    assert(feed.send(snapshot, 600200));
    idle(feed.state, 600200);
    assert(feed.state.boot.ready_ms == 600200 && boot_finish_ms(feed.state.boot) == 600200);
    const auto revision = feed.state.view_revision;
    idle(feed.state, 600200 + kBootFinishMs - 1);
    assert(feed.state.boot.active);
    idle(feed.state, 600200 + kBootFinishMs);
    assert(!feed.state.boot.active && feed.state.page_entered_ms == 600200 + kBootFinishMs &&
           feed.state.view_revision == revision + 1);

    Feed early;
    start_boot(early.state, 1000);
    early.clock();
    assert(early.send(snapshot, 1100));
    idle(early.state, 1100);
    assert(boot_finish_ms(early.state.boot) == 1000 + kBootIntroMs);
    idle(early.state, 1000 + kBootIntroMs + kBootFinishMs);
    assert(!early.state.boot.active);
}

void reading_changes() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot, 1000));
    const auto& session = feed.state.providers[0].session;
    assert(!session.from_used_tenths && !changing(session, 1000));
    assert(feed.send(patch + "1800000010 session=metered session_id=period-a session_used=440",
                     11000));
    assert(session.from_used_tenths == 640 && session.changed_ms == 11000);
    assert(shown_used(session, 11000) == 640);
    const auto middle = shown_used(session, 11000 + kTransitionMs / 2);
    assert(middle < 640 && middle > 440);
    assert(shown_used(session, 11000 + kTransitionMs) == 440 && !changing(session, 11650));
    assert(feed.send(patch + "1800000011 session=metered session_id=period-a session_used=900",
                     13000));
    assert(feed.send(patch + "1800000012 session=metered session_id=period-a session_used=100",
                     13000 + kTransitionMs / 2));
    const auto from = *session.from_used_tenths;
    assert(from > 440 && from < 900);
    assert(feed.send(patch + "1800000013 session=metered session_id=period-a session_used=100",
                     20000));
    assert(!session.from_used_tenths);
}

void backlight() {
    Feed feed;
    feed.clock();
    assert(backlight_level(feed.state, kHostPresenceMs - 1) == 100);
    assert(backlight_level(feed.state, 3600000) == 0 && !newest_reading_age_ms(feed.state, 0));
    assert(feed.send(snapshot, 0));
    assert(backlight_level(feed.state, 0) == 100);
    assert(feed.send("kind=host state=asleep", 1000));
    assert(backlight_level(feed.state, 1000) == 0);
    assert(feed.send("kind=host state=awake", 2000));
    assert(backlight_level(feed.state, 2000) == 100);

    constexpr uint64_t dim = kStaleAfterMs + kDimAfterStaleMs;
    constexpr uint64_t off = kStaleAfterMs + kOffAfterStaleMs;
    // Isolate quota freshness from the independent helper-presence timeout.
    const auto with_heartbeat = [&](uint64_t now) {
        State state = feed.state;
        state.host_received_ms = now;
        return backlight_level(state, now);
    };
    assert(with_heartbeat(dim - 1) == 100);
    assert(with_heartbeat(dim) == kDimLevel);
    assert(with_heartbeat(off - 1) == kDimLevel);
    assert(with_heartbeat(off) == 0);
    assert(feed.send("kind=usage provider=claude mode=full account=sample-c observed=1800000900 "
                     "weekly=metered weekly_id=week-c weekly_used=100",
                     900000));
    assert(with_heartbeat(off) == 100);
    const auto page = effective_page(feed.state);
    constexpr uint64_t dark = 900000 + off;
    assert(backlight_level(feed.state, dark) == 0);
    touch(feed.state, true, 300, 225, dark);
    touch(feed.state, true, 300, 225, dark + kAboutHoldMs);
    assert(!feed.state.about_open); // The waking gesture cannot also open About.
    release_touch(feed.state, dark + kAboutHoldMs + 50);
    assert(effective_page(feed.state) == page);
    assert(backlight_level(feed.state, dark + kAboutHoldMs + 50) == 100);
    assert(backlight_level(feed.state, dark + kAboutHoldMs + 50 + kTouchWakeMs) == 0);
    constexpr uint64_t fresh = dark + kAboutHoldMs + kTouchWakeMs + 1000;
    assert(feed.send("kind=host state=awake", fresh));
    assert(feed.send(patch + std::to_string(epoch + dark / 1000) +
                         " session=metered session_id=period-a session_used=500",
                     fresh));
    assert(backlight_level(feed.state, fresh) == 100);
}

void host_power_backlight() {
    Feed feed;
    feed.clock();
    assert(feed.send(snapshot, 0));
    assert(feed.send("kind=host state=awake", 0));
    assert(backlight_level(feed.state, kHostPresenceMs - 1) == 100);
    assert(backlight_level(feed.state, kHostPresenceMs) == 0);
    assert(feed.state.host == HostState::Awake && feed.state.overlay == Overlay::None);
    assert(!host_alive(feed.state, kHostPresenceMs));

    // Recovering host presence lights the screen without waiting for an API read.
    constexpr uint64_t stale = kStaleAfterMs + kOffAfterStaleMs + 1000;
    assert(feed.send("kind=host state=awake", stale));
    assert(backlight_level(feed.state, stale) == 100);
    assert(feed.send("kind=host state=awake", stale + 30000));
    assert(feed.send("kind=host state=awake", stale + kHostWakeMs));
    assert(backlight_level(feed.state, stale + kHostWakeMs) == 0);

    assert(feed.send("kind=host state=asleep", stale + 61000));
    touch(feed.state, true, 100, 100, stale + 61001);
    assert(backlight_level(feed.state, stale + 61001) == 0);
    assert(feed.send("kind=host state=awake", stale + 62000));
    assert(backlight_level(feed.state, stale + 62000) == 100);
    touch(feed.state, true, 100, 100, stale + 62001);
    assert(feed.send("kind=host state=disconnected", stale + 63000));
    assert(backlight_level(feed.state, stale + 63000) == 0);
    assert(feed.state.host == HostState::Disconnected && feed.state.overlay == Overlay::None);

    // Generic traffic cannot keep a host's expired heartbeat alive.
    Feed noisy;
    noisy.clock();
    assert(noisy.send("kind=host state=awake", 0));
    assert(noisy.send(snapshot, kHostPresenceMs));
    assert(backlight_level(noisy.state, kHostPresenceMs) == 0);
    assert(noisy.state.host == HostState::Awake);
}

void unsequenced_senders() {
    State state;
    assert(ingest(state, "v=1 kind=clock unix=1800000000 tz=0", 0));
    assert(ingest(state, "v=1 " + snapshot, 1000));
    assert(state.last_sequence == 0 && state.providers[0].session.used_tenths == 640);
    assert(ingest(state, "v=1 seq=5 kind=host state=awake", 2000));
    assert(ingest(state, "v=1 kind=usage provider=claude mode=full account=sample-c "
                         "observed=1800000001 weekly=metered weekly_id=week-c weekly_used=100",
                  3000));
    assert(state.last_sequence == 5);
    assert(!ingest(state, "v=1 seq=5 kind=host state=awake", 4000));
    assert(!ingest(state, "v=1 seq= kind=host state=awake", 4000));
    assert(!ingest(state, "v=1 " + patch + "1799999999 banked=3", 4000));
    assert(!ingest(state, "v=1 kind=clock unix=1800000000 tz=0 rebase=1", 4000));
    assert(state.providers[0].banked_resets == 0);
}

// Packets written by the Pipkin CLI's test suite; every packet must be accepted in order.
void helper_fixture() {
    std::ifstream fixture("tests/fixtures/helper_packets.txt");
    assert(fixture);
    State state;
    std::string line;
    uint64_t now = 0;
    int count = 0;
    bool saw_not_started = false;
    while (std::getline(fixture, line)) {
        assert(ingest(state, line, now += 100));
        if (line.find("kind=host state=asleep") != std::string::npos)
            assert(state.overlay == Overlay::Sleep && backlight_level(state, now) == 0);
        if (line.find("kind=host state=awake") != std::string::npos)
            assert(state.overlay == Overlay::None && backlight_level(state, now) == 100);
        if (line.find("session=not_started") != std::string::npos) {
            const auto& claude = state.providers[1];
            assert(claude.session.state == AllowanceState::NotStarted);
            assert(!claude.session.used_tenths && !claude.session.reset_unix);
            assert(claude.session.duration_seconds == 18000);
            assert(claude.weekly.used_tenths == 412 && provider_visible(state, Provider::Claude));
            saw_not_started = true;
        }
        ++count;
    }
    assert(count == 19 && saw_not_started);
    assert(state.providers[0].weekly.used_tenths == 1000 && state.providers[0].banked_resets == 0);
    assert(state.providers[1].session.state == AllowanceState::Metered);
    assert(state.providers[1].session.used_tenths == 10 && state.clock.observation_epoch == 7);
    assert(state.providers[1].session.reset_unix == 1800018000);
    assert(state.host == HostState::Disconnected);
}

} // namespace

int main() {
    state_and_merge();
    accounts_and_ordering();
    reject_invalid_packets();
    clock_freshness_and_liveness();
    unknown_observation_age();
    navigation_and_overlays();
    conditional_provider_visibility();
    session_not_started();
    gestures_and_about();
    about_hold_anywhere();
    about_hold_movement();
    about_hold_contact_dropouts();
    about_requires_separate_exit_gesture();
    reading_changes();
    status_closes_when_idle();
    boot_sequence();
    backlight();
    host_power_backlight();
    unsequenced_senders();
    helper_fixture();
    std::cout << "Core protocol, state, clock and navigation checks passed.\n";
}
