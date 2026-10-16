#include <hog/net/events.hpp>

#include <steam/steamnetworkingtypes.h>

namespace hog::net {

Message& Message::operator=(Message&& other) noexcept {
    if (this != &other) {
        if (message_ != nullptr) {
            message_->Release();
        }
        message_ = std::exchange(other.message_, nullptr);
    }
    return *this;
}

Message::~Message() {
    if (message_ != nullptr) {
        message_->Release();
    }
}

ConnectionId Message::from() const noexcept {
    return ConnectionId{message_->m_conn};
}

Delivery Message::delivery() const noexcept {
    if (message_->m_idxLane == 1) {
        return Delivery::Bulk;
    }
    return (message_->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0 ? Delivery::Reliable
                                                                       : Delivery::Unreliable;
}

std::span<const std::byte> Message::bytes() const noexcept {
    return {static_cast<const std::byte*>(message_->m_pData),
            static_cast<std::size_t>(message_->m_cbSize)};
}

} // namespace hog::net
