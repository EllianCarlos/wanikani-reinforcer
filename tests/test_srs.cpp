#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

#include "model.h"
#include "srs.h"

namespace {

ConfusionPair make_pair(long long a, long long b, double score) {
    return ConfusionPair{a, b, score, EdgeKind::WkVisual, false};
}

PairSrsState make_state(long long a, long long b, int stage, const std::string& due_at) {
    return PairSrsState{a, b, stage, due_at, "2026-08-16T00:00:00Z"};
}

const std::string kNow = "2026-08-16T00:00:00Z";

}  // namespace

TEST_CASE("format_wk_timestamp round-trips through parse_wk_timestamp", "[srs]") {
    const auto parsed = parse_wk_timestamp("2026-08-16T03:14:07Z");
    REQUIRE(parsed.has_value());
    CHECK(format_wk_timestamp(*parsed) == "2026-08-16T03:14:07Z");
}

TEST_CASE("a never-drilled pair answered correctly moves to stage 1 due in 4 hours", "[srs]") {
    const PairSrsState next = advance_pair_srs(std::nullopt, 7, 3, true, kNow);

    CHECK(next.a_id == 3);
    CHECK(next.b_id == 7);
    CHECK(next.stage == 1);
    CHECK(next.due_at == "2026-08-16T04:00:00Z");
    CHECK(next.last_reviewed_at == kNow);
}

TEST_CASE("a never-drilled pair answered incorrectly is still scheduled at stage 1", "[srs]") {
    const PairSrsState next = advance_pair_srs(std::nullopt, 3, 7, false, kNow);

    CHECK(next.stage == 1);
    CHECK(next.due_at == "2026-08-16T04:00:00Z");
}

TEST_CASE("an incorrect answer drops two stages but never below stage 1", "[srs]") {
    const PairSrsState from_five =
        advance_pair_srs(make_state(3, 7, 5, "2026-08-10T00:00:00Z"), 3, 7, false, kNow);
    CHECK(from_five.stage == 3);
    CHECK(from_five.due_at == "2026-08-17T00:00:00Z");

    const PairSrsState from_two =
        advance_pair_srs(make_state(3, 7, 2, "2026-08-10T00:00:00Z"), 3, 7, false, kNow);
    CHECK(from_two.stage == 1);
}

TEST_CASE("a correct answer on stage 8 burns the pair", "[srs]") {
    const PairSrsState next =
        advance_pair_srs(make_state(3, 7, 8, "2026-08-10T00:00:00Z"), 3, 7, true, kNow);

    CHECK(next.stage == kSrsBurnedStage);
    CHECK(next.due_at.empty());
}

TEST_CASE("select_due_pairs keeps unseen and overdue pairs in score order", "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(1, 2, 9.0), make_pair(3, 4, 8.0),
                                              make_pair(5, 6, 7.0), make_pair(7, 8, 6.0)};
    const std::vector<PairSrsState> states = {
        make_state(1, 2, 2, "2026-08-16T08:00:00Z"),
        make_state(5, 6, 3, "2026-08-15T00:00:00Z"),
        make_state(7, 8, kSrsBurnedStage, ""),
    };

    const std::vector<ConfusionPair> due = select_due_pairs(pairs, states, kNow);

    REQUIRE(due.size() == 2);
    CHECK(due[0].a_id == 3);
    CHECK(due[1].a_id == 5);
}

TEST_CASE("select_due_pairs matches state regardless of endpoint order", "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(9, 2, 5.0)};
    const std::vector<PairSrsState> states = {make_state(2, 9, 2, "2026-08-16T08:00:00Z")};

    CHECK(select_due_pairs(pairs, states, kNow).empty());
}

TEST_CASE("next_due_at returns the earliest due time among non-burned pairs", "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(1, 2, 9.0), make_pair(3, 4, 8.0),
                                              make_pair(5, 6, 7.0)};
    const std::vector<PairSrsState> states = {
        make_state(1, 2, 2, "2026-08-16T08:00:00Z"),
        make_state(3, 4, 3, "2026-08-16T03:20:00Z"),
        make_state(5, 6, kSrsBurnedStage, ""),
    };

    const std::optional<std::string> next = next_due_at(pairs, states);

    REQUIRE(next.has_value());
    CHECK(*next == "2026-08-16T03:20:00Z");
}

TEST_CASE("next_due_at is nullopt when every pair is burned", "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(1, 2, 9.0)};
    const std::vector<PairSrsState> states = {make_state(1, 2, kSrsBurnedStage, "")};

    CHECK_FALSE(next_due_at(pairs, states).has_value());
}

TEST_CASE("format_wait renders the two largest nonzero units", "[srs]") {
    CHECK(format_wait("2026-08-16T04:00:00Z", kNow) == "4h");
    CHECK(format_wait("2026-08-16T03:20:00Z", kNow) == "3h 20m");
    CHECK(format_wait("2026-08-18T00:00:00Z", kNow) == "2d");
    CHECK(format_wait("2026-08-17T04:00:00Z", kNow) == "1d 4h");
    CHECK(format_wait("2026-08-16T00:00:30Z", kNow) == "<1m");
}

TEST_CASE("describe_pair_srs covers new, scheduled, due and burned pairs", "[srs]") {
    CHECK(describe_pair_srs(std::nullopt, kNow) == "new");
    CHECK(describe_pair_srs(make_state(1, 2, 3, "2026-08-18T00:00:00Z"), kNow) ==
          "stage 3, due in 2d");
    CHECK(describe_pair_srs(make_state(1, 2, 3, "2026-08-15T00:00:00Z"), kNow) ==
          "stage 3, due now");
    CHECK(describe_pair_srs(make_state(1, 2, kSrsBurnedStage, ""), kNow) == "burned");
}

TEST_CASE("nothing_due_message names the wait until the next review", "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(1, 2, 9.0), make_pair(3, 4, 8.0)};
    const std::vector<PairSrsState> states = {
        make_state(1, 2, 2, "2026-08-16T08:00:00Z"),
        make_state(3, 4, 1, "2026-08-16T03:20:00Z"),
    };

    CHECK(nothing_due_message(pairs, states, kNow) == "Nothing due. Next review in 3h 20m.");
}

TEST_CASE("nothing_due_message reports a fully burned queue", "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(1, 2, 9.0)};
    const std::vector<PairSrsState> states = {make_state(1, 2, kSrsBurnedStage, "")};

    CHECK(nothing_due_message(pairs, states, kNow) ==
          "Nothing due -- every confusion pair is burned.");
}

TEST_CASE("a drilled pair is skipped on the next selection and returns once its due time passes",
          "[srs]") {
    const std::vector<ConfusionPair> pairs = {make_pair(1, 2, 9.0)};
    const PairSrsState drilled = advance_pair_srs(std::nullopt, 1, 2, true, kNow);

    CHECK(select_due_pairs(pairs, {drilled}, kNow).empty());
    CHECK(select_due_pairs(pairs, {drilled}, "2026-08-16T04:00:00Z").size() == 1);
}
