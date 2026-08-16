#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "confusion.h"
#include "model.h"

using Catch::Approx;

namespace {

ReviewStat make_stat(long long subject_id, int meaning_correct, int meaning_incorrect,
                      int meaning_streak, int reading_correct, int reading_incorrect, int reading_streak) {
    ReviewStat stat;
    stat.subject_id = subject_id;
    stat.meaning_correct = meaning_correct;
    stat.meaning_incorrect = meaning_incorrect;
    stat.meaning_current_streak = meaning_streak;
    stat.reading_correct = reading_correct;
    stat.reading_incorrect = reading_incorrect;
    stat.reading_current_streak = reading_streak;
    return stat;
}

FailureEvent make_event(long long subject_id, FailureKind kind, long long session_id,
                         const std::string& occurred_at) {
    FailureEvent event;
    event.subject_id = subject_id;
    event.kind = kind;
    event.session_id = session_id;
    event.occurred_at = occurred_at;
    return event;
}

}  // namespace

TEST_CASE("compute_leech_scores picks the larger of meaning/reading and sets is_meaning", "[confusion]") {
    // Subject 1: meaning_incorrect=10, meaning_streak=0 -> 10/(1)^1.5 = 10.
    //            reading_incorrect=0 -> reading score 0. Larger is meaning.
    const ReviewStat s1 = make_stat(1, /*mc=*/5, /*mi=*/10, /*ms=*/0, /*rc=*/5, /*ri=*/0, /*rs=*/5);
    // Subject 2: reading_incorrect=8, reading_streak=0 -> 8/(1)^1.5 = 8.
    //            meaning_incorrect=0 -> meaning score 0. Larger is reading.
    const ReviewStat s2 = make_stat(2, /*mc=*/5, /*mi=*/0, /*ms=*/5, /*rc=*/5, /*ri=*/8, /*rs=*/0);
    // Subject 3: only 4 total reviews -- below kMinReviewsForLeech, excluded.
    const ReviewStat s3 = make_stat(3, /*mc=*/1, /*mi=*/1, /*ms=*/0, /*rc=*/1, /*ri=*/1, /*rs=*/0);

    const std::vector<LeechEntry> leeches = compute_leech_scores({s1, s2, s3});

    REQUIRE(leeches.size() == 2);
    // Sorted descending by leech_score: subject 1 (10.0) before subject 2 (8.0).
    CHECK(leeches[0].subject_id == 1);
    CHECK(leeches[0].is_meaning == true);
    CHECK(leeches[0].leech_score == Approx(10.0));
    CHECK(leeches[1].subject_id == 2);
    CHECK(leeches[1].is_meaning == false);
    CHECK(leeches[1].leech_score == Approx(8.0));
}

TEST_CASE("compute_leech_scores excludes never-failed subjects even when they clear the review minimum",
          "[confusion][regression]") {
    // 10 reviews, all correct on both axes -> both axis scores are 0.0.
    // Without the positive-score gate this subject would still be
    // reported as a leech (print_report prints the top N
    // unconditionally), claiming the user fails its meaning -- which
    // never happened.
    const ReviewStat never_failed = make_stat(7, /*mc=*/5, /*mi=*/0, /*ms=*/5, /*rc=*/5, /*ri=*/0, /*rs=*/5);
    // Regression guard on the other side: one real incorrect answer must
    // still produce a leech entry.
    const ReviewStat failed_once = make_stat(8, /*mc=*/5, /*mi=*/1, /*ms=*/2, /*rc=*/5, /*ri=*/0, /*rs=*/5);

    const std::vector<LeechEntry> leeches = compute_leech_scores({never_failed, failed_once});

    REQUIRE(leeches.size() == 1);
    CHECK(leeches[0].subject_id == 8);
    CHECK(leeches[0].leech_score > 0.0);
    CHECK(leeches[0].is_meaning == true);
}

TEST_CASE("compute_leech_scores excludes subjects below the minimum review count", "[confusion]") {
    const ReviewStat below = make_stat(9, 1, 1, 0, 1, 1, 0);  // total = 4
    const std::vector<LeechEntry> leeches = compute_leech_scores({below});
    CHECK(leeches.empty());
}

TEST_CASE("compute_confusion_pairs applies the match kind_bonus and decay for a co-failure pair",
          "[confusion]") {
    const std::string now = "2026-08-16T00:00:00Z";
    const Session session{10, "2026-08-15T00:00:00Z", "2026-08-15T00:00:00Z"};  // 1 day before now
    const std::vector<FailureEvent> events = {
        make_event(1, FailureKind::Meaning, 10, "2026-08-15T00:00:00Z"),
        make_event(2, FailureKind::Meaning, 10, "2026-08-15T00:00:00Z"),
    };
    const std::vector<SimilarityEdge> edges = {SimilarityEdge{1, 2, EdgeKind::Meaning, 0.7}};

    const std::vector<ConfusionPair> pairs =
        compute_confusion_pairs({session}, events, edges, /*leeches=*/{}, now);

    REQUIRE(pairs.size() == 1);
    const ConfusionPair& pair = pairs[0];
    CHECK(pair.a_id == 1);
    CHECK(pair.b_id == 2);
    CHECK(pair.likely == false);
    CHECK(pair.dominant_kind == EdgeKind::Meaning);
    // 0.7 (weight) * 1.5 (match bonus: meaning failure vs. a Meaning edge) * exp(-1/30) (decay)
    const double expected = 0.7 * 1.5 * std::exp(-1.0 / 30.0);
    CHECK(pair.score == Approx(expected));
}

TEST_CASE("compute_confusion_pairs applies the mismatch kind_bonus when failure kind doesn't match "
          "the edge kind",
          "[confusion]") {
    const std::string now = "2026-08-16T00:00:00Z";
    const Session session{20, "2026-08-14T00:00:00Z", "2026-08-14T00:00:00Z"};  // 2 days before now
    const std::vector<FailureEvent> events = {
        make_event(3, FailureKind::Reading, 20, "2026-08-14T00:00:00Z"),
        make_event(4, FailureKind::Reading, 20, "2026-08-14T00:00:00Z"),
    };
    // A Meaning edge only takes the match bonus for a meaning failure; here
    // both subjects failed on Reading, so this is a mismatch.
    const std::vector<SimilarityEdge> edges = {SimilarityEdge{3, 4, EdgeKind::Meaning, 0.5}};

    const std::vector<ConfusionPair> pairs =
        compute_confusion_pairs({session}, events, edges, /*leeches=*/{}, now);

    REQUIRE(pairs.size() == 1);
    const double expected = 0.5 * 0.6 * std::exp(-2.0 / 30.0);
    CHECK(pairs[0].score == Approx(expected));
}

TEST_CASE("compute_confusion_pairs emits a discounted likely pair for a leech with no co-failure",
          "[confusion]") {
    const std::string now = "2026-08-16T00:00:00Z";
    // No sessions/events at all -- subject 5 never co-failed with anyone.
    const std::vector<SimilarityEdge> edges = {
        SimilarityEdge{5, 6, EdgeKind::Component, 0.9},
        SimilarityEdge{5, 7, EdgeKind::Meaning, 0.3},  // weaker edge; 6 must win
    };
    LeechEntry leech;
    leech.subject_id = 5;
    leech.leech_score = 42.0;
    leech.is_meaning = true;

    const std::vector<ConfusionPair> pairs =
        compute_confusion_pairs(/*sessions=*/{}, /*events=*/{}, edges, {leech}, now);

    REQUIRE(pairs.size() == 1);
    const ConfusionPair& pair = pairs[0];
    CHECK(pair.a_id == 5);
    CHECK(pair.b_id == 6);
    CHECK(pair.likely == true);
    CHECK(pair.dominant_kind == EdgeKind::Component);
    CHECK(pair.score == Approx(0.9 * 0.5));
}

TEST_CASE("compute_confusion_pairs skips a leech with no edges at all", "[confusion]") {
    LeechEntry leech;
    leech.subject_id = 99;
    leech.leech_score = 1.0;
    leech.is_meaning = false;

    const std::vector<ConfusionPair> pairs =
        compute_confusion_pairs(/*sessions=*/{}, /*events=*/{}, /*edges=*/{}, {leech}, "2026-08-16T00:00:00Z");

    CHECK(pairs.empty());
}

TEST_CASE("compute_confusion_pairs does not emit a likely pair for a leech that co-failed", "[confusion]") {
    const std::string now = "2026-08-16T00:00:00Z";
    const Session session{1, "2026-08-16T00:00:00Z", "2026-08-16T00:00:00Z"};
    const std::vector<FailureEvent> events = {
        make_event(1, FailureKind::Meaning, 1, now),
        make_event(2, FailureKind::Meaning, 1, now),
    };
    const std::vector<SimilarityEdge> edges = {SimilarityEdge{1, 2, EdgeKind::Meaning, 0.7}};
    LeechEntry leech;
    leech.subject_id = 1;
    leech.leech_score = 5.0;
    leech.is_meaning = true;

    const std::vector<ConfusionPair> pairs = compute_confusion_pairs({session}, events, edges, {leech}, now);

    // Only the co-failure pair, no separate "likely" entry for subject 1.
    REQUIRE(pairs.size() == 1);
    CHECK(pairs[0].likely == false);
}
