#include <hog/planet/continents.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using hog::planet::CellId;

// Hashes of generated tiles, checked against tests/golden/tiles.txt on every platform. After an
// intended change to generation, rewrite the file with: hog_planet_tests "[.update-golden]"
namespace {

constexpr std::uint64_t seed = 1;

struct Layer {
    std::string_view name;
    std::int64_t (*sample)(CellId);
};

std::int64_t noise_layer(CellId cell) {
    return hog::planet::fbm(seed, hog::planet::noise_point(hog::planet::cell_center(cell), 4.0), 8);
}

std::int64_t continents_layer(CellId cell) {
    static const hog::planet::Continents continents{seed, 0.7};
    return continents.height(cell);
}

constexpr std::array layers{Layer{.name = "noise", .sample = noise_layer},
                            Layer{.name = "continents", .sample = continents_layer}};

struct GoldenTile {
    CellId tile;
    int depth = 0;
};

// Every face whole, then small deep tiles at a cube corner, at the north pole and mid-face.
std::vector<GoldenTile> golden_tiles() {
    std::vector<GoldenTile> tiles;
    tiles.reserve(hog::planet::face_count + 3);
    for (int face = 0; face < hog::planet::face_count; ++face) {
        tiles.push_back({.tile = CellId::from_face(face), .depth = 5});
    }
    tiles.push_back({.tile = CellId::from_face_ij(2, 0, 0, 12), .depth = 4});
    tiles.push_back({.tile = CellId::from_face_ij(2, 8'191, 8'191, 14), .depth = 4});
    tiles.push_back({.tile = CellId::from_face_ij(1, 123'456, 654'321, 20), .depth = 4});
    return tiles;
}

std::vector<std::string> generate_lines() {
    std::vector<std::string> lines{"# layer tile depth hash"};
    for (const Layer& layer : layers) {
        for (const GoldenTile& golden : golden_tiles()) {
            const auto samples = hog::planet::sample_tile(golden.tile, golden.depth, layer.sample);
            lines.push_back(std::format("{} {} {} {:016x}", layer.name, golden.tile, golden.depth,
                                        hog::planet::tile_hash(std::span{samples})));
        }
    }
    return lines;
}

std::filesystem::path golden_path() {
    return std::filesystem::path{HOG_SOURCE_DIR} / "libs/planet/tests/golden/tiles.txt";
}

// Tolerates CRLF checkouts.
std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

TEST_CASE("generated tiles match the golden hashes", "[planet][golden]") {
    const auto expected = read_lines(golden_path());
    REQUIRE_FALSE(expected.empty());
    const auto actual = generate_lines();
    for (std::size_t line = 0; line < std::max(expected.size(), actual.size()); ++line) {
        INFO("line " << line + 1);
        CHECK((line < expected.size() ? expected[line] : "") ==
              (line < actual.size() ? actual[line] : ""));
    }
}

TEST_CASE("rewrite the golden hashes", "[.update-golden]") {
    std::ostringstream text;
    for (const auto& line : generate_lines()) {
        text << line << '\n';
    }
    std::ofstream file{golden_path(), std::ios::binary};
    file << text.str();
    REQUIRE(file.good());
}
