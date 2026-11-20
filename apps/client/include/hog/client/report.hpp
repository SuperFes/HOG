#pragma once

#include <hog/client/block_meshes.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/piece_asking.hpp>
#include <hog/core/log.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// A report of what the client shows, taken on a key, with all it holds about the view so what
// was seen can be made again off line: the picture, where the eye was, the pieces held near it as
// the chunks they came in, what's asked for and what's drawn, and the lines logged lately.
namespace hog::client {

// The lines logged lately, kept to go in a report.
class RecentLog final : public log::Sink {
public:
    explicit RecentLog(std::size_t most = 500) noexcept : most_{most} {}
    void write(const log::Record& record) override;
    [[nodiscard]] std::vector<std::string> lines() const;

private:
    std::size_t most_;
    mutable std::mutex lock_;
    std::deque<std::string> lines_;
};

// Where a report was taken and how things stood, each a name and its value.
struct ReportAbout {
    DVec3 eye{0.0};
    DVec3 forward{0.0};
    DVec3 up{0.0};
    std::optional<DVec3> feet;
    std::vector<std::pair<std::string, std::string>> lines;
};

// What goes in a report besides the picture.
struct Report {
    ReportAbout about;
    const HeldPieces* held = nullptr;
    double reach = 0.0; // metres from the eye pieces are kept within
    std::span<const PieceWish> asked;
    std::span<const PieceWish> wanted;
    std::span<const DrawnBlock> drawn;
    std::span<const std::string> log;
};

// The files a report is written as, in its directory.
inline constexpr std::string_view report_about = "about.txt";
inline constexpr std::string_view report_pieces = "pieces.bin";
inline constexpr std::string_view report_bodies = "bodies.bin";
inline constexpr std::string_view report_asked = "asked.txt";
inline constexpr std::string_view report_drawn = "drawn.txt";
inline constexpr std::string_view report_log = "log.txt";
inline constexpr std::string_view report_picture = "picture.png";

// Writes `report` into `directory`, making it.
[[nodiscard]] Result<void> write_report(const std::filesystem::path& directory,
                                        const Report& report);

// The chunks a report's pieces came in, to take in again.
[[nodiscard]] Result<std::vector<protocol::SentChunk>>
read_report_chunks(const std::filesystem::path& directory);

// Takes a report's pieces and water bodies into `held`, as they were held when it was taken.
[[nodiscard]] Result<void> read_report(const std::filesystem::path& directory, HeldPieces& held);

} // namespace hog::client
