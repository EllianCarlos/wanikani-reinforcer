#include <catch2/catch_test_macros.hpp>

#include "advice.h"
#include "model.h"

namespace {

// A small 太 (fat)/犬 (dog) fixture: both share the 大 (big) component
// radical, but 太 additionally has a "dot" component that 犬 lacks (and
// vice versa) -- modeled loosely here just to exercise the component
// diff rule, not to be botanically accurate about WaniKani's real
// component breakdown.
Subject make_tai() {
    Subject s;
    s.id = 1;
    s.type = "kanji";
    s.characters = "太";
    s.slug = "fat";
    s.meanings = {Meaning{"Fat", true, true}, Meaning{"Plump", false, true}};
    s.component_subject_ids = {10, 11};  // "big", "dot"
    s.meaning_mnemonic = "This kanji looks like a big person with a dot, because being fat makes you "
                          "look bigger than everyone else around you.";
    s.reading_mnemonic = "Reads as TAI, like a Thai person who loves to eat.";
    return s;
}

Subject make_inu() {
    Subject s;
    s.id = 2;
    s.type = "kanji";
    s.characters = "犬";
    s.slug = "dog";
    s.meanings = {Meaning{"Dog", true, true}};
    s.component_subject_ids = {10, 12};  // "big", "stray-mark"
    s.meaning_mnemonic = "A dog with a big stray mark on its back.";
    s.reading_mnemonic = "Reads as KEN.";
    return s;
}

std::vector<Subject> make_all_subjects() {
    Subject big;
    big.id = 10;
    big.slug = "big";
    Subject dot;
    dot.id = 11;
    dot.slug = "dot";
    Subject stray;
    stray.id = 12;
    stray.slug = "stray-mark";
    return {big, dot, stray};
}

}  // namespace

TEST_CASE("generate_advice always prints the failure-type line first", "[advice]") {
    const Subject tai = make_tai();
    const Subject inu = make_inu();
    const std::vector<Subject> all = make_all_subjects();

    const std::string meaning_advice =
        generate_advice(tai, inu, EdgeKind::Component, FailureKind::Meaning, std::nullopt, all);
    REQUIRE(meaning_advice.rfind("You fail the MEANING of 太, not the reading.", 0) == 0);

    const std::string reading_advice =
        generate_advice(tai, inu, EdgeKind::Component, FailureKind::Reading, std::nullopt, all);
    REQUIRE(reading_advice.rfind("You fail the READING of 太, not the meaning.", 0) == 0);

    const std::string both_advice =
        generate_advice(tai, inu, EdgeKind::Component, FailureKind::Both, std::nullopt, all);
    REQUIRE(both_advice.rfind("You fail BOTH the meaning and the reading of 太.", 0) == 0);
}

TEST_CASE("generate_advice's component diff rule names the components unique to each subject",
          "[advice]") {
    const Subject tai = make_tai();
    const Subject inu = make_inu();
    const std::vector<Subject> all = make_all_subjects();

    const std::string advice =
        generate_advice(tai, inu, EdgeKind::Component, FailureKind::Meaning, std::nullopt, all);

    CHECK(advice.find("太 has: [dot]") != std::string::npos);
    CHECK(advice.find("犬 has: [stray-mark]") != std::string::npos);
}

TEST_CASE("generate_advice skips the component diff rule when the component sets are identical",
          "[advice]") {
    Subject a = make_tai();
    Subject b = make_inu();
    b.component_subject_ids = a.component_subject_ids;  // now identical sets

    const std::vector<Subject> all = make_all_subjects();
    const std::string advice = generate_advice(a, b, EdgeKind::Component, FailureKind::Meaning, std::nullopt, all);

    CHECK(advice.find("has: [") == std::string::npos);
}

TEST_CASE("generate_advice's shared-reading rule only fires for a Reading edge", "[advice]") {
    Subject a = make_tai();
    a.readings = {Reading{"たい", true, true}};
    Subject b = make_inu();
    b.readings = {Reading{"たい", false, true}, Reading{"けん", true, true}};
    const std::vector<Subject> all = make_all_subjects();

    const std::string reading_edge_advice =
        generate_advice(a, b, EdgeKind::Reading, FailureKind::Reading, std::nullopt, all);
    CHECK(reading_edge_advice.find("Shared reading: たい") != std::string::npos);

    // Any other edge kind must not trigger rule 3, even with the same
    // shared-reading data present on both subjects.
    const std::string component_edge_advice =
        generate_advice(a, b, EdgeKind::Component, FailureKind::Reading, std::nullopt, all);
    CHECK(component_edge_advice.find("Shared reading:") == std::string::npos);
}

TEST_CASE("generate_advice's meaning-collision rule only fires for a Meaning edge", "[advice]") {
    Subject a = make_tai();  // meanings: Fat, Plump
    Subject b = make_inu();
    b.meanings = {Meaning{"Dog", true, true}, Meaning{"Fat", false, true}};  // shares "Fat"
    const std::vector<Subject> all = make_all_subjects();

    const std::string meaning_edge_advice =
        generate_advice(a, b, EdgeKind::Meaning, FailureKind::Meaning, std::nullopt, all);
    CHECK(meaning_edge_advice.find("Overlapping meaning:") != std::string::npos);

    const std::string component_edge_advice =
        generate_advice(a, b, EdgeKind::Component, FailureKind::Meaning, std::nullopt, all);
    CHECK(component_edge_advice.find("Overlapping meaning:") == std::string::npos);
}

TEST_CASE("generate_advice prints the meaning mnemonic and the user's own note when present",
          "[advice]") {
    const Subject tai = make_tai();
    const Subject inu = make_inu();
    const std::vector<Subject> all = make_all_subjects();

    wk_api::StudyMaterial material;
    material.subject_id = tai.id;
    material.meaning_note = "I always mix this up with dog.";

    const std::string advice =
        generate_advice(tai, inu, EdgeKind::Component, FailureKind::Meaning, material, all);

    CHECK(advice.find("Mnemonic:") != std::string::npos);
    CHECK(advice.find("Your note: I always mix this up with dog.") != std::string::npos);
}

TEST_CASE("generate_advice truncates a very long mnemonic instead of printing it in full", "[advice]") {
    Subject tai = make_tai();
    tai.meaning_mnemonic = std::string(1000, 'x') + ". trailing sentence.";
    const Subject inu = make_inu();
    const std::vector<Subject> all = make_all_subjects();

    const std::string advice =
        generate_advice(tai, inu, EdgeKind::Component, FailureKind::Meaning, std::nullopt, all);

    CHECK(advice.size() < tai.meaning_mnemonic.size());
    CHECK(advice.find("...") != std::string::npos);
}
