#include <catch2/catch_test_macros.hpp>

#include "model.h"

namespace {

ReviewStat make_stat(long long subject_id, const std::string& subject_type,
                      const std::string& data_updated_at) {
    ReviewStat stat;
    stat.subject_id = subject_id;
    stat.subject_type = subject_type;
    stat.data_updated_at = data_updated_at;
    return stat;
}

FailureEvent make_event(long long id, const std::string& occurred_at) {
    FailureEvent event;
    event.id = id;
    event.subject_id = 1;
    event.occurred_at = occurred_at;
    event.kind = FailureKind::Meaning;
    return event;
}

}  // namespace

// --- classify_failure: counter delta ---------------------------------

TEST_CASE("classify_failure detects a rise in meaning_incorrect as a meaning failure",
          "[classify_failure]") {
    ReviewStat prev = make_stat(1, "kanji", "2026-08-16T00:00:00.000000Z");
    prev.meaning_incorrect = 3;

    ReviewStat curr = make_stat(1, "kanji", "2026-08-16T01:00:00.000000Z");
    curr.meaning_incorrect = 4;

    const std::vector<FailureEvent> events = classify_failure(prev, curr);

    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == FailureKind::Meaning);
    CHECK(events[0].cold_start == false);
    CHECK(events[0].occurred_at == curr.data_updated_at);
    CHECK(events[0].subject_id == 1);
}

TEST_CASE("classify_failure ignores a flat meaning_incorrect counter", "[classify_failure]") {
    ReviewStat prev = make_stat(1, "kanji", "2026-08-16T00:00:00.000000Z");
    prev.meaning_incorrect = 3;

    ReviewStat curr = make_stat(1, "kanji", "2026-08-16T01:00:00.000000Z");
    curr.meaning_incorrect = 3;

    const std::vector<FailureEvent> events = classify_failure(prev, curr);

    REQUIRE(events.empty());
}

TEST_CASE("classify_failure reports Both when meaning and reading both rise", "[classify_failure]") {
    ReviewStat prev = make_stat(1, "kanji", "2026-08-16T00:00:00.000000Z");
    prev.meaning_incorrect = 3;
    prev.reading_incorrect = 1;

    ReviewStat curr = make_stat(1, "kanji", "2026-08-16T01:00:00.000000Z");
    curr.meaning_incorrect = 4;
    curr.reading_incorrect = 2;

    const std::vector<FailureEvent> events = classify_failure(prev, curr);

    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == FailureKind::Both);
}

TEST_CASE("classify_failure detects a reading-only rise", "[classify_failure]") {
    ReviewStat prev = make_stat(1, "vocabulary", "2026-08-16T00:00:00.000000Z");
    prev.reading_incorrect = 1;

    ReviewStat curr = make_stat(1, "vocabulary", "2026-08-16T01:00:00.000000Z");
    curr.reading_incorrect = 2;

    const std::vector<FailureEvent> events = classify_failure(prev, curr);

    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == FailureKind::Reading);
}

// --- classify_failure: cold start -------------------------------------

TEST_CASE("classify_failure fires a cold-start meaning failure on a zero streak",
          "[classify_failure]") {
    ReviewStat curr = make_stat(1, "kanji", "2026-08-16T00:00:00.000000Z");
    curr.meaning_current_streak = 0;
    curr.reading_current_streak = 5;

    const std::vector<FailureEvent> events = classify_failure(std::nullopt, curr);

    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == FailureKind::Meaning);
    CHECK(events[0].cold_start == true);
}

TEST_CASE("classify_failure produces no meaning failure when the cold-start streak is nonzero",
          "[classify_failure]") {
    ReviewStat curr = make_stat(1, "kanji", "2026-08-16T00:00:00.000000Z");
    curr.meaning_current_streak = 1;
    curr.reading_current_streak = 1;

    const std::vector<FailureEvent> events = classify_failure(std::nullopt, curr);

    REQUIRE(events.empty());
}

TEST_CASE("classify_failure cold-start reports Both when both streaks are zero on a kanji",
          "[classify_failure]") {
    ReviewStat curr = make_stat(1, "kanji", "2026-08-16T00:00:00.000000Z");
    curr.meaning_current_streak = 0;
    curr.reading_current_streak = 0;

    const std::vector<FailureEvent> events = classify_failure(std::nullopt, curr);

    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == FailureKind::Both);
    CHECK(events[0].cold_start == true);
}

TEST_CASE("classify_failure cold-start ignores a radical's zero reading streak",
          "[classify_failure]") {
    // Radicals have no reading quiz; reading_current_streak is always 0
    // and must not be treated as a reading failure.
    ReviewStat curr = make_stat(1, "radical", "2026-08-16T00:00:00.000000Z");
    curr.meaning_current_streak = 1;
    curr.reading_current_streak = 0;

    const std::vector<FailureEvent> events = classify_failure(std::nullopt, curr);

    REQUIRE(events.empty());
}

// --- group_into_sessions: 45-minute boundary ---------------------------

TEST_CASE("group_into_sessions splits on a >45 minute gap and keeps closer events together",
          "[group_into_sessions]") {
    // t, t+10min, t+60min (i.e. 50min after the *second* event) ->
    // {t, t+10min} then {t+60min}.
    //
    // Note: the brief's worked example literally reads "t, t+10min,
    // t+50min (relative to the first)", which — taken literally — gives
    // a 40-minute gap between the 2nd and 3rd events (50-10), not >45,
    // so it would NOT split under the "gap between consecutive events"
    // rule documented in the brief's own section 4c. That appears to be
    // an arithmetic slip in the brief: the intended split only works if
    // the "50min" is the gap size between the 2nd and 3rd events (as
    // used here), not an offset from the first event. Implemented per
    // section 4c's explicit wording: gap is measured between each event
    // and the one immediately before it (a sliding window), which is
    // also the conventional definition of a "session" (a contiguous run
    // without a long pause) — flagged in the task report.
    std::vector<FailureEvent> events = {
        make_event(1, "2026-08-16T00:00:00.000000Z"),
        make_event(2, "2026-08-16T00:10:00.000000Z"),
        make_event(3, "2026-08-16T01:00:00.000000Z"),
    };

    const std::vector<Session> sessions = group_into_sessions(events, 45);

    REQUIRE(sessions.size() == 2);
    CHECK(sessions[0].started_at == "2026-08-16T00:00:00.000000Z");
    CHECK(sessions[0].ended_at == "2026-08-16T00:10:00.000000Z");
    CHECK(sessions[1].started_at == "2026-08-16T01:00:00.000000Z");
    CHECK(sessions[1].ended_at == "2026-08-16T01:00:00.000000Z");

    CHECK(events[0].session_id == 0);
    CHECK(events[1].session_id == 0);
    CHECK(events[2].session_id == 1);
}

TEST_CASE("group_into_sessions does not split on a gap of exactly 45 minutes",
          "[group_into_sessions]") {
    // Documented choice: only a gap STRICTLY GREATER than the threshold
    // splits. A gap of exactly 45 minutes stays in the same session.
    std::vector<FailureEvent> events = {
        make_event(1, "2026-08-16T00:00:00.000000Z"),
        make_event(2, "2026-08-16T00:45:00.000000Z"),
    };

    const std::vector<Session> sessions = group_into_sessions(events, 45);

    REQUIRE(sessions.size() == 1);
    CHECK(events[0].session_id == 0);
    CHECK(events[1].session_id == 0);
}

TEST_CASE("group_into_sessions splits on a gap of 45 minutes and one second",
          "[group_into_sessions]") {
    std::vector<FailureEvent> events = {
        make_event(1, "2026-08-16T00:00:00.000000Z"),
        make_event(2, "2026-08-16T00:45:01.000000Z"),
    };

    const std::vector<Session> sessions = group_into_sessions(events, 45);

    REQUIRE(sessions.size() == 2);
    CHECK(events[0].session_id == 0);
    CHECK(events[1].session_id == 1);
}

TEST_CASE("group_into_sessions puts a single event in its own session", "[group_into_sessions]") {
    std::vector<FailureEvent> events = {make_event(1, "2026-08-16T00:00:00.000000Z")};

    const std::vector<Session> sessions = group_into_sessions(events, 45);

    REQUIRE(sessions.size() == 1);
    CHECK(events[0].session_id == 0);
}

TEST_CASE("group_into_sessions handles an empty list", "[group_into_sessions]") {
    std::vector<FailureEvent> events;

    const std::vector<Session> sessions = group_into_sessions(events, 45);

    CHECK(sessions.empty());
}

// --- parse_wk_timestamp -------------------------------------------------

TEST_CASE("parse_wk_timestamp parses a well-formed RFC3339 timestamp", "[parse_wk_timestamp]") {
    const auto parsed = parse_wk_timestamp("2026-08-16T03:14:07.123456Z");
    REQUIRE(parsed.has_value());
}

TEST_CASE("parse_wk_timestamp rejects a malformed timestamp", "[parse_wk_timestamp]") {
    CHECK_FALSE(parse_wk_timestamp("not-a-timestamp").has_value());
    CHECK_FALSE(parse_wk_timestamp("").has_value());
}
