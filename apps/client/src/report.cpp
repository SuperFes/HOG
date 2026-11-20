#include <hog/client/report.hpp>
#include <hog/core/bytes.hpp>
#include <hog/core/text_file.hpp>

#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>

namespace hog::client {
namespace {

// What the chunk file starts with, so another file isn't read as one.
constexpr std::uint32_t pieces_mark = 0x504f'4748; // "HGOP"

Result<void> write_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()), // NOLINT(*-reinterpret-cast)
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        return make_error(ErrorCode::Io, std::format("can't write {}", path.string()));
    }
    return {};
}

Result<void> write_text(const std::filesystem::path& path, const std::string& text) {
    return write_file(path, std::as_bytes(std::span{text}));
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::error_code failed;
    const std::uintmax_t size = std::filesystem::file_size(path, failed);
    std::ifstream in{path, std::ios::binary};
    if (failed || !in) {
        return make_error(ErrorCode::NotFound, std::format("can't read {}", path.string()));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), // NOLINT(*-reinterpret-cast)
                 static_cast<std::streamsize>(size))) {
        return make_error(ErrorCode::Io, std::format("can't read all of {}", path.string()));
    }
    return bytes;
}

std::string key_text(grid::Key key) {
    return std::format("{} {} {} {}", key.depth, key.x, key.y, key.z);
}

std::string wishes_text(std::string_view title, std::span<const PieceWish> wishes) {
    std::string text =
        std::format("{} ({}): node depth x y z, to depth, priority\n", title, wishes.size());
    for (const PieceWish& wish : wishes) {
        text += std::format("{} {} {:.6g}\n", key_text(wish.node), wish.depth, wish.priority);
    }
    return text;
}

} // namespace

void RecentLog::write(const log::Record& record) {
    std::string line = log::format_line(record);
    const std::scoped_lock guard{lock_};
    lines_.push_back(std::move(line));
    while (lines_.size() > most_) {
        lines_.pop_front();
    }
}

std::vector<std::string> RecentLog::lines() const {
    const std::scoped_lock guard{lock_};
    return {lines_.begin(), lines_.end()};
}

Result<void> write_report(const std::filesystem::path& directory, const Report& report) {
    std::error_code made;
    std::filesystem::create_directories(directory, made);
    if (made) {
        return make_error(ErrorCode::Io,
                          std::format("can't make {}: {}", directory.string(), made.message()));
    }
    const ReportAbout& about = report.about;
    std::string text;
    text += std::format("eye = {:.17g} {:.17g} {:.17g}\n", about.eye.x, about.eye.y, about.eye.z);
    text += std::format("forward = {:.17g} {:.17g} {:.17g}\n", about.forward.x, about.forward.y,
                        about.forward.z);
    text += std::format("up = {:.17g} {:.17g} {:.17g}\n", about.up.x, about.up.y, about.up.z);
    if (about.feet) {
        text += std::format("feet = {:.17g} {:.17g} {:.17g}\n", about.feet->x, about.feet->y,
                            about.feet->z);
    }
    text += std::format("pieces within = {:.17g}\n", report.reach);
    for (const auto& [name, value] : about.lines) {
        text += std::format("{} = {}\n", name, value);
    }
    if (auto written = write_text(directory / report_about, text); !written) {
        return written;
    }

    if (report.held != nullptr) {
        const std::vector<protocol::SentChunk> chunks =
            report.held->chunks_near(about.eye, report.reach);
        ByteWriter out;
        out.write(pieces_mark);
        out.write(static_cast<std::uint32_t>(chunks.size()));
        for (const protocol::SentChunk& chunk : chunks) {
            out.write(chunk.kind);
            out.write(chunk.band);
            out.write(chunk.depth);
            out.write(chunk.x);
            out.write(chunk.y);
            out.write(chunk.z);
            out.write(static_cast<std::uint32_t>(chunk.tree.size()));
            out.write_bytes(chunk.tree);
        }
        if (auto written = write_file(directory / report_pieces, std::move(out).take()); !written) {
            return written;
        }
        if (auto written = write_file(directory / report_bodies, report.held->bodies().encode());
            !written) {
            return written;
        }
    }

    if (auto written =
            write_text(directory / report_asked, wishes_text("asked last", report.asked) + "\n" +
                                                     wishes_text("wanted now", report.wanted));
        !written) {
        return written;
    }

    std::string drawn = std::format(
        "drawn ({}): node depth x y z, cells, finer sides, ground and water triangles, pieces "
        "read\n",
        report.drawn.size());
    for (const DrawnBlock& block : report.drawn) {
        drawn += std::format("{} {} {} {} {} |", key_text(block.block.node), block.block.cells,
                             block.block.finer, block.ground_triangles, block.water_triangles);
        for (const grid::Key root : block.reads) {
            drawn += " " + key_text(root) + ";";
        }
        drawn += '\n';
    }
    if (auto written = write_text(directory / report_drawn, drawn); !written) {
        return written;
    }

    std::string lines;
    for (const std::string& line : report.log) {
        lines += line + "\n";
    }
    return write_text(directory / report_log, lines);
}

Result<std::vector<protocol::SentChunk>>
read_report_chunks(const std::filesystem::path& directory) {
    auto bytes = read_file(directory / report_pieces);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    ByteReader in{*bytes};
    const auto mark = in.read<std::uint32_t>();
    if (!mark || *mark != pieces_mark) {
        return make_error(ErrorCode::Parse, "not a report's pieces");
    }
    const auto count = in.read<std::uint32_t>();
    if (!count) {
        return std::unexpected(count.error());
    }
    std::vector<protocol::SentChunk> chunks;
    for (std::uint32_t n = 0; n < *count; ++n) {
        const auto kind = in.read<std::uint8_t>();
        const auto band = in.read<std::uint8_t>();
        const auto depth = in.read<std::uint8_t>();
        const auto x = in.read<std::uint32_t>();
        const auto y = in.read<std::uint32_t>();
        const auto z = in.read<std::uint32_t>();
        const auto size = in.read<std::uint32_t>();
        if (!kind || !band || !depth || !x || !y || !z || !size) {
            return make_error(ErrorCode::Parse, "a report's pieces cut short");
        }
        const auto tree = in.read_bytes(*size);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        chunks.push_back({.kind = *kind,
                          .band = *band,
                          .depth = *depth,
                          .x = *x,
                          .y = *y,
                          .z = *z,
                          .version = 1,
                          .tree = {tree->begin(), tree->end()}});
    }
    if (auto finished = in.finish(); !finished) {
        return std::unexpected(finished.error());
    }
    return chunks;
}

Result<void> read_report(const std::filesystem::path& directory, HeldPieces& held) {
    auto chunks = read_report_chunks(directory);
    if (!chunks) {
        return std::unexpected(chunks.error());
    }
    // Shallower bands first, as they came, so each nests as it did.
    std::ranges::stable_sort(*chunks, {}, &protocol::SentChunk::band);
    for (const protocol::SentChunk& chunk : *chunks) {
        if (auto taken = held.take(chunk); !taken) {
            return taken;
        }
    }
    if (auto bodies = read_file(directory / report_bodies); bodies) {
        if (auto taken = held.take_bodies(*bodies); !taken) {
            return taken;
        }
    }
    return {};
}

} // namespace hog::client
