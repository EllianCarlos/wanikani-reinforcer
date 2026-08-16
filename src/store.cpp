#include "store.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace {

using nlohmann::json;

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
