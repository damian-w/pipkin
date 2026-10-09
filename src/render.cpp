#include "pipkin/render.h"
#include "font_data.h"
#include "setup_qr_data.h"
#include "wordmark_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pipkin {
namespace {
constexpr uint16_t rgb(int r, int g, int b) {
    return static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
constexpr auto background = rgb(13, 20, 25);
constexpr auto foreground = rgb(239, 245, 244);
constexpr auto secondary = rgb(154, 176, 187);
constexpr auto track = rgb(43, 59, 68);
constexpr auto hairline = rgb(29, 41, 49);
constexpr auto accent = rgb(155, 217, 193);
constexpr auto caution = rgb(245, 186, 135);
constexpr auto depleted = rgb(245, 145, 139);
constexpr auto codex_tile = rgb(94, 106, 210);
constexpr auto claude_tile = rgb(201, 112, 78);
constexpr auto pip_body = rgb(238, 138, 92);
constexpr auto pip_hand = rgb(224, 122, 77);
constexpr auto sparkle = rgb(245, 197, 66);
constexpr float pi = 3.14159265f;

bool revealing(const State& state, uint64_t now) {
    return state.overlay == Overlay::None && !state.about_open && state.view_revision > 0 &&
           now >= state.page_entered_ms && now - state.page_entered_ms < kTransitionMs;
}

double reveal(const State& state, uint64_t now) {
    return revealing(state, now) ? transition(state.page_entered_ms, now) : 1;
}

unsigned remaining(const Window& window, uint64_t now, double progress) {
    return static_cast<unsigned>((1000 - shown_used(window, now)) * progress + 0.5);
}

bool session_visible(const ProviderData& provider) {
    const auto session = provider.session.state;
    return session != AllowanceState::NoCap &&
           !(usable(provider.weekly) &&
             (session == AllowanceState::Unknown || session == AllowanceState::Unsupported));
}

bool linked(const State& state) {
    return state.transport_connected && state.host != HostState::Disconnected;
}

// Seconds since the epoch in local time; validated times are always positive.
int64_t local_time(const State& state, int64_t unix) {
    return unix + state.clock.utc_offset_minutes * 60;
}

uint16_t blend(uint16_t below, uint16_t above, int alpha) {
    const int r = (((below >> 11) & 31) * (15 - alpha) + ((above >> 11) & 31) * alpha) / 15;
    const int g = (((below >> 5) & 63) * (15 - alpha) + ((above >> 5) & 63) * alpha) / 15;
    const int b = ((below & 31) * (15 - alpha) + (above & 31) * alpha) / 15;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

uint16_t tone(const Window& window) {
    const auto used = window.used_tenths.value_or(0);
    return used >= 996 ? depleted : used >= 800 ? caution : accent;
}

uint16_t figure(const Window& window) {
    return window.used_tenths.value_or(0) >= 996 ? depleted : foreground;
}

// Tabular digits keep changing percentages aligned.
bool tabular(const font::Font& f) {
    return &f == &font::size24 || &f == &font::size36 || &f == &font::size48;
}

int cell(const font::Font& f) {
    int widest = 0;
    for (char digit = '0'; digit <= '9'; ++digit)
        widest = std::max<int>(widest, f.glyphs[digit - 32].advance);
    return widest;
}

int advance(const font::Font& f, unsigned char c) {
    return tabular(f) && c >= '0' && c <= '9' ? cell(f) : f.glyphs[c - 32].advance;
}

int inset(const font::Font& f, unsigned char c) {
    return (advance(f, c) - f.glyphs[c - 32].advance) / 2;
}

struct Segment {
    float x0, y0, x1, y1;
};

// Draws into the band of rows [top, bottom); pixels holds only that band.
struct Canvas {
    uint16_t* pixels;
    int top;
    int bottom;
    void pixel(int x, int y, uint16_t color, int alpha = 15) {
        if (x < 0 || x >= kDisplayWidth || y < top || y >= bottom)
            return;
        auto& target = pixels[(y - top) * kDisplayWidth + x];
        target = alpha >= 15 ? color : blend(target, color, alpha);
    }
    void rect(int x, int y, int w, int h, uint16_t color) {
        const int left = std::max(x, 0);
        const int right = std::min(x + w, kDisplayWidth);
        if (left >= right)
            return;
        for (int j = std::max(y, top); j < std::min(y + h, bottom); ++j)
            std::fill_n(pixels + (j - top) * kDisplayWidth + left, right - left, color);
    }
    int width(const char* text, const font::Font& f) const {
        int w = 0;
        for (; *text; ++text) {
            auto c = static_cast<unsigned char>(*text);
            if (c >= 32 && c <= 126)
                w += advance(f, c);
        }
        return w;
    }
    // A w by h map of 4-bit coverage, two pixels a byte, high nibble first.
    void coverage_map(int x, int y, int w, int h, const uint8_t* coverage, uint16_t color) {
        const int first = std::max(0, top - y);
        const int last = std::min(h, bottom - y);
        for (int j = first; j < last; ++j)
            for (int i = 0; i < w; ++i) {
                const int n = j * w + i;
                const int a = (coverage[n / 2] >> (n % 2 ? 0 : 4)) & 15;
                if (a)
                    pixel(x + i, y + j, color, a);
            }
    }
    void text(int x, int y, const char* text, const font::Font& f, uint16_t color) {
        for (; *text; ++text) {
            auto c = static_cast<unsigned char>(*text);
            if (c < 32 || c > 126)
                continue;
            const auto& g = f.glyphs[c - 32];
            coverage_map(x + inset(f, c) + g.x, y + g.y, g.width, g.height, f.coverage + g.offset,
                         color);
            x += advance(f, c);
        }
    }
    void centered(int x, int y, const char* s, const font::Font& f, uint16_t color) {
        text(x - width(s, f) / 2, y, s, f, color);
    }
    void right(int x, int y, const char* s, const font::Font& f, uint16_t color) {
        text(x - width(s, f), y, s, f, color);
    }
    void coverage_pixel(int x, int y, float coverage, uint16_t color) {
        if (coverage > 0)
            pixel(x, y, color, static_cast<int>(std::min(1.0f, coverage) * 15 + 0.5f));
    }
    void round_rect(int x, int y, int w, int h, float radius, uint16_t color) {
        for (int j = std::max(y, top); j < std::min(y + h, bottom); ++j)
            for (int i = x; i < x + w; ++i) {
                const float px = i + 0.5f;
                const float py = j + 0.5f;
                const float dx = std::max({x + radius - px, px - (x + w - radius), 0.0f});
                const float dy = std::max({y + radius - py, py - (y + h - radius), 0.0f});
                coverage_pixel(i, j, radius + 0.5f - std::sqrt(dx * dx + dy * dy), color);
            }
    }
    void line(float x0, float y0, float x1, float y1, float width, uint16_t color) {
        const Segment segment{x0, y0, x1, y1};
        strokes(&segment, 1, width, color);
    }
    // Round-capped segments stroked as one shape, so joins and crossings are not drawn twice.
    void strokes(const Segment* segments, int count, float width, uint16_t color) {
        const float half = width / 2;
        float left = segments[0].x0, right = left, high = segments[0].y0, low = high;
        for (int n = 0; n < count; ++n) {
            const auto& s = segments[n];
            left = std::min({left, s.x0, s.x1});
            right = std::max({right, s.x0, s.x1});
            high = std::min({high, s.y0, s.y1});
            low = std::max({low, s.y0, s.y1});
        }
        const int first = std::max(static_cast<int>(high - half - 1), top);
        const int last = std::min(static_cast<int>(low + half + 1), bottom - 1);
        for (int j = first; j <= last; ++j)
            for (int i = static_cast<int>(left - half - 1); i <= static_cast<int>(right + half + 1);
                 ++i) {
                float nearest = 1e9f;
                for (int n = 0; n < count; ++n) {
                    const auto& s = segments[n];
                    const float dx = s.x1 - s.x0;
                    const float dy = s.y1 - s.y0;
                    const float length = std::max(dx * dx + dy * dy, 0.0001f);
                    const float px = i + 0.5f - s.x0;
                    const float py = j + 0.5f - s.y0;
                    const float t = std::clamp((px * dx + py * dy) / length, 0.0f, 1.0f);
                    const float ex = px - t * dx;
                    const float ey = py - t * dy;
                    nearest = std::min(nearest, ex * ex + ey * ey);
                }
                coverage_pixel(i, j, half + 0.5f - std::sqrt(nearest), color);
            }
    }
    void ellipse(float cx, float cy, float rx, float ry, uint16_t color) {
        const float edge = std::min(rx, ry);
        const int first = std::max(static_cast<int>(cy - ry - 1), top);
        const int last = std::min(static_cast<int>(cy + ry + 1), bottom - 1);
        for (int j = first; j <= last; ++j)
            for (int i = static_cast<int>(cx - rx - 1); i <= static_cast<int>(cx + rx + 1); ++i) {
                const float dx = (i + 0.5f - cx) / rx;
                const float dy = (j + 0.5f - cy) / ry;
                coverage_pixel(i, j, (1 - std::sqrt(dx * dx + dy * dy)) * edge + 0.5f, color);
            }
    }
    // from/to are fractions of a 270-degree sweep with rounded ends.
    void arc(int cx, int cy, float middle, float half, float from, float to, uint16_t color) {
        // Single-precision math: the ESP32 FPU has no double-precision support.
        constexpr float start = pi * 0.75f;
        constexpr float sweep = pi * 1.5f;
        const float ends[2][2] = {
            {middle * std::cos(start + sweep * from), middle * std::sin(start + sweep * from)},
            {middle * std::cos(start + sweep * to), middle * std::sin(start + sweep * to)}};
        const int reach = static_cast<int>(middle + half) + 2;
        const float inner = std::max(0.0f, middle - half - 1);
        const float outer = middle + half + 1;
        const int first = std::max(-reach, top - cy);
        const int last = std::min(reach, bottom - 1 - cy);
        for (int y = first; y <= last; ++y)
            for (int x = -reach; x <= reach; ++x) {
                const float px = x + 0.5f;
                const float py = y + 0.5f;
                const float squared = px * px + py * py;
                if (squared < inner * inner || squared > outer * outer)
                    continue;
                float angle = std::atan2(py, px) - start;
                if (angle < 0)
                    angle += 2 * pi;
                const float at = angle / sweep;
                float distance;
                if (at >= from && at <= to)
                    distance = std::fabs(std::sqrt(squared) - middle);
                else {
                    const float ax = px - ends[0][0], ay = py - ends[0][1];
                    const float bx = px - ends[1][0], by = py - ends[1][1];
                    distance = std::sqrt(std::min(ax * ax + ay * ay, bx * bx + by * by));
                }
                coverage_pixel(cx + x, cy + y, half + 0.5f - distance, color);
            }
    }
    void gauge(int cx, int cy, int radius, const Window& window, uint64_t now, double progress) {
        const float fraction = metered(window) ? remaining(window, now, progress) / 1000.0f : 0;
        arc(cx, cy, radius - 2.5f, 3, 0, 1, track);
        if (fraction > 0)
            arc(cx, cy, radius - 2.5f, 3, 0, fraction, tone(window));
    }
};

constexpr int kMarkSize = 16;

// Original generic marks: a prompt for Codex and code brackets for Claude Code.
void provider_mark(Canvas& c, int index, int x, int y) {
    c.round_rect(x, y, kMarkSize, kMarkSize, 4, index == 0 ? codex_tile : claude_tile);
    constexpr float stroke = 1.6f;
    if (index == 0) {
        c.line(x + 4.5f, y + 5, x + 7.5f, y + 8, stroke, foreground);
        c.line(x + 7.5f, y + 8, x + 4.5f, y + 11, stroke, foreground);
        c.line(x + 9.5f, y + 11.5f, x + 12, y + 11.5f, stroke, foreground);
    } else {
        c.line(x + 5, y + 5, x + 2.8f, y + 8, stroke, foreground);
        c.line(x + 2.8f, y + 8, x + 5, y + 11, stroke, foreground);
        c.line(x + 9, y + 4.5f, x + 7, y + 11.5f, stroke, foreground);
        c.line(x + 11, y + 5, x + 13.2f, y + 8, stroke, foreground);
        c.line(x + 13.2f, y + 8, x + 11, y + 11, stroke, foreground);
    }
}

const char* provider_name(int index) { return index == 0 ? "Codex" : "Claude Code"; }

void provider_title(Canvas& c, int index, int y) {
    provider_mark(c, index, 16, y);
    c.text(40, y - 1, provider_name(index), font::size14, foreground);
}

const char* setup_hint(const State& state) {
    return linked(state) ? "Sign in to Codex or Claude Code" : "Set up at pipkin.io/start";
}

// Unknown sessions need a label; a confirmed uncapped session does not.
const char* session_note(const ProviderData& p) {
    switch (p.session.state) {
    case AllowanceState::NoCap:
        return nullptr;
    case AllowanceState::Unsupported:
        return "Session unsupported";
    default:
        return "Session unknown";
    }
}

void percentage(const Window& window, uint64_t now, double progress, char* out,
                std::size_t length) {
    if (metered(window))
        std::snprintf(out, length, "%u%%", (remaining(window, now, progress) + 5) / 10);
    else
        std::snprintf(out, length, "--");
}

const char* state_label(const Window& window) {
    switch (window.state) {
    case AllowanceState::Metered:
        return "remaining";
    case AllowanceState::NoCap:
        return "No cap";
    case AllowanceState::NotStarted:
        return "Not started";
    case AllowanceState::Unsupported:
        return "Unsupported";
    default:
        return "Unknown";
    }
}

// Label nonstandard window durations supplied by the provider.
void window_label(const Window& w, bool weekly, char* out, std::size_t length) {
    const char* name = weekly ? "Weekly" : "Session";
    const uint32_t usual = weekly ? 7 * 86400 : 5 * 3600;
    if (!w.duration_seconds || *w.duration_seconds == usual) {
        std::snprintf(out, length, "%s", name);
        return;
    }
    const auto seconds = *w.duration_seconds;
    if (seconds % 86400 == 0)
        std::snprintf(out, length, "%s / %ud", name, static_cast<unsigned>(seconds / 86400));
    else if (seconds % 3600 == 0)
        std::snprintf(out, length, "%s / %uh", name, static_cast<unsigned>(seconds / 3600));
    else
        std::snprintf(out, length, "%s", name);
}

void countdown(const State& state, const Window& w, uint64_t now, char* out, std::size_t length) {
    if (w.state == AllowanceState::NotStarted) {
        std::snprintf(out, length, "Starts on first message");
        return;
    }
    if (w.state != AllowanceState::Metered) {
        std::snprintf(out, length, "%s", state_label(w));
        return;
    }
    const auto time = unix_now(state, now);
    if (!w.reset_unix) {
        std::snprintf(out, length, "Reset unknown");
        return;
    }
    if (!time) {
        std::snprintf(out, length, "Time not synced");
        return;
    }
    int64_t left = *w.reset_unix - *time;
    if (left <= 0) {
        std::snprintf(out, length, "Awaiting refresh");
        return;
    }
    // A day or more away, the local day and time read better than a long countdown.
    if (left >= 86400 && left < 7 * 86400) {
        static constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        const int64_t local = local_time(state, *w.reset_unix);
        const int64_t second = local % 86400;
        // 1 January 1970 was a Thursday.
        std::snprintf(out, length, "Resets %s %02d:%02d", days[(local / 86400 + 4) % 7],
                      static_cast<int>(second / 3600), static_cast<int>(second % 3600 / 60));
    } else if (left >= 86400)
        std::snprintf(out, length, "Reset in %lldd %lldh", static_cast<long long>(left / 86400),
                      static_cast<long long>(left % 86400 / 3600));
    else if (left >= 3600)
        std::snprintf(out, length, "Reset in %lldh %lldm", static_cast<long long>(left / 3600),
                      static_cast<long long>(left % 3600 / 60));
    else if (left >= 60)
        std::snprintf(out, length, "Reset in %lldm", static_cast<long long>(left / 60));
    else
        std::snprintf(out, length, "Reset in <1m");
}

const char* freshness(const ProviderData& p, uint64_t now) {
    if (p.app == AppState::Unsupported)
        return "App unsupported";
    if (p.app == AppState::Unavailable)
        return "App unavailable";
    bool observed = false;
    for (const auto* w : {&p.session, &p.weekly}) {
        if (!w->freshness.observed_unix && w->state == AllowanceState::Unknown)
            continue;
        observed = true;
        if (w->freshness.known_stale(now))
            return "Stale reading";
    }
    if (!observed)
        return "Waiting for data";
    return nullptr;
}

const char* connection(const State& state) {
    if (state.host == HostState::Disconnected)
        return "Disconnected";
    if (!state.transport_connected)
        return "Not connected";
    return nullptr;
}

int pill(Canvas& c, int x, int middle, const char* s, uint16_t color) {
    const int w = c.width(s, font::size11) + 12;
    c.round_rect(x, middle - 8, w, 16, 8, track);
    c.text(x + 6, middle - 7, s, font::size11, color);
    return w;
}

void banked(Canvas& c, const ProviderData& p, int index, int middle, uint64_t now) {
    if (!p.banked_resets || *p.banked_resets == 0)
        return;
    char text[24];
    std::snprintf(text, sizeof(text), "%u reset%s", static_cast<unsigned>(*p.banked_resets),
                  *p.banked_resets == 1 ? "" : "s");
    pill(c, 48 + c.width(provider_name(index), font::size14), middle, text,
         p.banked_freshness.known_stale(now) ? caution : secondary);
}

void clock_text(Canvas& c, const State& state, uint64_t now, bool centred) {
    char value[16] = "--:--";
    if (auto time = unix_now(state, now)) {
        const auto second = local_time(state, *time) % 86400;
        std::snprintf(value, sizeof(value), "%02u:%02u", static_cast<unsigned>(second / 3600),
                      static_cast<unsigned>(second / 60 % 60));
    }
    const auto& size = font::size12;
    const int digit = cell(size);
    constexpr int colon = 4;
    int x = centred ? (kDisplayWidth - digit * 4 - colon) / 2 : 304 - digit * 4 - colon;
    for (int i = 0; i < 5; ++i) {
        const int width = i == 2 ? colon : digit;
        const char text[] = {value[i], 0};
        if (i == 2) {
            c.rect(x + width / 2, 14, 1, 1, foreground);
            c.rect(x + width / 2, 18, 1, 1, foreground);
        } else
            c.centered(x + width / 2, 9, text, size, foreground);
        x += width;
    }
}

void page_indicator(Canvas& c, const State& state) {
    const auto pages = visible_pages(state);
    const auto selected = effective_page(state);
    constexpr int spacing = 14;
    const int first = kDisplayWidth / 2 - (pages.count - 1) * spacing / 2;
    for (int i = 0; i < pages.count; ++i) {
        const bool current = selected == pages.pages[i];
        const int x = first + i * spacing;
        c.round_rect(x - 3, 225, 6, 6, 3, current ? foreground : track);
    }
}

void bar(Canvas& c, int x, int y, int width, const Window& w, uint64_t now, double progress) {
    c.round_rect(x, y, width, 4, 2, track);
    const int filled = width * static_cast<int>(remaining(w, now, progress)) / 1000;
    if (filled > 0)
        c.round_rect(x, y, std::max(filled, 4), 4, 2, tone(w));
}

void provider_heading(Canvas& c, const ProviderData& p, int index, int y, uint64_t now) {
    provider_title(c, index, y);
    banked(c, p, index, y + 8, now);
    if (const char* status = freshness(p, now))
        c.right(304, y + 2, status, font::size11, caution);
}

void overview_window(Canvas& c, const State& state, int x, int y, int width, const Window& w,
                     bool weekly, bool expanded, uint64_t now, double progress,
                     const char* note = nullptr) {
    char label[32];
    window_label(w, weekly, label, sizeof(label));
    c.text(x, y, label, font::size11, secondary);
    if (note)
        c.right(x + width, y, note, font::size11, secondary);
    const auto& number = expanded ? font::size36 : font::size24;
    const int baseline = y + (expanded ? 48 : 38);
    char reset[40];
    countdown(state, w, now, reset, sizeof(reset));
    const auto reset_color = w.freshness.known_stale(now) ? caution : secondary;
    if (metered(w)) {
        char value[16];
        percentage(w, now, progress, value, sizeof(value));
        c.text(x, baseline - (expanded ? 35 : 24), value, number, figure(w));
        c.text(x + c.width(value, number) + 5, baseline - 10, "remaining", font::size10,
               secondary);
        bar(c, x, baseline + (expanded ? 33 : 7), width, w, now, progress);
    } else {
        c.text(x, baseline - 11, state_label(w), font::size11, foreground);
        c.round_rect(x, baseline + (expanded ? 33 : 7), width, 4, 2, track);
    }
    if (expanded && w.state != AllowanceState::NotStarted)
        c.text(x, baseline + 10, reset, font::size11, reset_color);
    else if (width > 200 && w.state == AllowanceState::Metered)
        c.right(x + width, baseline - 11, reset, font::size11, reset_color);
}

void provider_row(Canvas& c, const State& state, int index, int y, bool expanded, uint64_t now,
                  double progress) {
    const auto& p = state.providers[index];
    provider_heading(c, p, index, y, now);
    if (session_visible(p)) {
        overview_window(c, state, 16, y + 25, 136, p.session, false, expanded, now, progress);
        overview_window(c, state, 168, y + 25, 136, p.weekly, true, expanded, now, progress);
    } else
        overview_window(c, state, 16, y + 25, 288, p.weekly, true, expanded, now, progress,
                        session_note(p));
}

// Two dials share a detail page; a lone weekly dial is larger.
struct Dial {
    int radius;
    const font::Font& number;
    int number_top;
    int note_top;
};
constexpr Dial kPairedDial{58, font::size36, -30, 10};
constexpr Dial kLoneDial{64, font::size48, -40, 14};

void detail_window(Canvas& c, const State& state, const Window& w, bool weekly, int cx, int cy,
                   const Dial& dial, uint64_t now, double progress) {
    char value[40];
    window_label(w, weekly, value, sizeof(value));
    c.centered(cx, cy - dial.radius - 25, value, font::size14, secondary);
    c.gauge(cx, cy, dial.radius, w, now, progress);
    if (metered(w)) {
        percentage(w, now, progress, value, sizeof(value));
        c.centered(cx, cy + dial.number_top, value, dial.number, figure(w));
        c.centered(cx, cy + dial.note_top, "remaining", font::size10, secondary);
    } else
        c.centered(cx, cy - 7, state_label(w), font::size14, foreground);
    countdown(state, w, now, value, sizeof(value));
    c.centered(cx, cy + dial.radius * 7 / 10 + 12, value, font::size11,
               w.freshness.known_stale(now) ? caution : secondary);
}

void detail(Canvas& c, const State& state, uint64_t now, double progress) {
    const int index = effective_page(state) == Page::Codex ? 0 : 1;
    const auto& p = state.providers[index];
    provider_title(c, index, 8);
    banked(c, p, index, 16, now);
    const char* footer = connection(state);
    if (!footer)
        footer = freshness(p, now);
    if (!footer && !session_visible(p))
        footer = session_note(p);
    // The dials sit midway between the heading and the page dots, rising to make room for a note.
    const int cy = footer ? 127 : 133;
    if (session_visible(p)) {
        detail_window(c, state, p.session, false, 84, cy, kPairedDial, now, progress);
        detail_window(c, state, p.weekly, true, 236, cy, kPairedDial, now, progress);
    } else
        detail_window(c, state, p.weekly, true, 160, cy, kLoneDial, now, progress);
    if (footer)
        c.centered(160, 203, footer, font::size11, caution);
}

void short_age(uint64_t milliseconds, char* out, std::size_t length) {
    const uint64_t seconds = milliseconds / 1000;
    if (seconds < 60)
        std::snprintf(out, length, "%llus", static_cast<unsigned long long>(seconds));
    else if (seconds < 3600)
        std::snprintf(out, length, "%llum", static_cast<unsigned long long>(seconds / 60));
    else
        std::snprintf(out, length, "%lluh", static_cast<unsigned long long>(seconds / 3600));
}

void status_dot(Canvas& c, int x, int middle, uint16_t color) {
    c.round_rect(x, middle - 3, 7, 7, 3.5f, color);
}

// Keep observation age distinct from receipt age when the source time is unknown.
void provider_status(Canvas& c, const ProviderData& p, int index, int y, uint64_t now) {
    provider_title(c, index, y);
    const auto& window = p.weekly.state == AllowanceState::Unknown ||
                                 p.weekly.state == AllowanceState::Unsupported
                             ? p.session
                             : p.weekly;
    const bool reported =
        window.state != AllowanceState::Unknown && window.state != AllowanceState::Unsupported;
    const char* shape = "No usage reported";
    if (p.app == AppState::Unsupported)
        shape = "Not supported on this computer";
    else if (p.app == AppState::Unavailable && !p.account[0])
        shape = "Signed out";
    else if (p.app == AppState::Unavailable)
        shape = "App closed; showing last reading";
    else if (reported) {
        const auto session = p.session.state;
        shape = session == AllowanceState::NoCap              ? "Weekly limit only"
                : session == AllowanceState::NotStarted       ? "Session not started"
                : session == AllowanceState::Unknown          ? "Session not reported"
                : session == AllowanceState::Unsupported      ? "Session unsupported"
                : p.weekly.state == AllowanceState::Unknown   ? "Weekly not reported"
                                                              : "Session and weekly limits";
    }
    c.text(40, y + 17, shape, font::size11, secondary);
    if (!reported)
        return;
    char age[20] = "--";
    char text[40];
    const auto observed = window.freshness.observation_age_ms(now);
    if (observed) {
        short_age(*observed, age, sizeof(age));
        std::snprintf(text, sizeof(text), "Read %s ago", age);
    } else {
        if (now >= window.freshness.received_ms)
            short_age(now - window.freshness.received_ms, age, sizeof(age));
        std::snprintf(text, sizeof(text), "Received %s ago", age);
    }
    c.right(304, y + 2, text, font::size11,
            window.freshness.known_stale(now) ? caution : secondary);
}

void about(Canvas& c, const State& state, uint64_t now) {
    c.text(16, 7, "Pipkin", font::size14, foreground);
    char version[48];
    const auto firmware = state.device_info.firmware_version;
    const auto revision = state.device_info.git_revision;
    std::snprintf(version, sizeof(version), "%.*s  %.*s",
                  static_cast<int>(std::min(firmware.size(), std::size_t{16})), firmware.data(),
                  static_cast<int>(std::min(revision.size(), std::size_t{12})), revision.data());
    c.text(24 + c.width("Pipkin", font::size14), 10, version, font::size11, secondary);

    const bool alive = host_alive(state, now);
    const char* host = state.host == HostState::Disconnected ? "Computer disconnected"
                       : !state.transport_connected            ? "Not connected"
                       : alive                                 ? "Connected"
                       : state.host == HostState::Asleep       ? "Computer asleep"
                                                               : "Connected, not confirmed";
    const auto dot = !linked(state) ? depleted : alive ? accent : caution;
    c.rect(16, 38, 288, 1, track);
    status_dot(c, 16, 59, dot);
    c.text(31, 52, host, font::size12, foreground);
    c.rect(16, 80, 288, 1, track);
    provider_status(c, state.providers[0], 0, 96, now);
    provider_status(c, state.providers[1], 1, 142, now);
    c.rect(16, 182, 288, 1, track);
    const int w = c.width("Run", font::size11) + c.width("for details", font::size11) +
                  c.width("pipkin status", font::size11) + 12 + 12;
    int x = 160 - w / 2;
    c.text(x, 200, "Run", font::size11, secondary);
    x += c.width("Run", font::size11) + 6;
    x += pill(c, x, 207, "pipkin status", foreground) + 6;
    c.text(x, 200, "for details", font::size11, secondary);
}

// Animation keys use milliseconds since boot, or since finish for the closing sequence.
constexpr float kBarLeft = 50, kBarTop = 150, kBarWidth = 220, kBarHeight = 12;
// Clip Pip behind the bar.
constexpr int kBarMiddle = 160;
constexpr float kPipRadius = 22;
// Stop the bar here while waiting; keep the blinks running.
constexpr uint64_t kBootHoldMs = 3680;
constexpr uint64_t kBootHintMs = 8000;
constexpr uint64_t kBootQrMs = kBootHintMs + 10000;
constexpr uint64_t kBlinkMs = 240;

struct Key {
    float at;
    float value;
};

float ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? 4 * t * t * t : 1 - (2 - 2 * t) * (2 - 2 * t) * (2 - 2 * t) / 2;
}

template <std::size_t N> float keyed(float t, const Key (&keys)[N]) {
    if (t <= keys[0].at)
        return keys[0].value;
    for (std::size_t i = 1; i < N; ++i)
        if (t < keys[i].at) {
            const auto& a = keys[i - 1];
            const auto& b = keys[i];
            return a.value + (b.value - a.value) * ease((t - a.at) / (b.at - a.at));
        }
    return keys[N - 1].value;
}

uint16_t fade(uint16_t color, float opacity) {
    return blend(background, color, static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 15 + 0.5f));
}

// Closed-lid factor for a blink centred at centre: 1 open, 0.1 at its narrowest.
float blink(uint64_t t, uint64_t centre) {
    const uint64_t distance = t > centre ? t - centre : centre - t;
    return distance >= kBlinkMs / 2 ? 1 : 1 - 0.9f * (1 - distance * 2.0f / kBlinkMs);
}

float boot_blink(uint64_t t) {
    float lid = std::min(blink(t, 1320), blink(t, 3200));
    if (t > 3200)
        lid = std::min(lid, blink((t - 3200) % 4000, 0));
    return lid;
}

void pip_eye(Canvas& c, float x, float top, float lid, float scale) {
    const float height = 11 * lid * scale;
    const float middle = top + 5.5f * scale;
    if (height < 5)
        c.line(x - 1.75f, middle, x + 1.75f, middle, std::max(height, 1.5f), background);
    else
        c.line(x, middle - (height - 5) / 2, x, middle + (height - 5) / 2, 5, background);
}

void happy_eye(Canvas& c, float x, float y) {
    Segment arc[4];
    float px = x, py = y;
    for (int i = 1; i <= 4; ++i) {
        const float t = i / 4.0f;
        const float nx = x + 9 * t;
        const float ny = y - 14 * t * (1 - t);
        arc[i - 1] = {px, py, nx, ny};
        px = nx;
        py = ny;
    }
    c.strokes(arc, 4, 2.6f, background);
}

void twinkle(Canvas& c, float x, float y, float half, float finish, float delay, uint16_t color) {
    const float t = finish - delay;
    const Key size[] = {{1680, 0.3f}, {2000, 1}, {2560, 0.6f}};
    const Key opacity[] = {{1680, 0}, {2000, 1}, {2560, 0}};
    const float alpha = keyed(t, opacity);
    if (alpha <= 0)
        return;
    const float h = half * keyed(t, size);
    const Segment plus[] = {{x, y - h, x, y + h}, {x - h, y, x + h, y}};
    c.strokes(plus, 2, 2.5f, fade(color, alpha));
}

void boot(Canvas& c, const State& state, uint64_t now) {
    const auto& b = state.boot;
    const uint64_t since = now > b.started_ms ? now - b.started_ms : 0;
    const float t = static_cast<float>(since);
    const auto finish = boot_finish_ms(b);
    const bool finishing = finish && now >= *finish;
    const float f = finishing ? static_cast<float>(now - *finish) : -1;

    // The bar starts 30 px wide so Pip has something to hold, and crawls while it waits.
    const Key filling[] = {{1280, 30}, {2720, 102}, {kBootHoldMs, 120}};
    float width = keyed(t, filling);
    if (finishing) {
        const float from = keyed(static_cast<float>(*finish - b.started_ms), filling);
        width = from + (kBarWidth - from) * ease(f / 1440);
    }
    const bool riding = finishing ? f < 1440 : t >= 1280 && t < 2720;
    const bool crawling = !finishing && t >= 2720 && t < 3520;

    const Key closing[] = {{3520, 1}, {kBootFinishMs, 0}};
    const float out = finishing ? keyed(f, closing) : 1;
    const Key rise[] = {{720, 40}, {1040, -4}, {1240, 0}};
    const Key hop[] = {{1520, 0},  {1920, -24}, {2080, -24}, {2400, 0},
                       {2600, -7}, {2800, 0},   {3200, 0},   {3680, 40}};
    const float lift = finishing ? keyed(f, hop) : keyed(t, rise);
    const Key squash[] = {{1440, 1}, {1600, 1.08f}, {1760, 1}, {2240, 1}, {2440, 1.08f}, {2640, 1}};
    const float wide = finishing ? keyed(f, squash) : 1;
    const float tall = 2 - wide;
    const Key cheer[] = {{1600, 0}, {1920, 1}, {2160, 1}, {2480, 0}};
    const float arms = finishing ? keyed(f, cheer) : 0;
    const bool happy = finishing && f >= 1600 && f < 2760;
    const float lid = finishing ? blink(static_cast<uint64_t>(f), 3080) : boot_blink(since);

    if (!finishing && !linked(state) && since >= kBootQrMs) {
        constexpr int scale = 3, quiet = 4, size = (setup_qr::kSize + 2 * quiet) * scale;
        constexpr int left = (kDisplayWidth - size) / 2, top = 8;
        c.rect(left, top, size, size, 0xffff);
        for (int row = 0; row < setup_qr::kSize; ++row)
            for (int col = 0; col < setup_qr::kSize; ++col)
                if (setup_qr::kRows[row] & (uint32_t{1} << col))
                    c.rect(left + (col + quiet) * scale, top + (row + quiet) * scale,
                           scale, scale, 0x0000);
    } else {
        const Key word_in[] = {{160, 0}, {720, 1}};
        const Key word_rise[] = {{160, 6}, {720, 0}};
        c.coverage_map(160 - wordmark::kWidth / 2,
                       88 - wordmark::kAscent + static_cast<int>(keyed(t, word_rise) + 0.5f),
                       wordmark::kWidth, wordmark::kHeight, wordmark::kCoverage,
                       fade(foreground, keyed(t, word_in) * out));
    }

    const float x = kBarLeft + width - 14;
    const float y = kBarTop + lift;
    Canvas behind{c.pixels, c.top, std::min(c.bottom, kBarMiddle)};
    behind.ellipse(x, y, kPipRadius * wide, kPipRadius * tall, fade(pip_body, out));
    if (happy) {
        happy_eye(behind, x - 11, y - 7);
        happy_eye(behind, x + 2, y - 7);
    } else {
        const float look_x = riding ? 2 : 0;
        const float look_y = crawling ? -2 : 0;
        const float top = y - 15 * tall + look_y;
        pip_eye(behind, x - 6.5f * wide + look_x, top, lid, tall);
        pip_eye(behind, x + 6.5f * wide + look_x, top, lid, tall);
    }

    const Key track_in[] = {{480, 0}, {800, 1}};
    const float bar_opacity = keyed(t, track_in) * out;
    c.round_rect(static_cast<int>(kBarLeft), static_cast<int>(kBarTop), static_cast<int>(kBarWidth),
                 static_cast<int>(kBarHeight), kBarHeight / 2, fade(track, bar_opacity));
    c.round_rect(static_cast<int>(kBarLeft), static_cast<int>(kBarTop),
                 static_cast<int>(width + 0.5f), static_cast<int>(kBarHeight), kBarHeight / 2,
                 fade(accent, bar_opacity));

    float left_hand = 0, right_hand = 0;
    if (riding) {
        const float wave = std::sin(2 * pi * static_cast<float>(since % 640) / 640);
        left_hand = -4 * std::max(0.0f, wave);
        right_hand = -4 * std::max(0.0f, -wave);
    }
    behind.ellipse(x - 15 - 9 * arms, kBarTop + 1 + lift + left_hand - 30 * arms, 5.5f, 5.5f,
                   fade(pip_hand, out));
    behind.ellipse(x + 15 + 9 * arms, kBarTop + 1 + lift + right_hand - 30 * arms, 5.5f, 5.5f,
                   fade(pip_hand, out));

    if (finishing) {
        twinkle(c, 226, 111, 5, f, 0, sparkle);
        twinkle(c, 288, 116, 4, f, 150, accent);
        twinkle(c, 274, 94, 4, f, 300, pip_body);
    }

    const float earlier = finishing ? 1 - std::clamp(f / 160, 0.0f, 1.0f) : 1;
    const Key starting[] = {{640, 0}, {960, 1}, {2560, 1}, {2720, 0}};
    const Key waiting[] = {{2720, 0}, {2960, 1}};
    const Key done[] = {{1520, 0}, {1760, 1}, {3360, 1}, {3680, 0}};
    if (const float a = keyed(t, starting) * earlier; a > 0)
        c.centered(160, 188, "Starting up", font::size12, fade(secondary, a));
    if (const float a = keyed(t, waiting) * earlier; a > 0)
        c.centered(160, 188, linked(state) ? "Waiting for usage" : "Looking for your computer",
                   font::size12, fade(secondary, a));
    if (!finishing && since >= kBootHintMs)
        c.centered(160, 208, setup_hint(state), font::size11, secondary);
    if (const float a = finishing ? keyed(f, done) : 0; a > 0)
        c.centered(160, 188, "All set", font::size12, fade(secondary, a));
}

bool boot_moving(const Boot& b, uint64_t now) {
    if (boot_finish_ms(b) || now < b.started_ms)
        return true;
    const uint64_t t = now - b.started_ms;
    return t < kBootHoldMs || boot_blink(t) < 1 || boot_blink(t + kBlinkMs / 2) < 1;
}
} // namespace

bool animation_active(const State& state, uint64_t now_ms) {
    if (state.overlay != Overlay::None || state.about_open)
        return false;
    if (state.boot.active)
        return boot_moving(state.boot, now_ms);
    if (revealing(state, now_ms))
        return true;
    for (const auto& provider : state.providers)
        if (changing(provider.session, now_ms) || changing(provider.weekly, now_ms))
            return true;
    return false;
}

void render(const State& state, uint64_t now_ms, uint16_t* pixels, int first_row, int rows) {
    if (!pixels || first_row < 0 || rows <= 0 || rows > kDisplayHeight - first_row)
        return;
    Canvas c{pixels, first_row, first_row + rows};
    c.rect(0, 0, kDisplayWidth, kDisplayHeight, background);
    if (state.overlay == Overlay::Sleep) {
        clock_text(c, state, now_ms, true);
        c.centered(160, 104, "Computer asleep", font::size14, foreground);
        c.centered(160, 128, "Pipkin wakes with it", font::size11, secondary);
        return;
    }
    if (state.about_open) {
        about(c, state, now_ms);
        clock_text(c, state, now_ms, false);
        return;
    }
    if (state.boot.active) {
        boot(c, state, now_ms);
        return;
    }
    const bool codex = provider_visible(state, Provider::Codex);
    const bool claude = provider_visible(state, Provider::Claude);
    const auto progress = reveal(state, now_ms);
    const bool overview = effective_page(state) == Page::Overview;
    if (!codex && !claude) {
        c.centered(160, 100, linked(state) ? "Waiting for usage" : "Waiting for your computer",
                   font::size14, foreground);
        c.centered(160, 124, setup_hint(state), font::size11, secondary);
    } else if (overview) {
        if (codex && claude) {
            provider_row(c, state, 0, 38, false, now_ms, progress);
            c.rect(16, kOverviewSplit, 288, 1, hairline);
            provider_row(c, state, 1, 134, false, now_ms, progress);
        } else
            provider_row(c, state, codex ? 0 : 1, 66, true, now_ms, progress);
        if (auto status = connection(state))
            c.right(304, 10, status, font::size11, caution);
    } else
        detail(c, state, now_ms, progress);
    clock_text(c, state, now_ms, overview || (!codex && !claude));
    if (codex || claude)
        page_indicator(c, state);
}
} // namespace pipkin
