#include "held_chunks.hpp"

#include <hog/client/held_pieces.hpp>
#include <hog/client/report.hpp>
#include <hog/core/log.hpp>
#include <hog/core/text_file.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using hog::client::HeldPieces;
using hog::grid::Key;

namespace {

hog::grid::Piece piece_at(Key root, std::uint8_t levels, std::uint16_t material) {
    hog::grid::Piece piece;
    piece.root = root;
    piece.generator = 1;
    piece.material =
        hog::grid::Tree<hog::grid::Material>::build(levels, [&](int x, int /*y*/, int /*z*/) {
            return static_cast<std::uint16_t>(material + x);
        });
    piece.water = hog::grid::Tree<hog::grid::Water>{levels, 0, false};
    return piece;
}

bool contains(const std::string& text, const std::string& part) {
    return text.contains(part);
}

} // namespace

TEST_CASE("a report holds the pieces near the eye as they were held", "[client][report]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    HeldPieces held{grid};
    const Key near{.depth = 4, .x = 8, .y = 8, .z = 8};
    const Key far{.depth = 4, .x = 0, .y = 0, .z = 0};
    REQUIRE(hog::testing::hold(held, piece_at(Key{}, 4, 10), 0).has_value());
    REQUIRE(hog::testing::hold(held, piece_at(near, 3, 20), 1).has_value());
    REQUIRE(hog::testing::hold(held, piece_at(far, 3, 30), 1).has_value());
    const hog::DVec3 eye = grid.centre(near);
    const std::vector<hog::client::PieceWish> asked{
        {.node = near.child(1), .depth = 9, .priority = 0.5}};
    const std::vector<std::string> log{"a line logged", "another"};
    const hog::client::ReportAbout about{.eye = eye,
                                         .forward = {1.0, 0.0, 0.0},
                                         .up = {0.0, 0.0, 1.0},
                                         .feet = std::nullopt,
                                         .lines = {{"at", "1.5,2.5,30"}}};
    const hog::testing::TemporaryDirectory directory;
    const hog::client::Report report{.about = about,
                                     .held = &held,
                                     .reach = 200.0,
                                     .asked = asked,
                                     .wanted = {},
                                     .drawn = {},
                                     .log = log};
    REQUIRE(hog::client::write_report(directory.path(), report).has_value());

    // Taken in again, the pieces near the eye are held as they were, and none far from it.
    HeldPieces again{grid};
    REQUIRE(hog::client::read_report(directory.path(), again).has_value());
    CHECK(again.pieces() == 2);
    for (const Key root : {Key{}, near}) {
        CAPTURE(root.depth, root.x);
        REQUIRE(again.piece(root) != nullptr);
        CHECK(again.piece(root)->material.value().same_as(held.piece(root)->material.value()));
        CHECK(again.reach(root) == held.reach(root));
    }
    CHECK(again.piece(far) == nullptr);

    const auto text = [&](std::string_view name) {
        auto read = hog::read_text_file(directory.path() / std::string{name});
        REQUIRE(read.has_value());
        return *read;
    };
    CHECK(contains(text(hog::client::report_about), "at = 1.5,2.5,30"));
    CHECK(contains(text(hog::client::report_asked), "5 17 16 16 9"));
    CHECK(contains(text(hog::client::report_log), "another"));
}

TEST_CASE("the lines logged lately are kept, the oldest let go", "[client][report]") {
    hog::client::RecentLog recent{3};
    const hog::log::Category category{"report"};
    for (int n = 0; n < 5; ++n) {
        const std::string message = "line " + std::to_string(n);
        recent.write({.level = hog::log::Level::Info,
                      .category = category.name(),
                      .message = message,
                      .time = std::chrono::system_clock::now()});
    }
    const std::vector<std::string> lines = recent.lines();
    REQUIRE(lines.size() == 3);
    CHECK(contains(lines.front(), "line 2"));
    CHECK(contains(lines.back(), "line 4"));
}
