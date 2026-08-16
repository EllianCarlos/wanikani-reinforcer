#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <filesystem>
#include <sstream>

#include "model.h"
#include "store.h"

namespace {

// Returns a fresh temp db path per test invocation so tests don't
// interfere with each other or leave state behind.
std::string temp_db_path(const std::string& label) {
    static int counter = 0;
    std::ostringstream name;
    name << "wkr_test_" << label << "_" << (counter++) << ".db";
    return (std::filesystem::temp_directory_path() / name.str()).string();
}

// Runs `sql` (a single "SELECT COUNT(...) ..." or similar scalar-int
// query) directly against the db file, bypassing Store, so tests can
// verify row counts/values without Store needing read accessors it
// doesn't otherwise require for this task.
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

Subject make_subject(long long id, const std::string& data_updated_at) {
    Subject subject;
    subject.id = id;
    subject.type = "kanji";
    subject.characters = "水";
    subject.slug = "water";
    subject.level = 1;
    subject.meanings = {Meaning{"Water", true, true}};
    subject.data_updated_at = data_updated_at;
    return subject;
}

ReviewStat make_stat(long long subject_id, const std::string& data_updated_at, int meaning_incorrect) {
    ReviewStat stat;
    stat.subject_id = subject_id;
    stat.subject_type = "kanji";
    stat.data_updated_at = data_updated_at;
    stat.meaning_incorrect = meaning_incorrect;
    return stat;
}

}  // namespace

TEST_CASE("upsert_subject is idempotent", "[store]") {
    const std::string db_path = temp_db_path("idempotent");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject subject = make_subject(42, "2020-01-01T00:00:00.000000Z");

    store.upsert_subject(subject);
    store.upsert_subject(subject);
    store.upsert_subject(subject);

    REQUIRE(store.subject_count() == 1);
}

TEST_CASE("upsert_subject replaces the row on re-sync with updated data", "[store]") {
    const std::string db_path = temp_db_path("replace");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject subject = make_subject(7, "2020-01-01T00:00:00.000000Z");
    store.upsert_subject(subject);

    subject.characters = "火";
    subject.data_updated_at = "2021-06-15T00:00:00.000000Z";
    store.upsert_subject(subject);

    REQUIRE(store.subject_count() == 1);
}

TEST_CASE("sync_meta get/set round-trips and is absent by default", "[store]") {
    const std::string db_path = temp_db_path("meta");
    std::filesystem::remove(db_path);
    Store store(db_path);

    REQUIRE_FALSE(store.get_meta("subjects_cursor").has_value());

    store.set_meta("subjects_cursor", "2020-01-01T00:00:00.000000Z");
    auto value = store.get_meta("subjects_cursor");
    REQUIRE(value.has_value());
    REQUIRE(*value == "2020-01-01T00:00:00.000000Z");

    store.set_meta("subjects_cursor", "2021-01-01T00:00:00.000000Z");
    value = store.get_meta("subjects_cursor");
    REQUIRE(*value == "2021-01-01T00:00:00.000000Z");
}

TEST_CASE("insert_stat_snapshot is idempotent on (subject_id, data_updated_at)", "[store]") {
    const std::string db_path = temp_db_path("stat_idempotent");
    std::filesystem::remove(db_path);
    Store store(db_path);

    ReviewStat stat = make_stat(1, "2026-08-16T00:00:00.000000Z", 3);

    REQUIRE(store.insert_stat_snapshot(stat) == true);
    REQUIRE(store.insert_stat_snapshot(stat) == false);
    REQUIRE(store.insert_stat_snapshot(stat) == false);
}

TEST_CASE("insert_stat_snapshot returns true for a new data_updated_at on the same subject",
          "[store]") {
    const std::string db_path = temp_db_path("stat_new_snapshot");
    std::filesystem::remove(db_path);
    Store store(db_path);

    REQUIRE(store.insert_stat_snapshot(make_stat(1, "2026-08-16T00:00:00.000000Z", 3)) == true);
    REQUIRE(store.insert_stat_snapshot(make_stat(1, "2026-08-16T01:00:00.000000Z", 4)) == true);
}

TEST_CASE("previous_snapshot returns nullopt when no earlier row exists", "[store]") {
    const std::string db_path = temp_db_path("prev_missing");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.insert_stat_snapshot(make_stat(1, "2026-08-16T00:00:00.000000Z", 3));

    REQUIRE_FALSE(store.previous_snapshot(1, "2026-08-16T00:00:00.000000Z").has_value());
}

TEST_CASE("previous_snapshot returns the closest earlier row for that subject", "[store]") {
    const std::string db_path = temp_db_path("prev_found");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.insert_stat_snapshot(make_stat(1, "2026-08-16T00:00:00.000000Z", 1));
    store.insert_stat_snapshot(make_stat(1, "2026-08-16T01:00:00.000000Z", 2));
    store.insert_stat_snapshot(make_stat(1, "2026-08-16T02:00:00.000000Z", 3));
    // A different subject's rows must never be returned.
    store.insert_stat_snapshot(make_stat(2, "2026-08-16T01:30:00.000000Z", 9));

    auto prev = store.previous_snapshot(1, "2026-08-16T02:00:00.000000Z");
    REQUIRE(prev.has_value());
    CHECK(prev->data_updated_at == "2026-08-16T01:00:00.000000Z");
    CHECK(prev->meaning_incorrect == 2);
}

TEST_CASE("failure_events_without_session returns unassigned events ordered by occurred_at",
          "[store]") {
    const std::string db_path = temp_db_path("failure_events");
    std::filesystem::remove(db_path);
    Store store(db_path);

    FailureEvent e1;
    e1.subject_id = 1;
    e1.occurred_at = "2026-08-16T01:00:00.000000Z";
    e1.kind = FailureKind::Meaning;
    e1.cold_start = false;

    FailureEvent e2;
    e2.subject_id = 2;
    e2.occurred_at = "2026-08-16T00:00:00.000000Z";
    e2.kind = FailureKind::Both;
    e2.cold_start = true;

    store.insert_failure_event(e1);
    store.insert_failure_event(e2);

    const std::vector<FailureEvent> events = store.failure_events_without_session();
    REQUIRE(events.size() == 2);
    // Ordered ascending by occurred_at, so e2 (earlier) comes first.
    CHECK(events[0].subject_id == 2);
    CHECK(events[0].kind == FailureKind::Both);
    CHECK(events[0].cold_start == true);
    CHECK(events[1].subject_id == 1);
    CHECK(events[1].kind == FailureKind::Meaning);
    CHECK(events[0].id != events[1].id);
}

TEST_CASE("insert_session, assign_failure_event_session remove events from the unassigned list",
          "[store]") {
    const std::string db_path = temp_db_path("session_assign");
    std::filesystem::remove(db_path);
    Store store(db_path);

    FailureEvent e1;
    e1.subject_id = 1;
    e1.occurred_at = "2026-08-16T00:00:00.000000Z";
    e1.kind = FailureKind::Meaning;
    store.insert_failure_event(e1);

    std::vector<FailureEvent> unassigned = store.failure_events_without_session();
    REQUIRE(unassigned.size() == 1);

    Session session{0, "2026-08-16T00:00:00.000000Z", "2026-08-16T00:00:00.000000Z"};
    const long long session_id = store.insert_session(session);
    store.assign_failure_event_session(unassigned[0].id, session_id);

    REQUIRE(store.failure_events_without_session().empty());
}

TEST_CASE("upsert_assignment is idempotent and replaces on re-sync", "[store]") {
    const std::string db_path = temp_db_path("upsert_assignment");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Assignment assignment;
    assignment.id = 1;
    assignment.subject_id = 5;
    assignment.subject_type = "kanji";
    assignment.srs_stage = 1;
    assignment.data_updated_at = "2026-08-16T00:00:00.000000Z";

    store.upsert_assignment(assignment);
    assignment.srs_stage = 2;
    assignment.data_updated_at = "2026-08-16T01:00:00.000000Z";
    store.upsert_assignment(assignment);

    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM assignment;") == 1);
    CHECK(query_scalar_int(db_path, "SELECT srs_stage FROM assignment WHERE id = 1;") == 2);
}

TEST_CASE("upsert_study_material can be called twice for the same subject without error",
          "[store]") {
    const std::string db_path = temp_db_path("upsert_study_material");
    std::filesystem::remove(db_path);
    Store store(db_path);

    wk_api::StudyMaterial material;
    material.subject_id = 5;
    material.subject_type = "kanji";
    material.meaning_note = "note one";
    material.meaning_synonyms = {"a", "b"};
    material.data_updated_at = "2026-08-16T00:00:00.000000Z";

    store.upsert_study_material(material);
    material.meaning_note = "note two";
    material.data_updated_at = "2026-08-16T01:00:00.000000Z";
    store.upsert_study_material(material);

    // Keyed by subject_id (study_material carries no id from the API per
    // this task's model), so the second call must replace, not duplicate.
    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM study_material;") == 1);
    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM study_material WHERE meaning_note = 'note two';") == 1);
}

TEST_CASE("all_subjects round-trips every field through the JSON columns", "[store]") {
    const std::string db_path = temp_db_path("all_subjects");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject subject = make_subject(5, "2026-08-16T00:00:00.000000Z");
    subject.type = "kanji";
    subject.characters = "行";
    subject.meanings = {Meaning{"Go", true, true}, Meaning{"Travel", false, true}};
    subject.auxiliary_meanings = {AuxiliaryMeaning{"Journey", "whitelist"}};
    subject.readings = {Reading{"こう", true, true}, Reading{"ぎょう", false, true}};
    subject.component_subject_ids = {1, 2, 3};
    subject.visually_similar_subject_ids = {6, 7};
    subject.meaning_mnemonic = "mm";
    subject.reading_mnemonic = "rm";
    store.upsert_subject(subject);

    const std::vector<Subject> all = store.all_subjects();

    REQUIRE(all.size() == 1);
    const Subject& s = all[0];
    CHECK(s.id == 5);
    CHECK(s.type == "kanji");
    CHECK(s.characters == "行");
    REQUIRE(s.meanings.size() == 2);
    CHECK(s.meanings[0].meaning == "Go");
    CHECK(s.meanings[0].primary == true);
    CHECK(s.meanings[1].meaning == "Travel");
    REQUIRE(s.auxiliary_meanings.size() == 1);
    CHECK(s.auxiliary_meanings[0].meaning == "Journey");
    REQUIRE(s.readings.size() == 2);
    CHECK(s.readings[0].reading == "こう");
    CHECK(s.readings[0].primary == true);
    CHECK(s.readings[1].primary == false);
    CHECK(s.component_subject_ids == std::vector<long long>{1, 2, 3});
    CHECK(s.visually_similar_subject_ids == std::vector<long long>{6, 7});
    CHECK(s.meaning_mnemonic == "mm");
    CHECK(s.reading_mnemonic == "rm");
    CHECK(s.data_updated_at == "2026-08-16T00:00:00.000000Z");
}

TEST_CASE("replace_similarity_edges round-trips through edges_for and fully replaces on rebuild",
          "[store]") {
    const std::string db_path = temp_db_path("similarity_edges");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.replace_similarity_edges({
        SimilarityEdge{1, 2, EdgeKind::WkVisual, 1.0},
        SimilarityEdge{1, 3, EdgeKind::Component, 0.5},
        SimilarityEdge{2, 3, EdgeKind::Meaning, 0.7},
    });

    const std::vector<SimilarityEdge> for_1 = store.edges_for(1);
    REQUIRE(for_1.size() == 2);  // (1,2,wk_visual) and (1,3,component)

    const std::vector<SimilarityEdge> for_3 = store.edges_for(3);
    REQUIRE(for_3.size() == 2);  // (1,3,component) and (2,3,meaning)
    bool found_component = false;
    bool found_meaning = false;
    for (const auto& e : for_3) {
        if (e.kind == EdgeKind::Component) {
            CHECK(e.a_id == 1);
            CHECK(e.b_id == 3);
            CHECK(e.weight == 0.5);
            found_component = true;
        } else if (e.kind == EdgeKind::Meaning) {
            CHECK(e.a_id == 2);
            CHECK(e.b_id == 3);
            CHECK(e.weight == 0.7);
            found_meaning = true;
        }
    }
    CHECK(found_component);
    CHECK(found_meaning);

    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM similarity_edge;") == 3);

    // A second rebuild must fully replace the previous set, not append.
    store.replace_similarity_edges({SimilarityEdge{9, 10, EdgeKind::CharShape, 0.4}});

    CHECK(query_scalar_int(db_path, "SELECT COUNT(*) FROM similarity_edge;") == 1);
    CHECK(store.edges_for(1).empty());
    const std::vector<SimilarityEdge> for_9 = store.edges_for(9);
    REQUIRE(for_9.size() == 1);
    CHECK(for_9[0].kind == EdgeKind::CharShape);
}

TEST_CASE("all_similarity_edges returns every edge regardless of endpoint", "[store]") {
    const std::string db_path = temp_db_path("all_similarity_edges");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.replace_similarity_edges({
        SimilarityEdge{1, 2, EdgeKind::WkVisual, 1.0},
        SimilarityEdge{3, 4, EdgeKind::Meaning, 0.7},
    });

    const std::vector<SimilarityEdge> all = store.all_similarity_edges();
    REQUIRE(all.size() == 2);
}

TEST_CASE("latest_stat_per_subject returns only the newest snapshot for each subject", "[store]") {
    const std::string db_path = temp_db_path("latest_stat");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.insert_stat_snapshot(make_stat(1, "2026-08-16T00:00:00.000000Z", 1));
    store.insert_stat_snapshot(make_stat(1, "2026-08-16T02:00:00.000000Z", 5));
    store.insert_stat_snapshot(make_stat(2, "2026-08-16T01:00:00.000000Z", 9));

    const std::vector<ReviewStat> latest = store.latest_stat_per_subject();
    REQUIRE(latest.size() == 2);

    bool found_1 = false;
    bool found_2 = false;
    for (const auto& stat : latest) {
        if (stat.subject_id == 1) {
            CHECK(stat.data_updated_at == "2026-08-16T02:00:00.000000Z");
            CHECK(stat.meaning_incorrect == 5);
            found_1 = true;
        } else if (stat.subject_id == 2) {
            CHECK(stat.meaning_incorrect == 9);
            found_2 = true;
        }
    }
    CHECK(found_1);
    CHECK(found_2);
}

TEST_CASE("all_sessions and all_failure_events return every row including assigned ones", "[store]") {
    const std::string db_path = temp_db_path("all_sessions_events");
    std::filesystem::remove(db_path);
    Store store(db_path);

    FailureEvent e1;
    e1.subject_id = 1;
    e1.occurred_at = "2026-08-16T00:00:00.000000Z";
    e1.kind = FailureKind::Meaning;
    store.insert_failure_event(e1);

    std::vector<FailureEvent> unassigned = store.failure_events_without_session();
    REQUIRE(unassigned.size() == 1);

    Session session{0, "2026-08-16T00:00:00.000000Z", "2026-08-16T00:00:00.000000Z"};
    const long long session_id = store.insert_session(session);
    store.assign_failure_event_session(unassigned[0].id, session_id);

    // Once assigned, failure_events_without_session no longer sees it...
    CHECK(store.failure_events_without_session().empty());

    // ...but all_failure_events and all_sessions still do, with the
    // session_id populated.
    const std::vector<Session> sessions = store.all_sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].id == session_id);

    const std::vector<FailureEvent> all_events = store.all_failure_events();
    REQUIRE(all_events.size() == 1);
    CHECK(all_events[0].session_id == session_id);
}

TEST_CASE("all_assignments and all_study_materials return every row", "[store]") {
    const std::string db_path = temp_db_path("all_assignments_materials");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Assignment assignment;
    assignment.id = 1;
    assignment.subject_id = 5;
    assignment.subject_type = "kanji";
    assignment.srs_stage = 3;
    assignment.data_updated_at = "2026-08-16T00:00:00.000000Z";
    store.upsert_assignment(assignment);

    wk_api::StudyMaterial material;
    material.subject_id = 5;
    material.subject_type = "kanji";
    material.meaning_note = "note";
    material.data_updated_at = "2026-08-16T00:00:00.000000Z";
    store.upsert_study_material(material);

    const std::vector<Assignment> assignments = store.all_assignments();
    REQUIRE(assignments.size() == 1);
    CHECK(assignments[0].subject_id == 5);
    CHECK(assignments[0].srs_stage == 3);

    const std::vector<wk_api::StudyMaterial> materials = store.all_study_materials();
    REQUIRE(materials.size() == 1);
    CHECK(materials[0].subject_id == 5);
    CHECK(materials[0].meaning_note == "note");
}
