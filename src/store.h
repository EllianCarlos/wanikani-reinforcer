#pragma once

#include <optional>
#include <string>

#include "model.h"

struct sqlite3;

// Wraps the wkr SQLite database. Opens (creating if missing) and runs the
// full schema on construction — every table the whole plan needs, not
// just what this task uses — so later tasks only add queries, never
// migrations.
class Store {
public:
    explicit Store(const std::string& db_path);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    // Inserts or replaces the subject row for subject.id. Subjects rarely
    // change, so replace-on-conflict is correct (unlike stat_snapshot,
    // which must never overwrite history).
    void upsert_subject(const Subject& subject);

    std::optional<std::string> get_meta(const std::string& key);
    void set_meta(const std::string& key, const std::string& value);

    int subject_count();

private:
    sqlite3* db_ = nullptr;
};
