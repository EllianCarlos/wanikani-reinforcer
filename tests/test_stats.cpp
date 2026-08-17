#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <sstream>

#include "stats.h"

TEST_CASE("compute_drill_stats totals correct/total and current streak from the tail", "[stats]") {
    const std::vector<DrillResult> results = {
        DrillResult{1, 2, true, "2026-08-10T00:00:00Z"},
        DrillResult{2, 1, false, "2026-08-11T00:00:00Z"},
        DrillResult{1, 2, true, "2026-08-12T00:00:00Z"},
        DrillResult{2, 1, true, "2026-08-13T00:00:00Z"},
    };

    const DrillStats stats = compute_drill_stats(results);
    CHECK(stats.total == 4);
    CHECK(stats.correct == 3);
    // Streak counts backward from the most recent (2026-08-13) result:
    // 08-13 correct, 08-12 correct, 08-11 wrong -- stops there, streak 2.
    CHECK(stats.current_streak == 2);
}

TEST_CASE("compute_drill_stats reports zero streak when the most recent answer was wrong", "[stats]") {
    const std::vector<DrillResult> results = {
        DrillResult{1, 2, true, "2026-08-10T00:00:00Z"},
        DrillResult{1, 2, false, "2026-08-11T00:00:00Z"},
    };

    CHECK(compute_drill_stats(results).current_streak == 0);
}

TEST_CASE("compute_drill_stats on an empty history returns all zeros", "[stats]") {
    const DrillStats stats = compute_drill_stats({});
    CHECK(stats.total == 0);
    CHECK(stats.correct == 0);
    CHECK(stats.current_streak == 0);
}

TEST_CASE("compute_missed_pairs ranks by miss count, descending, capped at limit", "[stats]") {
    const std::vector<DrillResult> results = {
        DrillResult{1, 2, false, "t1"}, DrillResult{1, 2, false, "t2"}, DrillResult{3, 4, false, "t3"},
        DrillResult{1, 2, true, "t4"},  // correct: not a miss
        DrillResult{5, 6, false, "t5"},
    };

    const std::vector<MissedPairCount> top2 = compute_missed_pairs(results, 2);
    REQUIRE(top2.size() == 2);
    CHECK(top2[0].subject_id == 1);
    CHECK(top2[0].distractor_id == 2);
    CHECK(top2[0].miss_count == 2);
    // (3,4) and (5,6) are tied at 1 miss each; the plan's stable_sort
    // keeps (3,4) first since it appeared earlier in `results`.
    CHECK(top2[1].subject_id == 3);
    CHECK(top2[1].miss_count == 1);
}

TEST_CASE("compute_missed_pairs with no misses at all returns empty", "[stats]") {
    const std::vector<DrillResult> results = {DrillResult{1, 2, true, "t1"}};
    CHECK(compute_missed_pairs(results, 5).empty());
}

TEST_CASE("compute_session_stats averages failures per session and counts recent ones", "[stats]") {
    const std::vector<Session> sessions = {
        Session{1, "2026-08-01T00:00:00Z", "2026-08-01T00:10:00Z"},
        Session{2, "2026-08-15T00:00:00Z", "2026-08-15T00:10:00Z"},
    };
    const std::vector<FailureEvent> events = {
        FailureEvent{1, 100, "2026-08-01T00:00:00Z", FailureKind::Meaning, 1, false},
        FailureEvent{2, 101, "2026-08-01T00:05:00Z", FailureKind::Meaning, 1, false},
        FailureEvent{3, 102, "2026-08-15T00:00:00Z", FailureKind::Meaning, 2, false},
    };

    // "now" is 2026-08-16: one day after session 2 (within 7 days) and 15
    // days after session 1 (outside 7 days).
    const SessionStats stats = compute_session_stats(sessions, events, "2026-08-16T00:00:00Z");
    CHECK(stats.session_count == 2);
    CHECK(std::abs(stats.avg_failures_per_session - 1.5) < 0.001);
    CHECK(stats.sessions_last_7_days == 1);
}

TEST_CASE("compute_session_stats on no sessions returns all zeros", "[stats]") {
    const SessionStats stats = compute_session_stats({}, {}, "2026-08-16T00:00:00Z");
    CHECK(stats.session_count == 0);
    CHECK(stats.avg_failures_per_session == 0.0);
    CHECK(stats.sessions_last_7_days == 0);
}

TEST_CASE("compute_leech_trend groups by subject and reports first/last percentage and delta", "[stats]") {
    ReviewStat s1;
    s1.subject_id = 1;
    s1.data_updated_at = "2026-08-01T00:00:00Z";
    s1.percentage_correct = 40;

    ReviewStat s2;
    s2.subject_id = 1;
    s2.data_updated_at = "2026-08-10T00:00:00Z";
    s2.percentage_correct = 70;

    ReviewStat s3;
    s3.subject_id = 2;
    s3.data_updated_at = "2026-08-01T00:00:00Z";
    s3.percentage_correct = 90;

    ReviewStat s4;
    s4.subject_id = 2;
    s4.data_updated_at = "2026-08-10T00:00:00Z";
    s4.percentage_correct = 85;

    const std::vector<LeechTrendEntry> trend = compute_leech_trend({s1, s2, s3, s4});
    REQUIRE(trend.size() == 2);
    // Sorted by |delta| descending: subject 1 improved by 30, subject 2
    // worsened by 5 -- 30 > 5, so subject 1 comes first.
    CHECK(trend[0].subject_id == 1);
    CHECK(trend[0].snapshot_count == 2);
    CHECK(trend[0].first_percentage == 40);
    CHECK(trend[0].last_percentage == 70);
    CHECK(trend[0].delta == 30);
    CHECK(trend[1].subject_id == 2);
    CHECK(trend[1].delta == -5);
}

TEST_CASE("compute_leech_trend on a single snapshot reports zero delta", "[stats]") {
    ReviewStat s1;
    s1.subject_id = 1;
    s1.data_updated_at = "2026-08-01T00:00:00Z";
    s1.percentage_correct = 50;

    const std::vector<LeechTrendEntry> trend = compute_leech_trend({s1});
    REQUIRE(trend.size() == 1);
    CHECK(trend[0].snapshot_count == 1);
    CHECK(trend[0].delta == 0);
}

TEST_CASE("print_stats renders the no-history fallback line in every section", "[stats]") {
    std::ostringstream out;
    print_stats(DrillStats{}, {}, SessionStats{}, {}, {}, out);
    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("No drills recorded yet") != std::string::npos);
    CHECK(output.find("No sessions recorded yet") != std::string::npos);
    CHECK(output.find("Not enough sync history yet") != std::string::npos);
}

TEST_CASE("print_stats renders populated drill stats, missed pairs, sessions, and a shown leech trend",
          "[stats]") {
    Subject fat;
    fat.id = 1;
    fat.characters = "太";
    fat.slug = "fat";

    Subject dog;
    dog.id = 2;
    dog.characters = "犬";
    dog.slug = "dog";

    const DrillStats drill{10, 7, 3};
    const std::vector<MissedPairCount> missed = {MissedPairCount{1, 2, 4}};
    const SessionStats sessions{5, 1.5, 2};
    const std::vector<LeechTrendEntry> trend = {LeechTrendEntry{1, 2, 40, 70, 30}};

    std::ostringstream out;
    print_stats(drill, missed, sessions, trend, {fat, dog}, out);
    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("7/10 correct (70.0%), current streak 3") != std::string::npos);
    CHECK(output.find("太 / 犬  missed 4x") != std::string::npos);
    CHECK(output.find("5 sessions") != std::string::npos);
    CHECK(output.find("太  40% -> 70%  (+30)") != std::string::npos);
}
