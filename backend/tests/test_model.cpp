#include <catch2/catch_test_macros.hpp>

#include "model.h"

TEST_CASE("primary_meaning picks the marked-primary entry", "[model]") {
    Subject subject;
    subject.meanings = {
        Meaning{"Wrong", false, true},
        Meaning{"Right", true, true},
        Meaning{"Also Wrong", false, false},
    };

    const Meaning* result = primary_meaning(subject);

    REQUIRE(result != nullptr);
    REQUIRE(result->meaning == "Right");
}

TEST_CASE("primary_meaning falls back to the first meaning when none is primary", "[model]") {
    Subject subject;
    subject.meanings = {
        Meaning{"First", false, true},
        Meaning{"Second", false, true},
    };

    const Meaning* result = primary_meaning(subject);

    REQUIRE(result != nullptr);
    REQUIRE(result->meaning == "First");
}

TEST_CASE("primary_meaning returns nullptr for a subject with no meanings", "[model]") {
    Subject subject;

    REQUIRE(primary_meaning(subject) == nullptr);
}
