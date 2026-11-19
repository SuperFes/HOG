#pragma once

#include <hog/client/held_pages.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/view.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::client {

// A page to ask the server for, how many levels under its root, and how big it looks from the
// eye: a node's size over its distance, the finest size added so the page holding the eye has one,
// less where it lies behind where the eye looks.
struct PageWish {
    grid::Key page;
    std::uint8_t levels = 0;
    double priority = 0.0;

    friend constexpr bool operator==(const PageWish&, const PageWish&) noexcept = default;
};

// How much less a page wholly behind where the eye looks is wanted than one ahead.
inline constexpr double behind_the_eye = 0.25;

// The pages to ask for to see the world from `eye`, metres from the planet's centre, looking
// along `facing` where given, that `held` lacks or holds shallower than they should be seen:
// the top page, and under each open leaf of a page held whole the pages there, wherever its finest
// nodes look coarser than `view` allows, each as deep as its nodes should be seen. The pages
// seen through are those held, so what's under a page comes once it's held. Biggest looking
// first, `most` at most.
[[nodiscard]] std::vector<PageWish> pages_to_ask(const HeldPages& held, DVec3 eye,
                                                 std::optional<DVec3> facing,
                                                 const grid::View& view, std::size_t most);

// The wishes as asked for over the wire.
[[nodiscard]] protocol::Ask ask_of(const std::vector<PageWish>& wishes);

} // namespace hog::client
