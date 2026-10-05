#include <catch2/catch_test_macros.hpp>

#include "width.h"

TEST_CASE("display_width returns 2 for a CJK codepoint", "[width]") {
    CHECK(display_width("太") == 2);
}

TEST_CASE("display_width returns 1 for an ASCII character", "[width]") {
    CHECK(display_width("a") == 1);
}

TEST_CASE("display_width sums mixed CJK and ASCII correctly", "[width]") {
    CHECK(display_width("太a") == 3);
}

TEST_CASE("display_width returns 0 for an empty string", "[width]") { CHECK(display_width("") == 0); }

TEST_CASE("display_width handles multiple CJK characters", "[width]") {
    CHECK(display_width("大学") == 4);
}

TEST_CASE("display_width does not crash on a truncated multi-byte sequence", "[width]") {
    // 0xE5 is a valid 3-byte UTF-8 lead byte, but the string ends before
    // the two continuation bytes it requires.
    const std::string truncated = "\xE5";
    CHECK(display_width(truncated) == 1);
}

TEST_CASE("display_width treats an invalid leading byte as width 1 and keeps going", "[width]") {
    // 0xFF is never a valid UTF-8 leading byte.
    const std::string malformed = "\xFF" "a";
    CHECK(display_width(malformed) == 2);
}

TEST_CASE("display_width counts a fullwidth ASCII-range codepoint as wide", "[width]") {
    // U+FF21 FULLWIDTH LATIN CAPITAL LETTER A, inside the FF00..FF60 range.
    CHECK(display_width("\xEF\xBC\xA1") == 2);
}
