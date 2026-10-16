#include <hog/net/simulation.hpp>

#include <steam/isteamnetworkingutils.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace hog::net {

namespace {

// The library's limits.
constexpr int most_lag = 5'000;
constexpr int most_jitter = 2'000;
constexpr int most_jitter_spike = 5'000;
// How far beyond the mean a random jitter may reach.
constexpr int jitter_spread = 4;
// Long enough to pass the next couple of snapshots at 20 a second.
constexpr int reorder_hold = 100;

std::optional<int> integer(std::string_view text) {
    int value = 0;
    const char* const last = std::to_address(text.end());
    const auto [end, error] = std::from_chars(std::to_address(text.begin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace

void simulate(const Simulation& simulation) {
    ISteamNetworkingUtils& utils = *SteamNetworkingUtils();
    const auto lag = static_cast<int32>(simulation.lag.count());
    const auto jitter = static_cast<float>(simulation.jitter.count());
    const auto spike = std::min(jitter * jitter_spread, static_cast<float>(most_jitter_spike));
    const auto loss = static_cast<float>(simulation.loss);
    const auto reorder = static_cast<float>(simulation.reorder);
    for (const auto [lag_key, jitter_key, spike_key, odds_key, loss_key, reorder_key] :
         {std::array{k_ESteamNetworkingConfig_FakePacketLag_Send,
                     k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg,
                     k_ESteamNetworkingConfig_FakePacketJitter_Send_Max,
                     k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct,
                     k_ESteamNetworkingConfig_FakePacketLoss_Send,
                     k_ESteamNetworkingConfig_FakePacketReorder_Send},
          std::array{k_ESteamNetworkingConfig_FakePacketLag_Recv,
                     k_ESteamNetworkingConfig_FakePacketJitter_Recv_Avg,
                     k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max,
                     k_ESteamNetworkingConfig_FakePacketJitter_Recv_Pct,
                     k_ESteamNetworkingConfig_FakePacketLoss_Recv,
                     k_ESteamNetworkingConfig_FakePacketReorder_Recv}}) {
        utils.SetGlobalConfigValueInt32(lag_key, lag);
        utils.SetGlobalConfigValueFloat(jitter_key, jitter);
        utils.SetGlobalConfigValueFloat(spike_key, spike);
        utils.SetGlobalConfigValueFloat(odds_key, 100.0F);
        utils.SetGlobalConfigValueFloat(loss_key, loss);
        utils.SetGlobalConfigValueFloat(reorder_key, reorder);
    }
    utils.SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, reorder_hold);
}

Result<Simulation> parse_simulation(std::string_view text) {
    Simulation simulation;
    if (text.empty()) {
        return simulation;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = std::min(text.find(',', start), text.size());
        const std::string_view part = text.substr(start, comma - start);
        start = comma + 1;
        const std::size_t equals = part.find('=');
        const std::string_view name = part.substr(0, equals);
        const auto value =
            equals == std::string_view::npos ? std::nullopt : integer(part.substr(equals + 1));
        const auto bad = [&](int most, std::string_view unit) {
            return make_error(
                ErrorCode::InvalidArgument,
                std::format("{} must be from 0 to {}{}, as in {}=10", name, most, unit, name));
        };
        if (name == "lag") {
            if (!value || *value < 0 || *value > most_lag) {
                return bad(most_lag, " ms");
            }
            simulation.lag = std::chrono::milliseconds{*value};
        } else if (name == "jitter") {
            if (!value || *value < 0 || *value > most_jitter) {
                return bad(most_jitter, " ms");
            }
            simulation.jitter = std::chrono::milliseconds{*value};
        } else if (name == "loss" || name == "reorder") {
            if (!value || *value < 0 || *value > 100) {
                return bad(100, "%");
            }
            (name == "loss" ? simulation.loss : simulation.reorder) = *value;
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{} isn't one of lag, jitter, loss and reorder", part));
        }
    }
    return simulation;
}

std::string describe(const Simulation& simulation) {
    std::vector<std::string> parts;
    if (simulation.lag.count() != 0) {
        parts.push_back(std::format("lag={}", simulation.lag.count()));
    }
    if (simulation.jitter.count() != 0) {
        parts.push_back(std::format("jitter={}", simulation.jitter.count()));
    }
    if (simulation.loss != 0) {
        parts.push_back(std::format("loss={}", simulation.loss));
    }
    if (simulation.reorder != 0) {
        parts.push_back(std::format("reorder={}", simulation.reorder));
    }
    std::string text;
    for (const std::string& part : parts) {
        text += text.empty() ? part : "," + part;
    }
    return text;
}

} // namespace hog::net
