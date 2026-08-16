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
#include "store.h"
#include "wk_api.h"

// End-to-end coverage that the modules built across Tasks 1-5 actually
// compose: sync -> report -> drill against one seeded Store, with no
// wk_api::fetch_* or http:: call anywhere in this file (grep confirms).
// Fixture vectors stand in for what a real `wkr sync` would have fetched
// over HTTP; everything downstream of "subjects and stats already
// arrived" needs no network at all, which is exactly the seam this file
// exercises.

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

// Mirrors the local (non-HTTP) portion of main.cpp's run_sync: upserts
// subjects, inserts stat_snapshot rows (idempotent via Store's INSERT OR
// IGNORE on its (subject_id, data_updated_at) primary key), classifies +
// persists failure events for newly-inserted snapshots only, groups
// ungrouped failure events into sessions, and rebuilds the similarity
// graph (also idempotent -- replace_similarity_edges is a full replace,
// not an append). Deliberately never calls wk_api::fetch_* or http::get
// -- see the file header comment.
void fixture_sync(Store& store, const std::vector<Subject>& subjects, const std::vector<ReviewStat>& stats,
                   int session_gap_minutes = 45) {
    for (const auto& subject : subjects) {
        store.upsert_subject(subject);
    }

    for (const auto& stat : stats) {
        if (!store.insert_stat_snapshot(stat)) {
            continue;  // already-seen (subject_id, data_updated_at); the idempotency gate
        }
        const std::optional<ReviewStat> prev = store.previous_snapshot(stat.subject_id, stat.data_updated_at);
        for (const auto& event : classify_failure(prev, stat)) {
            store.insert_failure_event(event);
        }
    }

    std::vector<FailureEvent> ungrouped = store.failure_events_without_session();
    const std::vector<Session> sessions = group_into_sessions(ungrouped, session_gap_minutes);
    std::vector<long long> real_session_ids;
    real_session_ids.reserve(sessions.size());
    for (const auto& session : sessions) {
        real_session_ids.push_back(store.insert_session(session));
    }
    for (const auto& event : ungrouped) {
        store.assign_failure_event_session(event.id,
                                            real_session_ids.at(static_cast<size_t>(event.session_id)));
    }

    if (!subjects.empty()) {
        const std::vector<Subject> graph_subjects = store.all_subjects();
        const std::vector<SimilarityEdge> edges = build_similarity_graph(graph_subjects);
        store.replace_similarity_edges(edges);
    }
}

}  // namespace

TEST_CASE("sync -> report -> drill pipeline composes end-to-end against a seeded fixture DB, no network",
          "[integration]") {
    const std::string db_path = temp_db_path("pipeline");
    std::filesystem::remove(db_path);
    Store store(db_path);

    fixture_sync(store, fixture_subjects(), fixture_stats());

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
