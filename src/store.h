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

    // Every similarity_edge row in the table. Used by `wkr report`, which
    // (unlike edges_for) needs the whole graph at once to score confusion
    // pairs rather than one subject's neighborhood.
    std::vector<SimilarityEdge> all_similarity_edges();

    // One ReviewStat per subject: its most recent stat_snapshot row (the
    // row with the largest data_updated_at for that subject_id). Relies
    // on SQLite's documented "bare column" behavior — when a query's only
    // aggregate is MAX()/MIN(), the non-aggregated columns in the same
    // SELECT come from the row that produced that MAX/MIN — so this is a
    // single GROUP BY query rather than a correlated subquery per row.
    // subject_type is left empty (stat_snapshot does not persist it; see
    // previous_snapshot, which has the same limitation).
    std::vector<ReviewStat> latest_stat_per_subject();

    // Every session row, ordered by started_at ascending.
    std::vector<Session> all_sessions();

    // Every failure_event row (assigned to a session or not), ordered by
    // occurred_at ascending, with session_id populated (-1 if the DB
    // column is NULL). Unlike failure_events_without_session, this is not
    // filtered to session_id IS NULL — confusion-pair scoring needs every
    // event's session membership, not just the ones sync hasn't grouped
    // yet.
    std::vector<FailureEvent> all_failure_events();

    // Every assignment row.
    std::vector<Assignment> all_assignments();

    // Every study_material row.
    std::vector<wk_api::StudyMaterial> all_study_materials();

private:
    sqlite3* db_ = nullptr;
};
