#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <filesystem>
#include <optional>
#include <sstream>
#include <vector>

#include "confusion.h"
#include "drill.h"
#include "model.h"
#include "report.h"
#include "similarity.h"
#include "stats.h"
#include "store.h"
#include "sync.h"
#include "wk_api.h"

// End-to-end coverage that the modules built across Tasks 1-5 actually
// compose: sync -> report -> drill against one seeded Store, with no
// wk_api::fetch_* or http:: call anywhere in this file (grep confirms).
//
// The sync step drives the REAL pipeline -- ::run_sync() from src/sync.cpp,
// the same function `wkr sync` calls -- with fixture fetchers injected in
// place of the HTTP ones (SyncFetchers; see sync.h). This file used to
// hand-reimplement run_sync's ordering, which meant it verified a copy of
// the pipeline rather than the pipeline itself; a bug in the real
// ordering could not be caught here. Fixture vectors stand in for what a
// real `wkr sync` would have fetched over HTTP; everything downstream of
// "subjects and stats already arrived" needs no network at all, which is
// exactly the seam this file exercises.

namespace {

std::string temp_db_path(const std::string& label) {
    static int counter = 0;
    std::ostringstream name;
    name << "wkr_integration_test_" << label << "_" << (counter++) << ".db";
    return (std::filesystem::temp_directory_path() / name.str()).string();
}

// Runs a scalar-int SELECT directly against the db file, bypassing Store,
// mirroring test_store.cpp's helper of the same name/behavior.
long long query_scalar_int(const std::string& db_path, const std::string& sql) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
    long long value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

// Fixture "WaniKani server response": two kanji (太/fat, 犬/dog) sharing
// one component (a "big" radical), plus the radical subject itself so
// component-based similarity has something to link. Loosely modeled, not
// botanically accurate about WaniKani's real component breakdown -- same
// spirit as test_drill.cpp's fixture.
std::vector<Subject> fixture_subjects() {
    Subject big;
    big.id = 10;
    big.type = "radical";
    big.characters = "大";
    big.slug = "big";
    big.level = 1;
    big.meanings = {Meaning{"Big", true, true}};
    big.data_updated_at = "2026-08-01T00:00:00Z";

    Subject fat;
    fat.id = 1;
    fat.type = "kanji";
    fat.characters = "太";
    fat.slug = "fat";
    fat.level = 1;
    fat.meanings = {Meaning{"Fat", true, true}};
    fat.readings = {Reading{"たい", true, true}};
    fat.component_subject_ids = {10};
    fat.meaning_mnemonic = "A fat person stands like the big radical, just with an extra dot.";
    fat.data_updated_at = "2026-08-01T00:00:00Z";

    Subject dog;
    dog.id = 2;
    dog.type = "kanji";
    dog.characters = "犬";
    dog.slug = "dog";
    dog.level = 1;
    dog.meanings = {Meaning{"Dog", true, true}};
    dog.readings = {Reading{"けん", true, true}};
    dog.component_subject_ids = {10};
    dog.meaning_mnemonic = "A dog with a big stray mark on its back.";
    dog.data_updated_at = "2026-08-01T00:00:00Z";

    return {big, fat, dog};
}

// Fixture review_statistics: both 太 and 犬 have a broken (zero) meaning
// streak on their very first snapshot ever -- a cold-start failure for
// both (see classify_failure), at the same instant, so they land in the
// same session and become a co-failure pair. Total review count on each
// (5) meets kMinReviewsForLeech exactly, so both qualify as leeches.
std::vector<ReviewStat> fixture_stats() {
    ReviewStat fat_stat;
    fat_stat.subject_id = 1;
    fat_stat.subject_type = "kanji";
    fat_stat.meaning_correct = 2;
    fat_stat.meaning_incorrect = 3;
    fat_stat.meaning_current_streak = 0;
    fat_stat.reading_correct = 0;
    fat_stat.reading_incorrect = 0;
    fat_stat.reading_current_streak = 5;
    fat_stat.percentage_correct = 40;
    fat_stat.data_updated_at = "2026-08-15T10:00:00.000000Z";

    ReviewStat dog_stat;
    dog_stat.subject_id = 2;
    dog_stat.subject_type = "kanji";
    dog_stat.meaning_correct = 2;
    dog_stat.meaning_incorrect = 3;
    dog_stat.meaning_current_streak = 0;
    dog_stat.reading_correct = 0;
    dog_stat.reading_incorrect = 0;
    dog_stat.reading_current_streak = 5;
    dog_stat.percentage_correct = 40;
    dog_stat.data_updated_at = "2026-08-15T10:00:00.000000Z";

    return {fat_stat, dog_stat};
}

// The fixture stand-in for the four HTTP fetches run_sync performs. Each
// closure ignores the `updated_after` cursor it is handed and returns the
// same fixture vector, which is exactly the "server has the same data as
// last time" scenario the idempotency test below needs. Nothing here
// calls wk_api::fetch_* or http::get -- see the file header comment.
SyncFetchers fixture_fetchers(const std::vector<Subject>& subjects, const std::vector<ReviewStat>& stats) {
    SyncFetchers fetchers;
    fetchers.subjects = [subjects](const std::string&) { return subjects; };
    fetchers.review_statistics = [stats](const std::string&) { return stats; };
    fetchers.assignments = [](const std::string&) { return std::vector<Assignment>{}; };
    fetchers.study_materials = [](const std::string&) { return std::vector<wk_api::StudyMaterial>{}; };
    return fetchers;
}

// Runs the real sync pipeline against `store` with fixture data: upsert
// subjects, snapshot/delta/failure detection, cursor advances, session
// grouping, similarity-graph rebuild -- all of it ::run_sync's own code,
// not a copy of it.
SyncSummary fixture_sync(Store& store, const std::vector<Subject>& subjects,
                          const std::vector<ReviewStat>& stats, int session_gap_minutes = 45) {
    return run_sync(store, fixture_fetchers(subjects, stats), session_gap_minutes);
}

}  // namespace

TEST_CASE("sync -> report -> drill pipeline composes end-to-end against a seeded fixture DB, no network",
          "[integration]") {
    const std::string db_path = temp_db_path("pipeline");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const SyncSummary summary = fixture_sync(store, fixture_subjects(), fixture_stats());
    CHECK(summary.subjects == 3);
    CHECK(summary.stats == 2);
    CHECK(summary.failures == 2);            // one cold-start meaning failure each for 太 and 犬
    CHECK(summary.similarity_edge_count > 0);  // subjects changed, so the graph was rebuilt

    // The pipeline's cursor bookkeeping ran too (main.cpp does not do this
    // itself -- it lives in run_sync, so this test covers it).
    CHECK(store.get_meta("subjects_cursor").value_or("") == "2026-08-01T00:00:00Z");
    CHECK(store.get_meta("stats_cursor").value_or("") == "2026-08-15T10:00:00.000000Z");

    // --- report step: reload everything from Store, exactly like
    // main.cpp's run_report does ---
    const std::vector<Subject> all_subjects = store.all_subjects();
    REQUIRE(all_subjects.size() == 3);

    const std::vector<ReviewStat> latest_stats = store.latest_stat_per_subject();
    const std::vector<Session> sessions = store.all_sessions();
    const std::vector<FailureEvent> events = store.all_failure_events();
    const std::vector<SimilarityEdge> edges = store.all_similarity_edges();
    REQUIRE_FALSE(edges.empty());  // the Component edge between 太 and 犬

    const std::vector<LeechEntry> leeches = compute_leech_scores(latest_stats);
    REQUIRE(leeches.size() == 2);  // both 太 and 犬 crossed kMinReviewsForLeech

    const std::vector<ConfusionPair> pairs =
        compute_confusion_pairs(sessions, events, edges, leeches, "2026-08-16T00:00:00Z");
    REQUIRE_FALSE(pairs.empty());
    CHECK(pairs[0].likely == false);  // a real observed co-failure, not just a "likely" guess

    std::ostringstream report_out;
    print_report(pairs, leeches, all_subjects, /*all_assignments=*/{}, /*all_study_materials=*/{},
                 latest_stats, report_out, /*use_color=*/false);
    const std::string report_text = report_out.str();
    CHECK_FALSE(report_text.empty());
    CHECK(report_text.find("LEECH") != std::string::npos);
    CHECK(report_text.find("Focus:") != std::string::npos);

    // --- drill step: same pairs/subjects, over a scripted stdin. Only one
    // ConfusionPair exists in this fixture (太/犬's single co-failure
    // pair), so run_drill's own min(question_count, pairs.size()) caps
    // the drill at one question even though question_count asks for more
    // -- exactly the "runnable drill" the brief calls for, not a fixed
    // question count.
    std::istringstream drill_in("1\n1\n");
    std::ostringstream drill_out;
    run_drill(pairs, all_subjects, /*study_materials=*/{}, store, /*question_count=*/2, drill_in, drill_out,
              /*use_color=*/false);
    const std::string drill_text = drill_out.str();
    CHECK_FALSE(drill_text.empty());
    CHECK(drill_text.find("correct") != std::string::npos);  // the final score line ("N/M correct")

    // The drill step must have written its own local record -- never sent
    // anywhere -- and left the confusion-pair data untouched.
    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM drill_result;") == static_cast<long long>(pairs.size()));
}

TEST_CASE("stats.cpp reads back drill history and session summary after a real sync + drill run",
          "[integration]") {
    // Same fixture/sync/drill setup as the pipeline test above; this test
    // is a smoke test that stats.cpp's compute functions work against
    // real Store-backed data, not a re-test of their own logic (already
    // covered by tests/test_stats.cpp).
    const std::string db_path = temp_db_path("stats_after_drill");
    std::filesystem::remove(db_path);
    Store store(db_path);

    fixture_sync(store, fixture_subjects(), fixture_stats());

    const std::vector<Subject> all_subjects = store.all_subjects();
    const std::vector<ReviewStat> latest_stats = store.latest_stat_per_subject();
    const std::vector<Session> sessions = store.all_sessions();
    const std::vector<FailureEvent> events = store.all_failure_events();
    const std::vector<SimilarityEdge> edges = store.all_similarity_edges();
    const std::vector<LeechEntry> leeches = compute_leech_scores(latest_stats);
    const std::vector<ConfusionPair> pairs =
        compute_confusion_pairs(sessions, events, edges, leeches, "2026-08-16T00:00:00Z");
    REQUIRE_FALSE(pairs.empty());

    // The fixture's single co-failure pair is (fat=1, dog=2), a_id < b_id
    // (see confusion.cpp). Question 1 is forced-choice, meaning axis,
    // unrotated (i=0): options are [fat, dog, big], so "1" both selects
    // and correctly answers "Fat".
    std::istringstream drill_in("1\n");
    std::ostringstream drill_out;
    run_drill(pairs, all_subjects, /*study_materials=*/{}, store, /*question_count=*/1, drill_in, drill_out,
              /*use_color=*/false);

    const std::vector<DrillResult> drill_results = store.all_drill_results();
    REQUIRE(drill_results.size() == 1);
    CHECK(drill_results[0].subject_id == 1);
    CHECK(drill_results[0].distractor_id == 2);
    CHECK(drill_results[0].correct == true);

    const DrillStats drill_stats = compute_drill_stats(drill_results);
    CHECK(drill_stats.total >= 0);
    CHECK(drill_stats.correct >= 0);
    CHECK(drill_stats.correct <= drill_stats.total);

    const SessionStats session_stats =
        compute_session_stats(store.all_sessions(), store.all_failure_events(), "2026-08-16T00:00:00Z");
    CHECK(session_stats.session_count >= 0);
    CHECK(session_stats.avg_failures_per_session >= 0.0);
    CHECK(session_stats.sessions_last_7_days >= 0);
}

TEST_CASE("re-running the sync path twice against identical fixture input produces no duplicate rows",
          "[integration]") {
    const std::string db_path = temp_db_path("idempotent_double_sync");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = fixture_subjects();
    const std::vector<ReviewStat> stats = fixture_stats();

    fixture_sync(store, subjects, stats);
    const long long subject_count_1 = query_scalar_int(db_path, "SELECT COUNT(*) FROM subject;");
    const long long stat_count_1 = query_scalar_int(db_path, "SELECT COUNT(*) FROM stat_snapshot;");
    const long long edge_count_1 = query_scalar_int(db_path, "SELECT COUNT(*) FROM similarity_edge;");
    const long long failure_event_count_1 = query_scalar_int(db_path, "SELECT COUNT(*) FROM failure_event;");
    REQUIRE(subject_count_1 == 3);
    REQUIRE(stat_count_1 == 2);
    REQUIRE(edge_count_1 > 0);
    REQUIRE(failure_event_count_1 == 2);  // one cold-start failure each for 太 and 犬

    // Re-run the exact same "sync" against the exact same fixture input --
    // as if `wkr sync` were invoked a second time with nothing new on the
    // (simulated) server.
    fixture_sync(store, subjects, stats);

    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM subject;") == subject_count_1);
    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM stat_snapshot;") == stat_count_1);
    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM similarity_edge;") == edge_count_1);
    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM failure_event;") == failure_event_count_1);
}

TEST_CASE("sync rebuilds the similarity graph when the edge table is empty, even with no new subjects",
          "[integration][regression]") {
    // Regression guard for the mid-sync-failure trap: run_sync advances
    // subjects_cursor immediately after the subjects fetch, but the graph
    // rebuild happens at the very end. If a later step throws (rate-limit
    // exhaustion, transport error), the next sync fetches zero subjects
    // because the cursor is already current -- and a rebuild gated solely
    // on "subjects changed this run" would then never fire again, leaving
    // the similarity graph permanently empty with no error and no
    // recovery path.
    const std::string db_path = temp_db_path("empty_graph_recovery");
    std::filesystem::remove(db_path);
    Store store(db_path);

    // The state such an interrupted sync leaves behind: subjects present,
    // cursor advanced, similarity_edge empty.
    for (const auto& subject : fixture_subjects()) {
        store.upsert_subject(subject);
    }
    store.set_meta("subjects_cursor", "2026-08-01T00:00:00Z");
    REQUIRE(store.all_similarity_edges().empty());

    // The next `wkr sync`: the (simulated) server reports nothing new.
    const SyncSummary summary = fixture_sync(store, /*subjects=*/{}, /*stats=*/{});

    REQUIRE(summary.subjects == 0);
    CHECK(summary.similarity_edge_count > 0);
    CHECK_FALSE(store.all_similarity_edges().empty());
}

TEST_CASE("sync still skips the graph rebuild when nothing changed and a graph already exists",
          "[integration]") {
    // The other half of the gate: the empty-table clause above must not
    // turn the rebuild into an unconditional every-sync cost.
    const std::string db_path = temp_db_path("skip_rebuild");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const SyncSummary first = fixture_sync(store, fixture_subjects(), fixture_stats());
    REQUIRE(first.similarity_edge_count > 0);

    const SyncSummary second = fixture_sync(store, /*subjects=*/{}, /*stats=*/{});
    CHECK(second.similarity_edge_count == -1);  // -1 == "skipped, nothing changed"
    CHECK_FALSE(store.all_similarity_edges().empty());
}
