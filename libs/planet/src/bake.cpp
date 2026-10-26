#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/bake.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <stop_token>
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

std::filesystem::path named_bakes() {
#ifdef _MSC_VER
    char* named = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&named, &length, "HOG_BAKES") != 0 || named == nullptr) {
        return {};
    }
    std::filesystem::path found{named};
    std::free(named);
    return found;
#else
    // Nothing sets the environment once running.
    const char* const named = std::getenv("HOG_BAKES"); // NOLINT(concurrency-mt-unsafe)
    return named != nullptr ? std::filesystem::path{named} : std::filesystem::path{};
#endif
}

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

BakeClaim::BakeClaim(const std::filesystem::path& directory, BakeName name,
                     const std::function<void()>& waiting, ClaimTiming timing)
    : claim_{bake_path(directory, name).replace_extension(".claim")} {
    const std::filesystem::path bake = bake_path(directory, name);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    bool told = false;
    while (true) {
        if (std::filesystem::exists(bake, error)) {
            return;
        }
        // Making a directory either makes it or finds it made, in one step everywhere.
        if (std::filesystem::create_directory(claim_, error)) {
            held_ = true;
            break;
        }
        if (error) {
            // Nowhere to keep it, so nothing to wait for.
            return;
        }
        const auto marked = std::filesystem::last_write_time(claim_, error);
        if (!error && std::filesystem::file_time_type::clock::now() - marked > timing.abandoned) {
            std::filesystem::remove(claim_, error);
            continue;
        }
        if (!told && waiting) {
            waiting();
            told = true;
        }
        std::this_thread::sleep_for(timing.poll);
    }
    beating_ = std::jthread{[claim = claim_, beat = timing.beat](const std::stop_token& stop) {
        std::mutex mutex;
        std::condition_variable_any woken;
        std::unique_lock lock{mutex};
        // Waking for a stop returns the predicate, so it must be what ends the loop.
        while (!woken.wait_for(lock, stop, beat, [&stop] { return stop.stop_requested(); })) {
            std::error_code ignored;
            std::filesystem::last_write_time(claim, std::filesystem::file_time_type::clock::now(),
                                             ignored);
        }
    }};
}

BakeClaim::~BakeClaim() {
    if (held_) {
        beating_.request_stop();
        beating_.join();
        std::error_code error;
        std::filesystem::remove(claim_, error);
    }
}

} // namespace hog::planet
