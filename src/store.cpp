#include "store.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace {

using nlohmann::json;

// FailureKind <-> the failure_event.kind TEXT column, which the schema
// constrains to 'meaning' | 'reading' | 'both'.
std::string failure_kind_to_text(FailureKind kind) {
    switch (kind) {
        case FailureKind::Meaning:
            return "meaning";
        case FailureKind::Reading:
            return "reading";
        case FailureKind::Both:
            return "both";
    }
    return "meaning";  // unreachable; keeps -Wall happy about return paths
}

FailureKind failure_kind_from_text(const std::string& text) {
    if (text == "reading") {
        return FailureKind::Reading;
    }
    if (text == "both") {
        return FailureKind::Both;
    }
    return FailureKind::Meaning;
}

// Current UTC time formatted like WaniKani's own timestamps
// ("2026-08-16T03:14:07Z"), used to stamp stat_snapshot.fetched_at —
// there's no API-provided value for "when did wkr observe this row".
std::string now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buf.data());
}

// The plan's schema is one block, not phased: create every table now so
// later tasks only add queries against tables that already exist.
constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS subject (
  id INTEGER PRIMARY KEY, type TEXT, characters TEXT, slug TEXT, level INTEGER,
  primary_meaning TEXT, meanings_json TEXT, aux_meanings_json TEXT,
  readings_json TEXT, component_ids_json TEXT, visually_similar_ids_json TEXT,
  meaning_mnemonic TEXT, reading_mnemonic TEXT, data_updated_at TEXT);

CREATE TABLE IF NOT EXISTS assignment (
  id INTEGER PRIMARY KEY, subject_id INTEGER, subject_type TEXT,
  srs_stage INTEGER, available_at TEXT, passed_at TEXT, data_updated_at TEXT);

CREATE TABLE IF NOT EXISTS study_material (
  id INTEGER PRIMARY KEY, subject_id INTEGER, subject_type TEXT,
  meaning_note TEXT, reading_note TEXT, meaning_synonyms_json TEXT,
  data_updated_at TEXT);

CREATE TABLE IF NOT EXISTS stat_snapshot (
  subject_id INTEGER, data_updated_at TEXT, fetched_at TEXT,
  meaning_correct INTEGER, meaning_incorrect INTEGER,
  reading_correct INTEGER, reading_incorrect INTEGER,
  meaning_current_streak INTEGER, reading_current_streak INTEGER,
  percentage_correct INTEGER,
  PRIMARY KEY (subject_id, data_updated_at));

CREATE TABLE IF NOT EXISTS failure_event (
  id INTEGER PRIMARY KEY, subject_id INTEGER, occurred_at TEXT,
  kind TEXT CHECK (kind IN ('meaning','reading','both')),
  session_id INTEGER, cold_start INTEGER DEFAULT 0);

CREATE TABLE IF NOT EXISTS session (
  id INTEGER PRIMARY KEY, started_at TEXT, ended_at TEXT);

CREATE TABLE IF NOT EXISTS similarity_edge (
  a_id INTEGER, b_id INTEGER, kind TEXT, weight REAL,
  PRIMARY KEY (a_id, b_id, kind));

CREATE TABLE IF NOT EXISTS drill_result (
  id INTEGER PRIMARY KEY, subject_id INTEGER, distractor_id INTEGER,
  correct INTEGER, answered_at TEXT);

CREATE TABLE IF NOT EXISTS sync_meta (key TEXT PRIMARY KEY, value TEXT);
)SQL";

void exec(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::string message = err_msg != nullptr ? err_msg : "unknown sqlite error";
        sqlite3_free(err_msg);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

json meanings_to_json(const std::vector<Meaning>& meanings) {
    json arr = json::array();
    for (const auto& m : meanings) {
        arr.push_back({{"meaning", m.meaning},
                        {"primary", m.primary},
                        {"accepted_answer", m.accepted_answer}});
    }
    return arr;
}

json aux_meanings_to_json(const std::vector<AuxiliaryMeaning>& aux) {
    json arr = json::array();
    for (const auto& m : aux) {
        arr.push_back({{"meaning", m.meaning}, {"type", m.type}});
    }
    return arr;
}

json readings_to_json(const std::vector<Reading>& readings) {
    json arr = json::array();
    for (const auto& r : readings) {
        arr.push_back({{"reading", r.reading},
                        {"primary", r.primary},
                        {"accepted_answer", r.accepted_answer}});
    }
    return arr;
}

// The reverse of *_to_json above, used by Store::all_subjects to
// reconstruct Subject from the JSON columns. Each treats an empty
// column as "no rows" rather than a parse error, since a freshly-created
// column could in principle be empty text.

std::vector<Meaning> meanings_from_json(const std::string& text) {
    std::vector<Meaning> result;
    if (text.empty()) {
        return result;
    }
    for (const auto& item : json::parse(text)) {
        Meaning m;
        m.meaning = item.value("meaning", "");
        m.primary = item.value("primary", false);
        m.accepted_answer = item.value("accepted_answer", false);
        result.push_back(std::move(m));
    }
    return result;
}

std::vector<AuxiliaryMeaning> aux_meanings_from_json(const std::string& text) {
    std::vector<AuxiliaryMeaning> result;
    if (text.empty()) {
        return result;
    }
    for (const auto& item : json::parse(text)) {
        AuxiliaryMeaning m;
        m.meaning = item.value("meaning", "");
        m.type = item.value("type", "");
        result.push_back(std::move(m));
    }
    return result;
}

std::vector<Reading> readings_from_json(const std::string& text) {
    std::vector<Reading> result;
    if (text.empty()) {
        return result;
    }
    for (const auto& item : json::parse(text)) {
        Reading r;
        r.reading = item.value("reading", "");
        r.primary = item.value("primary", false);
        r.accepted_answer = item.value("accepted_answer", false);
        result.push_back(std::move(r));
    }
    return result;
}

std::vector<long long> ids_from_json(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    return json::parse(text).get<std::vector<long long>>();
}

// EdgeKind <-> the similarity_edge.kind TEXT column.
std::string edge_kind_to_text(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::WkVisual:
            return "wk_visual";
        case EdgeKind::Component:
            return "component";
        case EdgeKind::Reading:
            return "reading";
        case EdgeKind::Meaning:
            return "meaning";
        case EdgeKind::CharShape:
            return "char_shape";
    }
    return "wk_visual";  // unreachable; keeps -Wall happy about return paths
}

EdgeKind edge_kind_from_text(const std::string& text) {
    if (text == "component") {
        return EdgeKind::Component;
    }
    if (text == "reading") {
        return EdgeKind::Reading;
    }
    if (text == "meaning") {
        return EdgeKind::Meaning;
    }
    if (text == "char_shape") {
        return EdgeKind::CharShape;
    }
    return EdgeKind::WkVisual;
}

}  // namespace

Store::Store(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error";
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error("failed to open database at " + db_path + ": " + message);
    }

    exec(db_, "PRAGMA foreign_keys = ON;");
    exec(db_, "PRAGMA journal_mode = WAL;");
    exec(db_, kSchemaSql);
}

Store::~Store() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

void Store::upsert_subject(const Subject& subject) {
    static const char* sql =
        "INSERT OR REPLACE INTO subject "
        "(id, type, characters, slug, level, primary_meaning, meanings_json, "
        "aux_meanings_json, readings_json, component_ids_json, "
        "visually_similar_ids_json, meaning_mnemonic, reading_mnemonic, "
        "data_updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare upsert_subject failed: ") + sqlite3_errmsg(db_));
    }

    const Meaning* primary = primary_meaning(subject);
    const std::string primary_meaning_text = primary != nullptr ? primary->meaning : "";
    const std::string meanings_text = meanings_to_json(subject.meanings).dump();
    const std::string aux_text = aux_meanings_to_json(subject.auxiliary_meanings).dump();
    const std::string readings_text = readings_to_json(subject.readings).dump();
    const std::string component_ids_text = json(subject.component_subject_ids).dump();
    const std::string visually_similar_text = json(subject.visually_similar_subject_ids).dump();

    sqlite3_bind_int64(stmt, 1, subject.id);
    sqlite3_bind_text(stmt, 2, subject.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, subject.characters.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, subject.slug.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, subject.level);
    sqlite3_bind_text(stmt, 6, primary_meaning_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, meanings_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, aux_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, readings_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, component_ids_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, visually_similar_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, subject.meaning_mnemonic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, subject.reading_mnemonic.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, subject.data_updated_at.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("upsert_subject failed: " + message);
    }
    sqlite3_finalize(stmt);
}

std::optional<std::string> Store::get_meta(const std::string& key) {
    static const char* sql = "SELECT value FROM sync_meta WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare get_meta failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<std::string> result;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        result = text != nullptr ? std::string(reinterpret_cast<const char*>(text)) : std::string();
    } else if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("get_meta failed: " + message);
    }
    sqlite3_finalize(stmt);
    return result;
}

void Store::set_meta(const std::string& key, const std::string& value) {
    static const char* sql = "INSERT OR REPLACE INTO sync_meta (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare set_meta failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("set_meta failed: " + message);
    }
    sqlite3_finalize(stmt);
}

int Store::subject_count() {
    static const char* sql = "SELECT COUNT(*) FROM subject;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare subject_count failed: ") + sqlite3_errmsg(db_));
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool Store::insert_stat_snapshot(const ReviewStat& stat) {
    static const char* sql =
        "INSERT OR IGNORE INTO stat_snapshot "
        "(subject_id, data_updated_at, fetched_at, meaning_correct, meaning_incorrect, "
        "reading_correct, reading_incorrect, meaning_current_streak, reading_current_streak, "
        "percentage_correct) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare insert_stat_snapshot failed: ") +
                                  sqlite3_errmsg(db_));
    }

    const std::string fetched_at = now_iso8601();
    sqlite3_bind_int64(stmt, 1, stat.subject_id);
    sqlite3_bind_text(stmt, 2, stat.data_updated_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, fetched_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, stat.meaning_correct);
    sqlite3_bind_int(stmt, 5, stat.meaning_incorrect);
    sqlite3_bind_int(stmt, 6, stat.reading_correct);
    sqlite3_bind_int(stmt, 7, stat.reading_incorrect);
    sqlite3_bind_int(stmt, 8, stat.meaning_current_streak);
    sqlite3_bind_int(stmt, 9, stat.reading_current_streak);
    sqlite3_bind_int(stmt, 10, stat.percentage_correct);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("insert_stat_snapshot failed: " + message);
    }
    const bool inserted = sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return inserted;
}

std::optional<ReviewStat> Store::previous_snapshot(long long subject_id,
                                                     const std::string& before_data_updated_at) {
    static const char* sql =
        "SELECT subject_id, data_updated_at, meaning_correct, meaning_incorrect, "
        "reading_correct, reading_incorrect, meaning_current_streak, reading_current_streak, "
        "percentage_correct FROM stat_snapshot "
        "WHERE subject_id = ? AND data_updated_at < ? "
        "ORDER BY data_updated_at DESC LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare previous_snapshot failed: ") +
                                  sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, subject_id);
    sqlite3_bind_text(stmt, 2, before_data_updated_at.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<ReviewStat> result;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ReviewStat stat;
        stat.subject_id = sqlite3_column_int64(stmt, 0);
        const unsigned char* updated = sqlite3_column_text(stmt, 1);
        stat.data_updated_at = updated != nullptr ? reinterpret_cast<const char*>(updated) : "";
        stat.meaning_correct = sqlite3_column_int(stmt, 2);
        stat.meaning_incorrect = sqlite3_column_int(stmt, 3);
        stat.reading_correct = sqlite3_column_int(stmt, 4);
        stat.reading_incorrect = sqlite3_column_int(stmt, 5);
        stat.meaning_current_streak = sqlite3_column_int(stmt, 6);
        stat.reading_current_streak = sqlite3_column_int(stmt, 7);
        stat.percentage_correct = sqlite3_column_int(stmt, 8);
        result = stat;
    } else if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("previous_snapshot failed: " + message);
    }
    sqlite3_finalize(stmt);
    return result;
}

void Store::insert_failure_event(const FailureEvent& event) {
    static const char* sql =
        "INSERT INTO failure_event (subject_id, occurred_at, kind, session_id, cold_start) "
        "VALUES (?, ?, ?, NULL, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare insert_failure_event failed: ") +
                                  sqlite3_errmsg(db_));
    }

    const std::string kind_text = failure_kind_to_text(event.kind);
    sqlite3_bind_int64(stmt, 1, event.subject_id);
    sqlite3_bind_text(stmt, 2, event.occurred_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, kind_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, event.cold_start ? 1 : 0);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("insert_failure_event failed: " + message);
    }
    sqlite3_finalize(stmt);
}

void Store::upsert_assignment(const Assignment& assignment) {
    static const char* sql =
        "INSERT OR REPLACE INTO assignment "
        "(id, subject_id, subject_type, srs_stage, available_at, passed_at, data_updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare upsert_assignment failed: ") +
                                  sqlite3_errmsg(db_));
    }

    sqlite3_bind_int64(stmt, 1, assignment.id);
    sqlite3_bind_int64(stmt, 2, assignment.subject_id);
    sqlite3_bind_text(stmt, 3, assignment.subject_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, assignment.srs_stage);
    sqlite3_bind_text(stmt, 5, assignment.available_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, assignment.passed_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, assignment.data_updated_at.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("upsert_assignment failed: " + message);
    }
    sqlite3_finalize(stmt);
}

void Store::upsert_study_material(const wk_api::StudyMaterial& material) {
    static const char* sql =
        "INSERT OR REPLACE INTO study_material "
        "(id, subject_id, subject_type, meaning_note, reading_note, meaning_synonyms_json, "
        "data_updated_at) "
        "VALUES ((SELECT id FROM study_material WHERE subject_id = ?), ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare upsert_study_material failed: ") +
                                  sqlite3_errmsg(db_));
    }

    const std::string synonyms_text = json(material.meaning_synonyms).dump();
    sqlite3_bind_int64(stmt, 1, material.subject_id);
    sqlite3_bind_int64(stmt, 2, material.subject_id);
    sqlite3_bind_text(stmt, 3, material.subject_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, material.meaning_note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, material.reading_note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, synonyms_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, material.data_updated_at.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("upsert_study_material failed: " + message);
    }
    sqlite3_finalize(stmt);
}

std::vector<FailureEvent> Store::failure_events_without_session() {
    static const char* sql =
        "SELECT id, subject_id, occurred_at, kind, cold_start FROM failure_event "
        "WHERE session_id IS NULL ORDER BY occurred_at ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare failure_events_without_session failed: ") +
                                  sqlite3_errmsg(db_));
    }

    std::vector<FailureEvent> events;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        FailureEvent event;
        event.id = sqlite3_column_int64(stmt, 0);
        event.subject_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* occurred = sqlite3_column_text(stmt, 2);
        event.occurred_at = occurred != nullptr ? reinterpret_cast<const char*>(occurred) : "";
        const unsigned char* kind = sqlite3_column_text(stmt, 3);
        event.kind = failure_kind_from_text(kind != nullptr ? reinterpret_cast<const char*>(kind) : "");
        event.cold_start = sqlite3_column_int(stmt, 4) != 0;
        event.session_id = -1;
        events.push_back(event);
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("failure_events_without_session failed: " + message);
    }
    sqlite3_finalize(stmt);
    return events;
}

long long Store::insert_session(const Session& session) {
    static const char* sql = "INSERT INTO session (started_at, ended_at) VALUES (?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare insert_session failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_text(stmt, 1, session.started_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session.ended_at.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("insert_session failed: " + message);
    }
    const long long new_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    return new_id;
}

void Store::assign_failure_event_session(long long failure_event_id, long long session_id) {
    static const char* sql = "UPDATE failure_event SET session_id = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare assign_failure_event_session failed: ") +
                                  sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, failure_event_id);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("assign_failure_event_session failed: " + message);
    }
    sqlite3_finalize(stmt);
}

std::vector<Subject> Store::all_subjects() {
    static const char* sql =
        "SELECT id, type, characters, slug, level, meanings_json, aux_meanings_json, "
        "readings_json, component_ids_json, visually_similar_ids_json, meaning_mnemonic, "
        "reading_mnemonic, data_updated_at FROM subject;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_subjects failed: ") + sqlite3_errmsg(db_));
    }

    auto text_col = [&](int idx) -> std::string {
        const unsigned char* t = sqlite3_column_text(stmt, idx);
        return t != nullptr ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };

    std::vector<Subject> subjects;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Subject s;
        s.id = sqlite3_column_int64(stmt, 0);
        s.type = text_col(1);
        s.characters = text_col(2);
        s.slug = text_col(3);
        s.level = sqlite3_column_int(stmt, 4);
        s.meanings = meanings_from_json(text_col(5));
        s.auxiliary_meanings = aux_meanings_from_json(text_col(6));
        s.readings = readings_from_json(text_col(7));
        s.component_subject_ids = ids_from_json(text_col(8));
        s.visually_similar_subject_ids = ids_from_json(text_col(9));
        s.meaning_mnemonic = text_col(10);
        s.reading_mnemonic = text_col(11);
        s.data_updated_at = text_col(12);
        subjects.push_back(std::move(s));
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_subjects failed: " + message);
    }
    sqlite3_finalize(stmt);
    return subjects;
}

void Store::replace_similarity_edges(const std::vector<SimilarityEdge>& edges) {
    exec(db_, "BEGIN;");
    try {
        exec(db_, "DELETE FROM similarity_edge;");

        static const char* sql =
            "INSERT INTO similarity_edge (a_id, b_id, kind, weight) VALUES (?, ?, ?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare replace_similarity_edges failed: ") +
                                      sqlite3_errmsg(db_));
        }

        for (const auto& edge : edges) {
            const std::string kind_text = edge_kind_to_text(edge.kind);
            sqlite3_bind_int64(stmt, 1, edge.a_id);
            sqlite3_bind_int64(stmt, 2, edge.b_id);
            sqlite3_bind_text(stmt, 3, kind_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 4, edge.weight);

            const int rc = sqlite3_step(stmt);
            if (rc != SQLITE_DONE) {
                const std::string message = sqlite3_errmsg(db_);
                sqlite3_finalize(stmt);
                throw std::runtime_error("replace_similarity_edges insert failed: " + message);
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    } catch (...) {
        exec(db_, "ROLLBACK;");
        throw;
    }
    exec(db_, "COMMIT;");
}

std::vector<SimilarityEdge> Store::edges_for(long long subject_id) {
    static const char* sql =
        "SELECT a_id, b_id, kind, weight FROM similarity_edge WHERE a_id = ? OR b_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare edges_for failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, subject_id);
    sqlite3_bind_int64(stmt, 2, subject_id);

    std::vector<SimilarityEdge> edges;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SimilarityEdge edge;
        edge.a_id = sqlite3_column_int64(stmt, 0);
        edge.b_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* kind_text = sqlite3_column_text(stmt, 2);
        edge.kind =
            edge_kind_from_text(kind_text != nullptr ? reinterpret_cast<const char*>(kind_text) : "");
        edge.weight = sqlite3_column_double(stmt, 3);
        edges.push_back(edge);
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("edges_for failed: " + message);
    }
    sqlite3_finalize(stmt);
    return edges;
}

std::vector<SimilarityEdge> Store::all_similarity_edges() {
    static const char* sql = "SELECT a_id, b_id, kind, weight FROM similarity_edge;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_similarity_edges failed: ") +
                                  sqlite3_errmsg(db_));
    }

    std::vector<SimilarityEdge> edges;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SimilarityEdge edge;
        edge.a_id = sqlite3_column_int64(stmt, 0);
        edge.b_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* kind_text = sqlite3_column_text(stmt, 2);
        edge.kind =
            edge_kind_from_text(kind_text != nullptr ? reinterpret_cast<const char*>(kind_text) : "");
        edge.weight = sqlite3_column_double(stmt, 3);
        edges.push_back(edge);
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_similarity_edges failed: " + message);
    }
    sqlite3_finalize(stmt);
    return edges;
}

std::vector<ReviewStat> Store::latest_stat_per_subject() {
    static const char* sql =
        "SELECT subject_id, MAX(data_updated_at) AS data_updated_at, meaning_correct, "
        "meaning_incorrect, reading_correct, reading_incorrect, meaning_current_streak, "
        "reading_current_streak, percentage_correct FROM stat_snapshot GROUP BY subject_id;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare latest_stat_per_subject failed: ") +
                                  sqlite3_errmsg(db_));
    }

    std::vector<ReviewStat> stats;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ReviewStat stat;
        stat.subject_id = sqlite3_column_int64(stmt, 0);
        const unsigned char* updated = sqlite3_column_text(stmt, 1);
        stat.data_updated_at = updated != nullptr ? reinterpret_cast<const char*>(updated) : "";
        stat.meaning_correct = sqlite3_column_int(stmt, 2);
        stat.meaning_incorrect = sqlite3_column_int(stmt, 3);
        stat.reading_correct = sqlite3_column_int(stmt, 4);
        stat.reading_incorrect = sqlite3_column_int(stmt, 5);
        stat.meaning_current_streak = sqlite3_column_int(stmt, 6);
        stat.reading_current_streak = sqlite3_column_int(stmt, 7);
        stat.percentage_correct = sqlite3_column_int(stmt, 8);
        stats.push_back(stat);
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("latest_stat_per_subject failed: " + message);
    }
    sqlite3_finalize(stmt);
    return stats;
}

std::vector<Session> Store::all_sessions() {
    static const char* sql = "SELECT id, started_at, ended_at FROM session ORDER BY started_at ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_sessions failed: ") + sqlite3_errmsg(db_));
    }

    std::vector<Session> sessions;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Session session;
        session.id = sqlite3_column_int64(stmt, 0);
        const unsigned char* started = sqlite3_column_text(stmt, 1);
        session.started_at = started != nullptr ? reinterpret_cast<const char*>(started) : "";
        const unsigned char* ended = sqlite3_column_text(stmt, 2);
        session.ended_at = ended != nullptr ? reinterpret_cast<const char*>(ended) : "";
        sessions.push_back(std::move(session));
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_sessions failed: " + message);
    }
    sqlite3_finalize(stmt);
    return sessions;
}

std::vector<FailureEvent> Store::all_failure_events() {
    static const char* sql =
        "SELECT id, subject_id, occurred_at, kind, session_id, cold_start FROM failure_event "
        "ORDER BY occurred_at ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_failure_events failed: ") +
                                  sqlite3_errmsg(db_));
    }

    std::vector<FailureEvent> events;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        FailureEvent event;
        event.id = sqlite3_column_int64(stmt, 0);
        event.subject_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* occurred = sqlite3_column_text(stmt, 2);
        event.occurred_at = occurred != nullptr ? reinterpret_cast<const char*>(occurred) : "";
        const unsigned char* kind = sqlite3_column_text(stmt, 3);
        event.kind = failure_kind_from_text(kind != nullptr ? reinterpret_cast<const char*>(kind) : "");
        event.session_id =
            sqlite3_column_type(stmt, 4) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 4);
        event.cold_start = sqlite3_column_int(stmt, 5) != 0;
        events.push_back(event);
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_failure_events failed: " + message);
    }
    sqlite3_finalize(stmt);
    return events;
}

std::vector<Assignment> Store::all_assignments() {
    static const char* sql =
        "SELECT id, subject_id, subject_type, srs_stage, available_at, passed_at, data_updated_at "
        "FROM assignment;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_assignments failed: ") + sqlite3_errmsg(db_));
    }

    auto text_col = [&](int idx) -> std::string {
        const unsigned char* t = sqlite3_column_text(stmt, idx);
        return t != nullptr ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };

    std::vector<Assignment> assignments;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Assignment assignment;
        assignment.id = sqlite3_column_int64(stmt, 0);
        assignment.subject_id = sqlite3_column_int64(stmt, 1);
        assignment.subject_type = text_col(2);
        assignment.srs_stage = sqlite3_column_int(stmt, 3);
        assignment.available_at = text_col(4);
        assignment.passed_at = text_col(5);
        assignment.data_updated_at = text_col(6);
        assignments.push_back(std::move(assignment));
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_assignments failed: " + message);
    }
    sqlite3_finalize(stmt);
    return assignments;
}

std::vector<wk_api::StudyMaterial> Store::all_study_materials() {
    static const char* sql =
        "SELECT subject_id, subject_type, meaning_note, reading_note, meaning_synonyms_json, "
        "data_updated_at FROM study_material;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_study_materials failed: ") +
                                  sqlite3_errmsg(db_));
    }

    auto text_col = [&](int idx) -> std::string {
        const unsigned char* t = sqlite3_column_text(stmt, idx);
        return t != nullptr ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };

    std::vector<wk_api::StudyMaterial> materials;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        wk_api::StudyMaterial material;
        material.subject_id = sqlite3_column_int64(stmt, 0);
        material.subject_type = text_col(1);
        material.meaning_note = text_col(2);
        material.reading_note = text_col(3);
        const std::string synonyms_text = text_col(4);
        material.meaning_synonyms =
            synonyms_text.empty() ? std::vector<std::string>{} : json::parse(synonyms_text).get<std::vector<std::string>>();
        material.data_updated_at = text_col(5);
        materials.push_back(std::move(material));
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_study_materials failed: " + message);
    }
    sqlite3_finalize(stmt);
    return materials;
}

void Store::insert_drill_result(const DrillResult& result) {
    static const char* sql =
        "INSERT INTO drill_result (subject_id, distractor_id, correct, answered_at) "
        "VALUES (?, ?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare insert_drill_result failed: ") +
                                  sqlite3_errmsg(db_));
    }

    sqlite3_bind_int64(stmt, 1, result.subject_id);
    sqlite3_bind_int64(stmt, 2, result.distractor_id);
    sqlite3_bind_int(stmt, 3, result.correct ? 1 : 0);
    sqlite3_bind_text(stmt, 4, result.answered_at.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("insert_drill_result failed: " + message);
    }
    sqlite3_finalize(stmt);
}
