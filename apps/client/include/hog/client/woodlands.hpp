#pragma once

#include <hog/flora/fates.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/protocol/messages.hpp>

#include <memory>
#include <vector>

namespace hog::client {

// How the woods fare, as the server last told: what the trees and the far woods are grown from,
// untouched until it tells. Each word replaces the fates whole, so what's in use stays as it was.
class Woodlands {
public:
    // Takes the server's word. Returns the cells at its level whose fates it changed, with those
    // beside each, since the fates are read between neighbouring cells.
    std::vector<planet::CellId> receive(const protocol::Woodlands& told);

    [[nodiscard]] const std::shared_ptr<const flora::Fates>& fates() const noexcept {
        return fates_;
    }

private:
    std::shared_ptr<const flora::Fates> fates_ = std::make_shared<const flora::Fates>();
};

} // namespace hog::client
