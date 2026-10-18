#include <hog/chat/filter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

using hog::chat::bleeped;
using hog::chat::hateful;
using hog::chat::shown_chat;
using hog::chat::shown_name;
using hog::chat::small_talk_for;

namespace {

// Every harmless line a hateful one may read as instead.
constexpr std::array small_talk{
    "lovely weather we're having, isn't it?",   "my, the clouds look fantastic today.",
    "nothing beats a warm breeze off the sea.", "I do enjoy a light drizzle.",
    "I hear it might rain on Tuesday.",         "what a sunset that was last night.",
};

[[nodiscard]] bool is_small_talk(std::string_view line) {
    return std::ranges::find(small_talk, line) != small_talk.end();
}

} // namespace

TEST_CASE("cussing becomes comic-book grawlix, whatever the case", "[chat]") {
    CHECK(bleeped("Fuck off") == "@#$% off");
    CHECK(bleeped("FUCK") == "@#$%");
    CHECK(bleeped("that's bullshit") == "that's bull@#$%");
    CHECK(bleeped("you ass") == "you @#$");
    CHECK(bleeped("hello there") == "hello there");
}

TEST_CASE("a bleeped word's endings go with it", "[chat]") {
    CHECK(bleeped("fucking hell") == "@#$%&!@ hell");
    CHECK(bleeped("no shits given") == "no @#$%& given");
    CHECK(bleeped("wankers, pricks") == "@#$%&!@, @#$%&!");
}

TEST_CASE("stretched and dressed-up cussing is still seen", "[chat]") {
    CHECK(bleeped("fuuuuck") == "@#$%&!@");
    CHECK(bleeped("a$$") == "@#$");
    CHECK(bleeped("sh1t") == "@#$%");
    CHECK(bleeped("5h17 happens") == "@#$% happens");
}

TEST_CASE("cussing welded into a compound is seen whole", "[chat]") {
    CHECK(bleeped("Hello dickbutt!") == "Hello @#$%&!@#!");
    CHECK(bleeped("what a dumbass") == "what a @#$%&!@");
    CHECK(bleeped("asshats, dickheads") == "@#$%&!@, @#$%&!@#$");
    CHECK(bleeped("you jackass") == "you @#$%&!@");
    // Words that only hold the letters, with no compound part around them, still pass.
    CHECK(bleeped("Scunthorpe class assessment") == "Scunthorpe class assessment");
    CHECK(bleeped("the cockpit hatch") == "the cockpit hatch");
}

TEST_CASE("cussing split apart is seen when it stands alone", "[chat]") {
    CHECK(bleeped("f u c k") == "@#$%&!@");
    CHECK(bleeped("f.u.c.k") == "@#$%&!@");
    CHECK(bleeped("say f-u-c-k now") == "say @#$%&!@ now");
    // Letters that only spell it across words are left be.
    CHECK(bleeped("his hit song") == "his hit song");
    CHECK(bleeped("pass it on") == "pass it on");
}

TEST_CASE("invisible characters between letters hide nothing", "[chat]") {
    const std::string split = "f​uck you";
    const std::string shown = bleeped(split);
    CHECK(shown.ends_with(" you"));
    CHECK(shown.find("uck") == std::string::npos);
}

TEST_CASE("lookalike letters from other alphabets hide nothing", "[chat]") {
    CHECK(bleeped("fuсk") != "fuсk");         // Cyrillic с
    CHECK(bleeped("shіt") != "shіt");         // Cyrillic і
    CHECK(bleeped("fuçk") != "fuçk");         // ç
    CHECK(bleeped("ｆｕｃｋ") != "ｆｕｃｋ"); // full width
}

TEST_CASE("innocent words holding cussing are left be", "[chat]") {
    for (const std::string_view line :
         {"the class assignment", "Scunthorpe", "the cockpit canopy", "a prickly passage",
          "assessment", "cocktails at the bar", "a passing grade"}) {
        CHECK(bleeped(line) == line);
    }
}

TEST_CASE("bleeping keeps the line's length", "[chat]") {
    const std::string line = "what the fuck is this shit";
    CHECK(bleeped(line).size() == line.size());
}

TEST_CASE("hate speech is seen through its disguises", "[chat]") {
    CHECK(hateful("you're a nigger"));
    CHECK(hateful("N1GG3R"));
    CHECK(hateful("n i g g e r"));
    CHECK(hateful("fa66ot"));
    CHECK(hateful("ni99a"));
    CHECK(hateful("faggot"));
    CHECK(hateful("f4ggot"));
    CHECK(hateful("k1ke"));
    CHECK(hateful("tr4nny"));
    CHECK_FALSE(hateful("lovely weather"));
    CHECK_FALSE(hateful("what the fuck")); // cussing, not hate
}

TEST_CASE("innocent words holding slurs are left be", "[chat]") {
    for (const std::string_view line :
         {"gobbledygook", "a raccoon in the bins", "conspicuous", "a chink of light"}) {
        CHECK_FALSE(hateful(line));
    }
}

TEST_CASE("a hateful line reads as small talk, whatever the filter says", "[chat]") {
    const std::string shown = shown_chat("you f4ggots don't belong here", false);
    CHECK(is_small_talk(shown));
    CHECK(is_small_talk(shown_chat("n i g g e r s out", true)));
    // The same line reads the same everywhere.
    CHECK(shown_chat("you f4ggots don't belong here", false) == shown);
}

TEST_CASE("chat follows the filter where there's no hate", "[chat]") {
    CHECK(shown_chat("what the fuck", true) == "what the @#$%");
    CHECK(shown_chat("what the fuck", false) == "what the fuck");
    CHECK(shown_chat("hello there", true) == "hello there");
}

TEST_CASE("a hateful name is all grawlix, a cussing one follows the filter", "[chat]") {
    CHECK(shown_name("xXnig6aXx", false) != "xXnig6aXx");
    CHECK(shown_name("f4ggot99", true) == "@#$%&!@#");
    CHECK(shown_name("a$$hole", true) == "@#$%&!@");
    CHECK(shown_name("a$$hole", false) == "a$$hole");
    CHECK(shown_name("Ada", true) == "Ada");
}

TEST_CASE("small talk is picked steadily from the few lines there are", "[chat]") {
    const std::string first = small_talk_for("some hateful line");
    CHECK(is_small_talk(first));
    CHECK(small_talk_for("some hateful line") == first);
}
