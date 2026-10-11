#include <hog/planet/cell_id.hpp>

namespace hog::planet {

std::string to_string(CellId cell) {
    if (!cell.is_valid()) {
        return "invalid";
    }
    const int level = cell.level();
    std::string text(static_cast<std::size_t>(level) + 2, '/');
    text[0] = static_cast<char>('0' + cell.face());
    for (int at = level; at > 0; --at) {
        const auto pair = (cell.raw() >> (61 - (2 * at))) & 3U;
        text[static_cast<std::size_t>(at) + 1] = static_cast<char>('0' + pair);
    }
    return text;
}

} // namespace hog::planet
