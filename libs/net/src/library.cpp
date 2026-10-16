#include "library.hpp"

#include <hog/core/log.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <unordered_map>
#include <utility>

namespace hog::net::detail {

namespace {

const log::Category net_log{"net"};

// The library is process-wide: initialised by the first owner, shut down with the last.
struct Registry {
    std::mutex mutex;
    int users = 0;
    std::int64_t next_id = 1;
    std::unordered_map<std::int64_t, Owner*> owners;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

void on_status(SteamNetConnectionStatusChangedCallback_t* status) {
    Registry& owners = registry();
    const std::scoped_lock lock{owners.mutex};
    const auto found = owners.owners.find(status->m_info.m_nUserData);
    if (found != owners.owners.end()) {
        found->second->push({.connection = status->m_hConn,
                             .state = status->m_info.m_eState,
                             .reason = std::data(status->m_info.m_szEndDebug)});
    }
}

void on_output(ESteamNetworkingSocketsDebugOutputType type, const char* text) {
    const log::Level level = type <= k_ESteamNetworkingSocketsDebugOutputType_Error
                                 ? log::Level::Error
                                 : log::Level::Warn;
    log::message(level, net_log, "{}", text);
}

} // namespace

Result<std::unique_ptr<Owner>> Owner::create() {
    Registry& owners = registry();
    const std::scoped_lock lock{owners.mutex};
    if (owners.users == 0) {
        SteamNetworkingErrMsg error{};
        if (!GameNetworkingSockets_Init(nullptr, error)) {
            return make_error(ErrorCode::Io,
                              std::format("couldn't start networking: {}", std::data(error)));
        }
        ISteamNetworkingUtils& utils = *SteamNetworkingUtils();
        utils.SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Warning, on_output);
        utils.SetGlobalCallback_SteamNetConnectionStatusChanged(on_status);
        // Encrypted, but no one's identity is checked here: the handshake's token does that.
        utils.SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
    }
    ++owners.users;
    const std::int64_t id = owners.next_id++;
    std::unique_ptr<Owner> owner{new Owner(id)};
    owners.owners.emplace(id, owner.get());
    return owner;
}

Owner::~Owner() {
    Registry& owners = registry();
    const std::scoped_lock lock{owners.mutex};
    owners.owners.erase(id_);
    if (--owners.users == 0) {
        GameNetworkingSockets_Kill();
    }
}

std::vector<Change> Owner::changes() {
    sockets().RunCallbacks();
    const std::scoped_lock lock{mutex_};
    return std::exchange(changes_, {});
}

void Owner::push(Change change) {
    const std::scoped_lock lock{mutex_};
    changes_.push_back(std::move(change));
}

SteamNetworkingConfigValue_t user_data(const Owner& owner) {
    return {.m_eValue = k_ESteamNetworkingConfig_ConnectionUserData,
            .m_eDataType = k_ESteamNetworkingConfig_Int64,
            .m_val = {.m_int64 = owner.id()}};
}

void configure_lanes(HSteamNetConnection connection) {
    constexpr std::array<int, 2> priorities{0, 1};
    constexpr std::array<uint16, 2> weights{1, 1};
    sockets().ConfigureConnectionLanes(connection, static_cast<int>(priorities.size()),
                                       priorities.data(), weights.data());
}

Result<void> send(HSteamNetConnection connection, Delivery delivery,
                  std::span<const std::byte> bytes) {
    if (bytes.size() > max_message_size) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("a message of {} bytes is over the {} allowed", bytes.size(),
                                      max_message_size));
    }
    SteamNetworkingMessage_t* message =
        SteamNetworkingUtils()->AllocateMessage(static_cast<int>(bytes.size()));
    if (!bytes.empty()) {
        std::memcpy(message->m_pData, bytes.data(), bytes.size());
    }
    message->m_conn = connection;
    switch (delivery) {
    case Delivery::Unreliable:
        message->m_nFlags = k_nSteamNetworkingSend_UnreliableNoNagle;
        break;
    case Delivery::Reliable:
        message->m_nFlags = k_nSteamNetworkingSend_ReliableNoNagle;
        break;
    case Delivery::Bulk:
        // Bulk can wait for a few to share a packet.
        message->m_nFlags = k_nSteamNetworkingSend_Reliable;
        message->m_idxLane = 1;
        break;
    }
    int64 result = 0;
    // The library takes the message, sent or not.
    sockets().SendMessages(1, &message, &result, true);
    if (result < 0) {
        return make_error(ErrorCode::Io,
                          std::format("couldn't send a message: result {}", -result));
    }
    return {};
}

std::optional<Stats> stats(HSteamNetConnection connection) {
    SteamNetConnectionRealTimeStatus_t status{};
    if (sockets().GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK) {
        return std::nullopt;
    }
    return Stats{.ping = std::chrono::milliseconds{status.m_nPing},
                 .delivered_here = status.m_flConnectionQualityLocal,
                 .delivered_there = status.m_flConnectionQualityRemote,
                 .bytes_in_per_second = status.m_flInBytesPerSec,
                 .bytes_out_per_second = status.m_flOutBytesPerSec,
                 .queued_bytes = static_cast<std::size_t>(status.m_cbPendingReliable) +
                                 static_cast<std::size_t>(status.m_cbPendingUnreliable)};
}

} // namespace hog::net::detail
