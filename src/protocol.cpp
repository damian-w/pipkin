#include "pipkin/model.h"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>

namespace pipkin {
namespace {

constexpr int64_t kFirstEpoch = 1577836800;
constexpr int64_t kLastEpoch = 4102444800;

struct Token {
    std::string_view key;
    std::string_view value;
};

struct Packet {
    std::array<Token, 32> tokens{};
    std::size_t count = 0;

    std::optional<std::string_view> get(std::string_view key) const {
        for (std::size_t i = 0; i < count; ++i)
            if (tokens[i].key == key)
                return tokens[i].value;
        return std::nullopt;
    }

    bool only(std::initializer_list<std::string_view> keys) const {
        for (std::size_t i = 0; i < count; ++i)
            if (std::find(keys.begin(), keys.end(), tokens[i].key) == keys.end())
                return false;
        return true;
    }
};

bool tokenize(std::string_view line, Packet& packet) {
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
    }
    if (line.empty() || line.size() > kMaxPacketBytes)
        return false;
    for (const char c : line)
        if (c < 32 || c > 126)
            return false;
    while (!line.empty()) {
        if (packet.count == packet.tokens.size())
            return false;
        const auto space = line.find(' ');
        const auto token = line.substr(0, space);
        const auto equals = token.find('=');
        if (equals == 0 || equals == token.npos || equals + 1 == token.size() ||
            token.find('=', equals + 1) != token.npos)
            return false;
        const auto key = token.substr(0, equals);
        if (packet.get(key))
            return false;
        packet.tokens[packet.count++] = {key, token.substr(equals + 1)};
        if (space == line.npos)
            break;
        line.remove_prefix(space + 1);
        if (line.empty())
            return false;
    }
    return true;
}

template <typename T>
bool number(std::optional<std::string_view> text, T minimum, T maximum, T& value) {
    if (!text || text->empty())
        return false;
    T parsed{};
    const auto result = std::from_chars(text->data(), text->data() + text->size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text->data() + text->size() || parsed < minimum ||
        parsed > maximum)
        return false;
    value = parsed;
    return true;
}

bool identifier(std::string_view text, std::array<char, 33>& output) {
    if (text.empty() || text.size() > 32 || text == "null")
        return false;
    for (const char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return false;
    output.fill(0);
    std::copy(text.begin(), text.end(), output.begin());
    return true;
}

bool provider(const Packet& packet, std::size_t& index) {
    const auto value = packet.get("provider");
    if (value == "codex")
        index = 0;
    else if (value == "claude")
        index = 1;
    else
        return false;
    return true;
}

bool freshness(Freshness& result, const Freshness& previous, std::optional<int64_t> observed,
               const State& state, uint64_t now_ms) {
    const bool same_epoch = previous.observation_epoch == state.clock.observation_epoch;
    const auto watermark = same_epoch ? previous.last_observed_unix : std::nullopt;
    if (observed && watermark && *observed < *watermark)
        return false;
    Freshness previous_watermark = previous;
    previous_watermark.age_on_receipt_ms =
        same_epoch ? previous.last_observed_age_ms : std::nullopt;
    const auto previous_age = previous_watermark.observation_age_ms(now_ms);
    result = Freshness{};
    result.observed_unix = observed;
    result.received_ms = now_ms;
    result.observation_epoch = state.clock.observation_epoch;
    const auto now = unix_now(state, now_ms);
    if (now && observed) {
        if (*observed > *now + 30)
            return false;
        result.age_on_receipt_ms =
            static_cast<uint64_t>(std::max<int64_t>(0, *now - *observed)) * 1000;
    }
    if (observed && observed == watermark && previous_age)
        result.age_on_receipt_ms = std::max(*previous_age, result.age_on_receipt_ms.value_or(0));
    result.last_observed_unix = observed ? observed : watermark;
    result.last_observed_age_ms = observed ? result.age_on_receipt_ms : previous_age;
    return true;
}

bool window(const Packet& packet, bool session, Window& result, const Window& previous,
            std::optional<int64_t> observed, const State& state, uint64_t now_ms) {
    const auto status = packet.get(session ? "session" : "weekly");
    const auto id = packet.get(session ? "session_id" : "weekly_id");
    const auto used = packet.get(session ? "session_used" : "weekly_used");
    const auto reset = packet.get(session ? "session_reset" : "weekly_reset");
    const auto seconds = packet.get(session ? "session_seconds" : "weekly_seconds");
    if (!status)
        return !id && !used && !reset && !seconds;
    result = Window{};
    if (!freshness(result.freshness, previous.freshness, observed, state, now_ms))
        return false;
    if (status == "null")
        return !id && !used && !reset && !seconds;
    if (status == "metered")
        result.state = AllowanceState::Metered;
    else if (status == "no_cap" && session)
        result.state = AllowanceState::NoCap;
    else if (status == "not_started")
        result.state = AllowanceState::NotStarted;
    else if (status == "unsupported")
        result.state = AllowanceState::Unsupported;
    else
        return false;
    if (result.state == AllowanceState::NoCap || result.state == AllowanceState::Unsupported)
        return !id && !used && !reset && !seconds;
    if (!id || !identifier(*id, result.id))
        return false;
    if (result.state == AllowanceState::Metered) {
        uint16_t value = 0;
        if (!number<uint16_t>(used, 0, 1000, value))
            return false;
        result.used_tenths = value;
        if (previous.state == AllowanceState::Metered && previous.used_tenths &&
            shown_used(previous, now_ms) != value) {
            result.from_used_tenths = shown_used(previous, now_ms);
            result.changed_ms = now_ms;
        }
    } else if (used)
        return false;
    if (reset && reset != "null") {
        int64_t value = 0;
        if (!number<int64_t>(reset, kFirstEpoch, kLastEpoch, value))
            return false;
        result.reset_unix = value;
    }
    if (seconds && seconds != "null") {
        uint32_t value = 0;
        if (!number<uint32_t>(seconds, 1, 366 * 24 * 60 * 60, value))
            return false;
        result.duration_seconds = value;
    }
    return true;
}

bool usage(const Packet& packet, State& state, uint64_t now_ms) {
    if (!packet.only({"v", "kind", "seq", "provider", "mode", "account", "observed", "clock_epoch",
                      "session", "session_id", "session_used", "session_reset", "session_seconds",
                      "weekly", "weekly_id", "weekly_used", "weekly_reset", "weekly_seconds",
                      "banked"}))
        return false;
    std::size_t index = 0;
    if (!provider(packet, index))
        return false;
    const auto mode = packet.get("mode");
    if (mode != "full" && mode != "patch")
        return false;
    const auto account = packet.get("account");
    if (!account)
        return false;
    auto& data = state.providers[index];
    if (account == "null") {
        if (mode != "full" || !packet.only({"v", "kind", "seq", "provider", "mode", "account"}))
            return false;
        data = ProviderData{};
        data.app = AppState::Unavailable;
        data.app_received_ms = now_ms;
        return true;
    }
    std::array<char, 33> account_id{};
    if (!identifier(*account, account_id))
        return false;
    const bool same_account = data.account == account_id;
    if (mode == "patch" && !same_account)
        return false;
    uint64_t observation_epoch = 0;
    if ((packet.get("clock_epoch") &&
         !number<uint64_t>(packet.get("clock_epoch"), 0, std::numeric_limits<uint64_t>::max(),
                           observation_epoch)) ||
        observation_epoch != state.clock.observation_epoch)
        return false;
    std::optional<int64_t> observed;
    if (packet.get("observed") != "null") {
        int64_t value = 0;
        if (!number<int64_t>(packet.get("observed"), kFirstEpoch, kLastEpoch, value))
            return false;
        observed = value;
    }
    const auto now = unix_now(state, now_ms);
    if (now && observed && *observed > *now + 30)
        return false;
    const ProviderData previous = same_account ? data : ProviderData{};
    if (mode == "full") {
        data = ProviderData{};
        if (!freshness(data.session.freshness, previous.session.freshness, observed, state,
                       now_ms) ||
            !freshness(data.weekly.freshness, previous.weekly.freshness, observed, state, now_ms) ||
            !freshness(data.banked_freshness, previous.banked_freshness, observed, state, now_ms))
            return false;
        if (same_account) {
            data.app = previous.app;
            data.app_received_ms = previous.app_received_ms;
        }
    }
    data.account = account_id;
    if (!window(packet, true, data.session, previous.session, observed, state, now_ms) ||
        !window(packet, false, data.weekly, previous.weekly, observed, state, now_ms))
        return false;
    const auto banked = packet.get("banked");
    if (banked) {
        data.banked_resets.reset();
        data.banked_freshness = Freshness{};
        if (!freshness(data.banked_freshness, previous.banked_freshness, observed, state, now_ms))
            return false;
        if (banked != "null") {
            uint32_t value = 0;
            if (!number<uint32_t>(banked, 0, 1000000, value))
                return false;
            data.banked_resets = value;
        }
    }
    return true;
}

bool apply(const Packet& packet, State& state, uint64_t now_ms) {
    const auto kind = packet.get("kind");
    if (kind == "usage")
        return usage(packet, state, now_ms);
    if (kind == "clock") {
        if (!packet.only({"v", "kind", "seq", "unix", "tz", "rebase"}))
            return false;
        int64_t epoch = 0;
        int16_t offset = 0;
        if (!number<int64_t>(packet.get("unix"), kFirstEpoch, kLastEpoch, epoch) ||
            !number<int16_t>(packet.get("tz"), -840, 840, offset))
            return false;
        const auto rebase = packet.get("rebase");
        if (rebase && rebase != "1")
            return false;
        const auto previous = unix_now(state, now_ms);
        if (previous && (epoch < *previous - 300 || epoch > *previous + 300) && !rebase)
            return false;
        uint64_t observation_epoch = state.clock.observation_epoch;
        if (rebase && !number<uint64_t>(packet.get("seq"), 1, std::numeric_limits<uint64_t>::max(),
                                        observation_epoch))
            return false;
        state.clock = {epoch, now_ms, offset, observation_epoch};
        return true;
    }
    if (kind == "host") {
        if (!packet.only({"v", "kind", "seq", "state"}))
            return false;
        const auto status = packet.get("state");
        if (status == "awake")
            state.host = HostState::Awake;
        else if (status == "asleep")
            state.host = HostState::Asleep;
        else if (status == "disconnected")
            state.host = HostState::Disconnected;
        else
            return false;
        state.host_received_ms = now_ms;
        if (state.host == HostState::Asleep && state.overlay == Overlay::None)
            state.overlay = Overlay::Sleep;
        else if (state.host != HostState::Asleep && state.overlay == Overlay::Sleep)
            state.overlay = Overlay::None;
        return true;
    }
    if (kind == "app") {
        if (!packet.only({"v", "kind", "seq", "provider", "state"}))
            return false;
        std::size_t index = 0;
        if (!provider(packet, index))
            return false;
        auto& data = state.providers[index];
        const auto status = packet.get("state");
        if (status == "available")
            data.app = AppState::Available;
        else if (status == "unavailable")
            data.app = AppState::Unavailable;
        else if (status == "unsupported")
            data.app = AppState::Unsupported;
        else if (status == "signed_out") {
            data = ProviderData{};
            data.app = AppState::Unavailable;
        } else
            return false;
        data.app_received_ms = now_ms;
        return true;
    }
    return false;
}

} // namespace

bool ingest(State& state, std::string_view line, uint64_t now_ms) {
    Packet packet;
    uint64_t sequence = 0;
    if (!tokenize(line, packet) || packet.get("v") != "1" || now_ms < state.last_received_ms)
        return false;
    // Sequence is optional so stateless senders need no identify round trip; usage ordering is
    // enforced independently by per-group observation watermarks.
    const bool sequenced = packet.get("seq").has_value();
    if (sequenced &&
        (!number<uint64_t>(packet.get("seq"), 1, std::numeric_limits<uint64_t>::max(), sequence) ||
         sequence <= state.last_sequence))
        return false;
    State next = state;
    if (next.host == HostState::Disconnected) {
        next.host = HostState::Unknown;
        next.host_received_ms.reset();
    }
    next.transport_connected = true;
    if (!apply(packet, next, now_ms))
        return false;
    if ((state.page == Page::Codex && provider_visible(state, Provider::Codex) &&
         !provider_visible(next, Provider::Codex)) ||
        (state.page == Page::Claude && provider_visible(state, Provider::Claude) &&
         !provider_visible(next, Provider::Claude)))
        next.page = selected_page(next);
    if (effective_page(next) != effective_page(state)) {
        next.page_entered_ms = now_ms;
        ++next.view_revision;
    }
    if (next.host == HostState::Disconnected)
        next.transport_connected = false;
    if (sequenced)
        next.last_sequence = sequence;
    next.last_received_ms = now_ms;
    state = next;
    return true;
}

} // namespace pipkin
