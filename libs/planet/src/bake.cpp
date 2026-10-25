#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/bake.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <thread>

namespace hog::planet {

namespace {

constexpr std::array<std::byte, 8> magic{std::byte{'H'}, std::byte{'O'}, std::byte{'G'},
                                         std::byte{'L'}, std::byte{'A'}, std::byte{'N'},
                                         std::byte{'D'}, std::byte{'1'}};

template <typename Bake>
auto fields(Bake& bake) {
    return std::array{&bake.heights, &bake.change, &bake.drained, &bake.ground};
}

} // namespace

std::filesystem::path bake_path(const std::filesystem::path& directory, BakeName name) {
    return directory / std::format("landform-{:016x}-{:016x}.bin", name.world, name.code);
}

std::optional<LandformBake> read_bake(const std::filesystem::path& directory, BakeName name) {
    std::ifstream file{bake_path(directory, name), std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    std::vector<char> text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    ByteReader in{std::as_bytes(std::span{text})};
    const auto start = in.read_bytes(magic.size());
    if (!start || !std::ranges::equal(*start, magic)) {
        return std::nullopt;
    }
    const auto world = in.read<std::uint64_t>();
    const auto code = in.read<std::uint64_t>();
    const auto level = in.read_varint();
    const auto count = in.read_varint();
    // No more cells than there are bytes left to hold them.
    if (!world || *world != name.world || !code || *code != name.code || !level || *level > 30 ||
        !count || *count > in.remaining()) {
        return std::nullopt;
    }
    LandformBake bake{.level = static_cast<int>(*level),
                      .heights = {},
                      .change = {},
                      .drained = {},
                      .ground = {}};
    for (std::vector<std::int64_t>* field : fields(bake)) {
        field->reserve(*count);
        for (std::uint64_t cell = 0; cell < *count; ++cell) {
            const auto value = in.read_signed();
            if (!value) {
                return std::nullopt;
            }
            field->push_back(*value);
        }
    }
    if (!in.finish()) {
        return std::nullopt;
    }
    return bake;
}

bool write_bake(const std::filesystem::path& directory, BakeName name, const LandformBake& bake) {
    ByteWriter out;
    out.write_bytes(magic);
    out.write(name.world);
    out.write(name.code);
    out.write_varint(static_cast<std::uint64_t>(bake.level));
    out.write_varint(bake.heights.size());
    for (const std::vector<std::int64_t>* field : fields(bake)) {
        if (field->size() != bake.heights.size()) {
            return false;
        }
        for (const std::int64_t value : *field) {
            out.write_signed(value);
        }
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path path = bake_path(directory, name);
    // Written aside and renamed into place, so a reader never finds half of one; named apart
    // from any other writer's at the same time.
    std::filesystem::path partial = path;
    partial +=
        std::format(".{:016x}.partial",
                    hash_values(std::hash<std::thread::id>{}(std::this_thread::get_id()),
                                std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream file{partial, std::ios::binary | std::ios::trunc};
        const auto bytes = out.bytes();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streams write chars
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            std::filesystem::remove(partial, error);
            return false;
        }
    }
    std::filesystem::rename(partial, path, error);
    if (error) {
        std::filesystem::remove(partial, error);
        return false;
    }
    // Bakes by other code are never read again.
    const std::string world = std::format("landform-{:016x}-", name.world);
    for (const auto& entry : std::filesystem::directory_iterator{directory, error}) {
        const std::string other = entry.path().filename().string();
        if (other.starts_with(world) && other.ends_with(".bin") && entry.path() != path) {
            std::filesystem::remove(entry.path(), error);
        }
    }
    return true;
}

} // namespace hog::planet
