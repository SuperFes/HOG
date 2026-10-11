#pragma once

#include <hog/core/hash.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace hog::testing {

// An empty directory, removed with everything in it when the test ends.
class TemporaryDirectory {
public:
    TemporaryDirectory() : path_{std::filesystem::temp_directory_path() / unique_name()} {
        std::filesystem::create_directories(path_);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    // Tests run as separate processes in parallel, so the time keeps names apart between them.
    static std::string unique_name() {
        static std::atomic<std::uint64_t> counter{0};
        return std::format(
            "hog-test-{:016x}",
            hash_values(static_cast<std::uint64_t>(
                            std::chrono::steady_clock::now().time_since_epoch().count()),
                        counter.fetch_add(1)));
    }

    std::filesystem::path path_;
};

} // namespace hog::testing
