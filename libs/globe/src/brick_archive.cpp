#include <hog/core/bytes.hpp>
#include <hog/globe/brick_archive.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

namespace hog::globe {

Result<void> MemoryBrickArchive::write(std::span<const std::byte> header,
                                       std::span<const SavedBrick> bricks,
                                       std::span<const std::uint64_t> erased) {
    header_.assign(header.begin(), header.end());
    for (const SavedBrick& brick : bricks) {
        bricks_.insert_or_assign(brick.key, brick.bytes);
    }
    for (const std::uint64_t key : erased) {
        bricks_.erase(key);
    }
    return {};
}

Result<BrickArchive::Contents> MemoryBrickArchive::load() const {
    Contents contents{.header = header_, .bricks = {}};
    contents.bricks.reserve(bricks_.size());
    for (const auto& [key, bytes] : bricks_) {
        contents.bricks.push_back({.key = key, .bytes = bytes});
    }
    return contents;
}

namespace {

std::vector<std::byte> header_for(const Layers& layers) {
    ByteWriter out;
    out.write(brick_format);
    out.write(static_cast<std::uint8_t>(layers.level));
    out.write(static_cast<std::uint32_t>(layers.count));
    out.write(std::bit_cast<std::uint64_t>(layers.floor));
    out.write(std::bit_cast<std::uint64_t>(layers.thickness));
    return std::move(out).take();
}

Result<Layers> layers_from(std::span<const std::byte> header) {
    ByteReader in{header};
    const auto format = in.read<std::uint16_t>();
    if (!format) {
        return std::unexpected(format.error());
    }
    if (*format != brick_format) {
        return make_error(ErrorCode::Unsupported,
                          std::format("bricks saved in format {}, not {}", *format, brick_format));
    }
    const auto level = in.read<std::uint8_t>();
    const auto count = in.read<std::uint32_t>();
    const auto floor = in.read<std::uint64_t>();
    const auto thickness = in.read<std::uint64_t>();
    if (!level || !count || !floor || !thickness) {
        return make_error(ErrorCode::Parse, "a brick header cut short");
    }
    if (auto finished = in.finish(); !finished) {
        return std::unexpected(finished.error());
    }
    return Layers{.level = *level,
                  .floor = std::bit_cast<double>(*floor),
                  .thickness = std::bit_cast<double>(*thickness),
                  .count = static_cast<int>(*count)};
}

} // namespace

SavedBrick coded_brick(const VoxelStore& store, BrickSite site) {
    SavedBrick coded{.key = brick_key(site), .bytes = {}};
    if (const Brick* brick = store.brick(site)) {
        ByteWriter out;
        brick->write(out);
        coded.bytes = std::move(out).take();
    }
    return coded;
}

Result<void> take_brick(VoxelStore& store, const SavedBrick& coded) {
    const BrickSite site = brick_at_key(coded.key);
    if (brick_key(site) != coded.key || !store.holds_site(site)) {
        return make_error(ErrorCode::Parse, std::format("no brick {:#x}", coded.key));
    }
    if (coded.bytes.empty()) {
        store.erase(site);
        return {};
    }
    ByteReader in{coded.bytes};
    auto brick = Brick::read(in);
    if (!brick) {
        return std::unexpected(std::move(brick.error()));
    }
    if (auto finished = in.finish(); !finished) {
        return std::unexpected(finished.error());
    }
    store.put(site, *std::move(brick));
    return {};
}

Result<std::size_t> save(VoxelStore& store, BrickArchive& archive) {
    const std::vector<BrickSite> changed = store.changed();
    std::vector<SavedBrick> bricks;
    std::vector<std::uint64_t> erased;
    for (const BrickSite site : changed) {
        if (store.brick(site) != nullptr) {
            bricks.push_back(coded_brick(store, site));
        } else {
            erased.push_back(brick_key(site));
        }
    }
    if (auto written = archive.write(header_for(store.layers()), bricks, erased); !written) {
        return std::unexpected(std::move(written.error()));
    }
    store.clear_changed();
    return changed.size();
}

Result<VoxelStore> load(const Layers& layers, const BrickArchive& archive) {
    auto contents = archive.load();
    if (!contents) {
        return std::unexpected(std::move(contents.error()));
    }
    VoxelStore store{layers};
    if (contents->header.empty()) {
        if (!contents->bricks.empty()) {
            return make_error(ErrorCode::Parse, "bricks saved without a header");
        }
        return store;
    }
    const auto saved_on = layers_from(contents->header);
    if (!saved_on) {
        return std::unexpected(saved_on.error());
    }
    if (*saved_on != layers) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("bricks saved on {} layers of {} m on level {}, not {} of "
                                      "{} m on level {}",
                                      saved_on->count, saved_on->thickness, saved_on->level,
                                      layers.count, layers.thickness, layers.level));
    }
    for (const SavedBrick& saved : contents->bricks) {
        if (saved.bytes.empty()) {
            return make_error(ErrorCode::Parse, std::format("brick {:#x} saved empty", saved.key));
        }
        if (auto taken = take_brick(store, saved); !taken) {
            return std::unexpected(std::move(taken.error()));
        }
    }
    // What was loaded is saved already.
    store.clear_changed();
    return store;
}

} // namespace hog::globe
