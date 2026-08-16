#pragma once

#include <optional>
#include <string>
#include <vector>

#include "model.h"
#include "wk_api.h"

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

    // Inserts a stat_snapshot row for (stat.subject_id, stat.data_updated_at)
    // unless one already exists (INSERT OR IGNORE — the primary key on
    // that pair is what makes re-syncing idempotent). Returns true iff a
    // row was actually inserted, which the sync logic uses to decide
    // whether this snapshot is new and delta detection should run.
    bool insert_stat_snapshot(const ReviewStat& stat);

    // Returns the stat_snapshot row for `subject_id` with the largest
    // data_updated_at strictly less than `before_data_updated_at`, or
    // nullopt if there is none (i.e. this is the first snapshot ever
    // seen for that subject — a "cold start").
    std::optional<ReviewStat> previous_snapshot(long long subject_id,
                                                 const std::string& before_data_updated_at);

    void insert_failure_event(const FailureEvent& event);

    // Inserts or replaces the assignment row for assignment.id.
    void upsert_assignment(const Assignment& assignment);

    // Inserts or replaces the study_material row for material.subject_id.
    void upsert_study_material(const wk_api::StudyMaterial& material);

    // Returns every failure_event row not yet assigned to a session
    // (session_id IS NULL in the DB), ordered by occurred_at ascending.
    std::vector<FailureEvent> failure_events_without_session();

    // Inserts a new session row and returns its id.
    long long insert_session(const Session& session);

    // Points failure_event.id == failure_event_id at session_id.
    void assign_failure_event_session(long long failure_event_id, long long session_id);

    // Reconstructs every subject row into a Subject, decoding the JSON
    // columns back into their vector fields. Feeds
    // build_similarity_graph, which needs the full subject set in memory.
    std::vector<Subject> all_subjects();

    // Full rebuild of the similarity_edge table: deletes every existing
    // row and bulk-inserts `edges`, wrapped in one transaction (BEGIN/
    // COMMIT) since a full subject set can produce thousands of edges.
    // Matches the plan: "Rebuild the similarity graph if the subject
    // data changed" — this is a replace, not an incremental diff.
    void replace_similarity_edges(const std::vector<SimilarityEdge>& edges);

    // Every similarity_edge row touching subject_id, on either side of
    // the pair (a_id == subject_id OR b_id == subject_id).
    std::vector<SimilarityEdge> edges_for(long long subject_id);

private:
    sqlite3* db_ = nullptr;
};
