#include <hog/chat/filter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace hog::chat {

namespace {

// A word looked for; `alone` ones only clear of letters, so innocent words holding them pass.
struct Looked {
    std::string_view word;
    bool alone = false;
};

// Hate speech, replaced whole whatever the filter says.
constexpr std::array slurs{
    Looked{.word = "nigger"},
    Looked{.word = "nigga"},
    Looked{.word = "faggot"},
    Looked{.word = "kike"},
    Looked{.word = "wetback"},
    Looked{.word = "beaner"},
    Looked{.word = "towelhead"},
    Looked{.word = "raghead"},
    Looked{.word = "fag", .alone = true},
    Looked{.word = "spic", .alone = true},
    Looked{.word = "gook", .alone = true},
    Looked{.word = "coon", .alone = true},
    Looked{.word = "tranny", .alone = true},
};

// Cussing, grawlixed when the player keeps the filter on.
constexpr std::array cussing{
    Looked{.word = "fuck"},
    Looked{.word = "shit"},
    Looked{.word = "bitch"},
    Looked{.word = "asshole"},
    Looked{.word = "bollock"},
    Looked{.word = "ass", .alone = true},
    Looked{.word = "arse", .alone = true},
    Looked{.word = "cunt", .alone = true},
    Looked{.word = "dick", .alone = true},
    Looked{.word = "cock", .alone = true},
    Looked{.word = "piss", .alone = true},
    Looked{.word = "twat", .alone = true},
    Looked{.word = "wank", .alone = true},
    Looked{.word = "prick", .alone = true},
    Looked{.word = "slut", .alone = true},
    Looked{.word = "whore", .alone = true},
    Looked{.word = "bastard", .alone = true},
    Looked{.word = "tits", .alone = true},
};

// Endings an alone word may wear and still be itself, longest first so none is half-taken.
constexpr std::array endings{"ing", "ers", "er", "es", "ed", "s", ""};

// Words cussing welds itself to; a compound of them still counts as standing alone, so
// "dickbutt" hides no better than "dick", while "Scunthorpe" and "class" still pass.
constexpr std::array compounds{"head",  "butt", "face",  "hole",  "hat",  "wad",
                               "bag",   "wipe", "stain", "weed",  "lips", "dumb",
                               "smart", "jack", "fat",   "horse", "bull", "lard"};

// What a hateful line reads as instead.
constexpr std::array small_talk{
    "lovely weather we're having, isn't it?",   "my, the clouds look fantastic today.",
    "nothing beats a warm breeze off the sea.", "I do enjoy a light drizzle.",
    "I hear it might rain on Tuesday.",         "what a sunset that was last night.",
};

constexpr std::string_view grawlix = "@#$%&!";

[[nodiscard]] constexpr bool letter(char character) noexcept {
    return character >= 'a' && character <= 'z';
}

[[nodiscard]] constexpr char lowered(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

// Digits and signs leant on to spell a letter anyway.
[[nodiscard]] constexpr char unleet(char character) noexcept {
    switch (character) {
    case '0':
        return 'o';
    case '1':
        return 'i';
    case '3':
        return 'e';
    case '4':
        return 'a';
    case '5':
        return 's';
    case '6':
    case '9':
        return 'g';
    case '7':
        return 't';
    case '8':
        return 'b';
    case '@':
        return 'a';
    case '$':
        return 's';
    default:
        return character;
    }
}

struct Decoded {
    char32_t point = 0;
    std::size_t length = 1;
};

[[nodiscard]] Decoded decode_utf8(std::string_view text, std::size_t at) noexcept {
    const auto first = static_cast<unsigned char>(text[at]);
    if (first < 0x80U) {
        return {.point = first, .length = 1};
    }
    const std::size_t length = first >= 0xF0U ? 4 : first >= 0xE0U ? 3 : first >= 0xC0U ? 2 : 1;
    if (length == 1 || at + length > text.size()) {
        return {.point = 0xFFFD, .length = 1};
    }
    char32_t point = first & (0xFFU >> (length + 1));
    for (std::size_t follow = 1; follow < length; ++follow) {
        const auto cont = static_cast<unsigned char>(text[at + follow]);
        if ((cont & 0xC0U) != 0x80U) {
            return {.point = 0xFFFD, .length = 1};
        }
        point = (point << 6U) | (cont & 0x3FU);
    }
    return {.point = point, .length = length};
}

// Letters of other alphabets posing as Latin ones.
[[nodiscard]] char posed_elsewhere(char32_t point) noexcept {
    switch (point) {
    // Cyrillic.
    case 0x430:
    case 0x410:
        return 'a';
    case 0x435:
    case 0x415:
        return 'e';
    case 0x43E:
    case 0x41E:
        return 'o';
    case 0x440:
    case 0x420:
        return 'p';
    case 0x441:
    case 0x421:
        return 'c';
    case 0x443:
    case 0x423:
        return 'y';
    case 0x445:
    case 0x425:
        return 'x';
    case 0x456:
        return 'i';
    case 0x455:
        return 's';
    case 0x458:
        return 'j';
    case 0x412:
        return 'b';
    case 0x41A:
        return 'k';
    case 0x41C:
        return 'm';
    case 0x41D:
        return 'h';
    case 0x422:
        return 't';
    // Greek.
    case 0x3B1:
    case 0x391:
        return 'a';
    case 0x3BF:
    case 0x39F:
        return 'o';
    case 0x3B9:
    case 0x399:
        return 'i';
    case 0x3BD:
        return 'v';
    case 0x3C1:
    case 0x3A1:
        return 'p';
    case 0x3C5:
        return 'u';
    case 0x3A5:
        return 'y';
    case 0x3BA:
    case 0x39A:
        return 'k';
    case 0x3C4:
    case 0x3A4:
        return 't';
    case 0x392:
        return 'b';
    case 0x395:
        return 'e';
    case 0x397:
        return 'h';
    case 0x39C:
        return 'm';
    case 0x39D:
        return 'n';
    case 0x3A7:
        return 'x';
    default:
        return ' ';
    }
}

// The one ASCII character a codepoint reads as, '\0' for one that's only invisible glue between
// letters, or a stand-in separator for the rest of Unicode.
[[nodiscard]] char posed(char32_t point) noexcept {
    if (point == 0xAD || point == 0x2060 || point == 0xFEFF ||
        (point >= 0x200B && point <= 0x200F)) {
        return '\0';
    }
    if (point >= 0xC0 && point <= 0xFF) {
        // Latin letters wearing marks, folded by their place in Latin-1.
        constexpr std::string_view latin = "aaaaaaaceeeeiiiidnooooo ouuuuyps"
                                           "aaaaaaaceeeeiiiidnooooo ouuuuypy";
        return latin[point - 0xC0];
    }
    if (point >= 0xFF21 && point <= 0xFF3A) {
        return static_cast<char>('a' + (point - 0xFF21));
    }
    if (point >= 0xFF41 && point <= 0xFF5A) {
        return static_cast<char>('a' + (point - 0xFF41));
    }
    return posed_elsewhere(point);
}

// The text as it reads for matching, folded to lowercase ASCII with lookalikes as the letters
// they pose as, and where each piece of it began in the original.
struct Shadow {
    std::string read;
    std::vector<std::size_t> origin;
};

[[nodiscard]] Shadow shadow_of(std::string_view text) {
    Shadow shadow;
    shadow.read.reserve(text.size());
    shadow.origin.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const auto byte = static_cast<unsigned char>(text[at]);
        char read = 0;
        std::size_t length = 1;
        if (byte < 0x80U) {
            read = unleet(lowered(static_cast<char>(byte)));
        } else {
            const Decoded decoded = decode_utf8(text, at);
            length = decoded.length;
            read = posed(decoded.point);
        }
        if (read != '\0') {
            shadow.read.push_back(read);
            shadow.origin.push_back(at);
        }
        at += length;
    }
    return shadow;
}

[[nodiscard]] bool letter_at(std::string_view read, std::size_t at) noexcept {
    return at < read.size() && letter(read[at]);
}

// Reads `word` at `start` of the shadow, letting each letter stretch ("fuuuck") and separators
// fall between letters ("f u c k"); where it ends, or nothing. `split` says separators were
// leant on, which the caller holds to a stricter standard.
[[nodiscard]] std::optional<std::size_t> read_word(std::string_view read, std::size_t start,
                                                   std::string_view word, bool& split) {
    std::size_t at = start;
    std::size_t index = 0;
    split = false;
    while (index < word.size()) {
        const char expect = word[index];
        std::size_t need = 0;
        while (index < word.size() && word[index] == expect) {
            ++need;
            ++index;
        }
        if (at != start) {
            while (at < read.size() && !letter(read[at])) {
                ++at;
                split = true;
            }
        }
        std::size_t got = 0;
        while (true) {
            while (at < read.size() && read[at] == expect) {
                ++got;
                ++at;
            }
            // The letter may run on across separators too: "a s s".
            std::size_t over = at;
            while (over < read.size() && !letter(read[over])) {
                ++over;
            }
            if (got >= need || over == at || !letter_at(read, over) || read[over] != expect) {
                break;
            }
            split = true;
            at = over;
        }
        if (got < need) {
            return std::nullopt;
        }
    }
    return at;
}

struct Span {
    std::size_t start = 0;
    std::size_t end = 0; // in the shadow, one past
};

// A compound part of the shadow ending at `at`, if one does; how long it is, else nothing.
[[nodiscard]] std::optional<std::size_t> part_before(std::string_view read, std::size_t at) {
    for (const std::string_view part : compounds) {
        if (part.size() <= at && read.compare(at - part.size(), part.size(), part) == 0) {
            return part.size();
        }
    }
    return std::nullopt;
}

// An alone word may wear compound parts and then an ending, but must then stand clear of
// letters; where it closes, or nothing.
[[nodiscard]] std::optional<std::size_t> closed_alone(std::string_view read, std::size_t end) {
    for (bool grew = true; grew;) {
        grew = false;
        for (const std::string_view part : compounds) {
            if (read.compare(end, part.size(), part) == 0) {
                end += part.size();
                grew = true;
                break;
            }
        }
    }
    for (const std::string_view ending : endings) {
        if (read.compare(end, ending.size(), ending) == 0 &&
            !letter_at(read, end + ending.size())) {
            return end + ending.size();
        }
    }
    return std::nullopt;
}

// A guarded match at [start, end) of the shadow counts when it stands clear of letters, in a
// compound ("dumbass", "dickbutt") or out of one; the whole of what it stood in, or nothing.
[[nodiscard]] std::optional<Span> stood_alone(std::string_view read, std::size_t start,
                                              std::size_t end) {
    std::size_t begin = start;
    while (begin > 0 && letter(read[begin - 1])) {
        const auto part = part_before(read, begin);
        if (!part) {
            return std::nullopt;
        }
        begin -= *part;
    }
    const auto close = closed_alone(read, end);
    if (!close) {
        return std::nullopt;
    }
    return Span{.start = begin, .end = *close};
}

// Where words from `list` show through the shadow.
[[nodiscard]] std::vector<Span> find_all(std::string_view read, std::span<const Looked> list) {
    std::vector<Span> found;
    for (const Looked& looked : list) {
        for (std::size_t start = 0; start < read.size(); ++start) {
            if (read[start] != looked.word.front()) {
                continue;
            }
            bool split = false;
            const auto end = read_word(read, start, looked.word, split);
            if (!end) {
                continue;
            }
            // A match that leant on separators must stand alone, so "his hit song" is no match
            // for anything, while "f u c k" on its own still is.
            if (looked.alone || split) {
                if (const auto span = stood_alone(read, start, *end)) {
                    found.push_back(*span);
                }
                continue;
            }
            // Found wherever it appears, its endings going with it, so nothing readable is left
            // of "fucking".
            std::size_t close = *end;
            while (letter_at(read, close)) {
                ++close;
            }
            found.push_back({.start = start, .end = close});
        }
    }
    return found;
}

// Masks the spans in the original text, each with the comic-book squiggles.
void mask(std::string& text, const Shadow& shadow, std::span<const Span> spans) {
    for (const Span& span : spans) {
        const std::size_t from = shadow.origin[span.start];
        const std::size_t to =
            span.end < shadow.origin.size() ? shadow.origin[span.end] : text.size();
        for (std::size_t at = from; at < to; ++at) {
            text[at] = grawlix[(at - from) % grawlix.size()];
        }
    }
}

} // namespace

bool hateful(std::string_view text) {
    const Shadow shadow = shadow_of(text);
    return !find_all(shadow.read, slurs).empty();
}

std::string small_talk_for(std::string_view text) {
    return std::string{small_talk[std::hash<std::string_view>{}(text) % small_talk.size()]};
}

std::string bleeped(std::string_view text) {
    const Shadow shadow = shadow_of(text);
    std::string shown{text};
    mask(shown, shadow, find_all(shadow.read, cussing));
    return shown;
}

std::string shown_chat(std::string_view text, bool filter) {
    if (hateful(text)) {
        return small_talk_for(text);
    }
    return filter ? bleeped(text) : std::string{text};
}

std::string shown_name(std::string_view name, bool filter) {
    if (hateful(name)) {
        std::string hidden{name};
        for (std::size_t at = 0; at < hidden.size(); ++at) {
            hidden[at] = grawlix[at % grawlix.size()];
        }
        return hidden;
    }
    return filter ? bleeped(name) : std::string{name};
}

} // namespace hog::chat
