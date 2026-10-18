#pragma once

#include <string>
#include <string_view>

// What chat may look like once shown. Both ends use it: the server to report hate speech and
// relay small talk in its place, the client to do the same for servers that don't, and to turn
// cussing to grawlix for players who'd rather not read it.
namespace hog::chat {

// Whether hate speech shows through `text`, past the usual disguises: lookalike letters and
// digits, stretched words, letters split apart, and invisible characters between them.
[[nodiscard]] bool hateful(std::string_view text);

// The harmless small talk a hateful line is shown as instead: hate speech gets no platform.
// Picked steadily by the line itself, so the same line reads the same everywhere.
[[nodiscard]] std::string small_talk_for(std::string_view text);

// `text` with cussing turned to comic-book grawlix, seeing through the same disguises. Hate
// speech isn't its business: ask hateful() first.
[[nodiscard]] std::string bleeped(std::string_view text);

// A line of chat as a client shows it: hate speech always as small talk; cussing as grawlix when
// `filter`, as it was said otherwise.
[[nodiscard]] std::string shown_chat(std::string_view text, bool filter);

// A name can't make small talk, so a hateful one is all grawlix; cussing in one follows `filter`
// as chat does.
[[nodiscard]] std::string shown_name(std::string_view name, bool filter);

} // namespace hog::chat
