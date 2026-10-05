#include <catch2/catch_test_macros.hpp>

#include "kana.h"

TEST_CASE("to_hiragana converts plain romaji", "[kana]") {
    CHECK(to_hiragana("inu") == "いぬ");
    CHECK(to_hiragana("nihon") == "にほん");
    CHECK(to_hiragana("sensei") == "せんせい");
    CHECK(to_hiragana("shashin") == "しゃしん");
    CHECK(to_hiragana("kyou") == "きょう");
    CHECK(to_hiragana("fuji") == "ふじ");
    CHECK(to_hiragana("tsuki") == "つき");
}

TEST_CASE("to_hiragana handles double consonants and the n rules", "[kana]") {
    CHECK(to_hiragana("kitte") == "きって");
    CHECK(to_hiragana("gakkou") == "がっこう");
    CHECK(to_hiragana("matcha") == "まっちゃ");
    CHECK(to_hiragana("konnichiha") == "こんにちは");
    CHECK(to_hiragana("kan") == "かん");
    CHECK(to_hiragana("nn") == "ん");
    CHECK(to_hiragana("ten'in") == "てんいん");
    CHECK(to_hiragana("kanya") == "かにゃ");
}

TEST_CASE("to_hiragana ignores case and surrounding spaces", "[kana]") {
    CHECK(to_hiragana("  Inu ") == "いぬ");
    CHECK(to_hiragana("NIHON") == "にほん");
}

TEST_CASE("to_hiragana passes hiragana through and folds katakana to hiragana", "[kana]") {
    CHECK(to_hiragana("いぬ") == "いぬ");
    CHECK(to_hiragana("イヌ") == "いぬ");
    CHECK(to_hiragana("コーヒー") == "こーひー");
}

TEST_CASE("to_hiragana leaves an incomplete trailing romaji letter as typed", "[kana]") {
    CHECK(to_hiragana("inuk") == "いぬk");
}
