#include <hog/bake/bake.hpp>
#include <hog/baked/page.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/volume_build.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>

namespace hog::bake {
namespace {

constexpr int side = globe::Volume::tile_side;
constexpr int side_bits = 6;
static_assert(side == 1 << side_bits);

// The level of the blocks a bake at `level` makes, no coarser than a tile spanning a face.
std::uint8_t block_level(const globe::Generator& generator, std::uint8_t level) {
    return static_cast<std::uint8_t>(
        std::min<int>(level + block_above, globe::top_level(generator.layers())));
}

bool in_shell(const globe::Layers& layers, globe::TileKey key) {
    return static_cast<std::int64_t>(key.k) * (std::int64_t{side} << key.level) < layers.count;
}

// Every block over the face from (i, j) to (i_end, j_end), counted in blocks, up the shell.
std::vector<globe::TileKey> blocks_over(const globe::Layers& layers, std::uint8_t face,
                                        std::uint8_t level, std::uint32_t i, std::uint32_t j,
                                        std::uint32_t i_end, std::uint32_t j_end) {
    std::vector<globe::TileKey> blocks;
    for (std::uint32_t bj = j; bj < j_end; ++bj) {
        for (std::uint32_t bi = i; bi < i_end; ++bi) {
            for (globe::TileKey key{.level = level, .face = face, .i = bi, .j = bj};
                 in_shell(layers, key); ++key.k) {
                blocks.push_back(key);
            }
        }
    }
    return blocks;
}

std::uint32_t blocks_across(const globe::Layers& layers, std::uint8_t level) {
    return std::max<std::uint32_t>((std::uint32_t{1} << layers.level) >> (side_bits + level), 1U);
}

// Whether records at least as fine as `level` cover the whole block.
bool baked_already(const baked::Snapshot& snapshot, globe::TileKey block, std::uint8_t level) {
    const baked::Node node = node_of(block);
    std::uint64_t covered = 0;
    snapshot.each(node, [&](const baked::Record& record) {
        if (record.page.voxel() <= level) {
            covered += std::uint64_t{1} << (3U * std::min(record.node.size, node.size));
        }
    });
    return covered >= std::uint64_t{1} << (3U * node.size);
}

// Writes the blocks' pages in one batch, and the header first where one's given.
Result<void> write(baked::Store& store, std::span<const std::byte> header,
                   std::span<const globe::TileKey> blocks,
                   std::span<const std::vector<std::byte>> pages) {
    return store.write([&](baked::Batch& out) -> Result<void> {
        if (!header.empty()) {
            if (Result<void> set = out.set_header(header); !set) {
                return set;
            }
        }
        for (std::size_t n = 0; n < blocks.size(); ++n) {
            if (Result<void> put = out.put(node_of(blocks[n]), pages[n]); !put) {
                return put;
            }
        }
        return {};
    });
}

// Makes the tiles of one footprint, putting those done into the block's tree and the rest's
// children in the shell into `next`.
void make_footprint(const globe::Generator& generator, globe::TileKey block, std::uint8_t level,
                    std::span<const globe::TileKey> footprint, globe::VoxelTree& tree,
                    std::vector<globe::TileKey>& next) {
    const globe::Layers& layers = generator.layers();
    for (const globe::MadeTile& made : globe::generate_stack(generator, footprint)) {
        const globe::TileKey key = made.key;
        if (key.level > level && !made.hidden) {
            for (std::size_t octant = 0; octant < 8; ++octant) {
                if (const globe::TileKey child = key.child(octant); in_shell(layers, child)) {
                    next.push_back(child);
                }
            }
            continue;
        }
        const int up = key.level - level;
        const unsigned below = block.level - key.level;
        const auto corner = [&](std::uint32_t at, std::uint32_t block_at) {
            return static_cast<int>(at - (block_at << below)) * (side << up);
        };
        tree.graft(corner(key.i, block.i), corner(key.j, block.j), corner(key.k, block.k),
                   made.tree, 1 << up, globe::made_at(key.level));
    }
}

} // namespace

baked::Node node_of(globe::TileKey key) noexcept {
    const unsigned size = side_bits + key.level;
    return {.face = key.face,
            .size = static_cast<std::uint8_t>(size),
            .x = key.i << size,
            .y = key.j << size,
            .z = key.k << size};
}

std::vector<globe::TileKey> face_blocks(const globe::Generator& generator, std::uint8_t face,
                                        std::uint8_t level) {
    const globe::Layers& layers = generator.layers();
    const std::uint8_t blocks = block_level(generator, level);
    const std::uint32_t across = blocks_across(layers, blocks);
    return blocks_over(layers, face, blocks, 0, 0, across, across);
}

std::vector<globe::TileKey> blocks_around(const globe::Generator& generator, DVec3 direction,
                                          double metres, std::uint8_t level) {
    const globe::Layers& layers = generator.layers();
    const std::uint8_t blocks = block_level(generator, level);
    const std::uint32_t across = blocks_across(layers, blocks);
    const globe::Cell cell = globe::cell_under(glm::normalize(direction), layers.level);
    // Cells are widest at a face's middle, a quarter turn of the planet across the face.
    const double cell_width = generator.planet().body.radius * (std::numbers::pi / 2.0) /
                              static_cast<double>(std::uint64_t{1} << layers.level);
    const auto half = static_cast<std::uint32_t>(std::ceil(metres / cell_width / 2.0));
    const unsigned shift = side_bits + blocks;
    const auto low = [&](std::uint32_t at) { return (at - std::min(at, half)) >> shift; };
    const auto high = [&](std::uint32_t at) {
        return std::min(((at + half) >> shift) + 1, across);
    };
    return blocks_over(layers, cell.face, blocks, low(cell.i), low(cell.j), high(cell.i),
                       high(cell.j));
}

globe::VoxelTree make_block(const globe::Generator& generator, globe::TileKey block,
                            std::uint8_t level) {
    globe::VoxelTree tree{side << (block.level - level), globe::Cube{}, baked::unknown};
    std::vector<globe::TileKey> frontier{block};
    while (!frontier.empty()) {
        std::ranges::sort(frontier);
        std::vector<globe::TileKey> next;
        // Sorted, the tiles over one footprint lie together.
        for (std::size_t from = 0; from < frontier.size();) {
            std::size_t to = from + 1;
            while (to < frontier.size() && frontier[to].i == frontier[from].i &&
                   frontier[to].j == frontier[from].j) {
                ++to;
            }
            make_footprint(generator, block, level, std::span{frontier}.subspan(from, to - from),
                           tree, next);
            from = to;
        }
        frontier = std::move(next);
    }
    return tree;
}

Result<Baked> bake(const globe::Generator& generator, baked::Store& store, jobs::JobSystem& jobs,
                   std::span<const globe::TileKey> blocks, std::uint8_t level, std::size_t batch,
                   const Progress& progress) {
    const std::vector<std::byte> header = globe::encode_planet(generator.planet());
    const Result<std::vector<std::byte>> held = store.header();
    if (!held) {
        return std::unexpected{held.error()};
    }
    if (!held->empty() && *held != header) {
        return make_error(ErrorCode::InvalidArgument, "the store was baked from another planet");
    }

    Baked baked;
    std::vector<globe::TileKey> wanted;
    {
        Result<baked::Snapshot> snapshot = store.read();
        if (!snapshot) {
            return std::unexpected{snapshot.error()};
        }
        for (const globe::TileKey block : blocks) {
            if (baked_already(*snapshot, block, level)) {
                ++baked.skipped;
            } else {
                wanted.push_back(block);
            }
        }
    }

    batch = std::max<std::size_t>(batch, 1);
    std::vector<std::vector<std::byte>> pages;
    for (std::size_t from = 0; from < wanted.size(); from += batch) {
        const std::size_t count = std::min(batch, wanted.size() - from);
        pages.assign(count, {});
        jobs.parallel_for(static_cast<std::uint32_t>(count), 1,
                          [&](jobs::Range range, std::uint32_t) {
                              for (std::uint32_t n = range.begin; n < range.end; ++n) {
                                  const globe::TileKey block = wanted[from + n];
                                  pages[n] = baked::encode_tree(make_block(generator, block, level),
                                                                node_of(block).size, level, 0);
                              }
                          });
        const Result<void> written =
            write(store, held->empty() ? std::span{header} : std::span<const std::byte>{},
                  std::span{wanted}.subspan(from, count), pages);
        if (!written) {
            return std::unexpected{written.error()};
        }
        for (const std::vector<std::byte>& page : pages) {
            baked.bytes += page.size();
        }
        baked.made += count;
        if (progress) {
            progress(from + count, wanted.size());
        }
    }
    return baked;
}

} // namespace hog::bake
