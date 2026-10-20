#pragma once

#include <hog/protocol/messages.hpp>
#include <hog/sim/forest.hpp>

namespace hog::server {

// How the woods fare, as players are sent them: the forest's own cells, or the cells as fine as
// may be sent, each a mean of those under it weighted by the plants each holds. Where nothing
// grows, the woods are as untouched.
[[nodiscard]] protocol::Woodlands woodlands_report(const sim::Forest& forest);

} // namespace hog::server
