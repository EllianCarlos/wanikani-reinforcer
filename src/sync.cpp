#include "sync.h"

#include <optional>

#include "similarity.h"

namespace {

// Advances sync_meta[cursor_key] to the max data_updated_at seen across
// `items`, using `updated_at_of` to pull that field out of each item.
// Leaves the cursor untouched if `items` is empty (nothing to advance
// past) or every item's data_updated_at happens to be <= the existing
// cursor.
template <typename T, typename Getter>
void advance_cursor(Store& store, const std::string& cursor_key, const std::string& start_value,
                     const std::vector<T>& items, Getter updated_at_of) {
    std::string max_updated_at = start_value;
    for (const auto& item : items) {
        const std::string& value = updated_at_of(item);
        if (value > max_updated_at) {
            max_updated_at = value;
        }
    }
    if (!max_updated_at.empty() && max_updated_at != start_value) {
        store.set_meta(cursor_key, max_updated_at);
    }
}

}  // namespace

SyncFetchers live_sync_fetchers() {
    SyncFetchers fetchers;
    fetchers.subjects = [](const std::string& cursor) { return wk_api::fetch_all_subjects(cursor); };
    fetchers.review_statistics = [](const std::string& cursor) {
        return wk_api::fetch_all_review_statistics(cursor);
    };
    fetchers.assignments = [](const std::string& cursor) { return wk_api::fetch_all_assignments(cursor); };
    fetchers.study_materials = [](const std::string& cursor) {
        return wk_api::fetch_all_study_materials(cursor);
    };
    return fetchers;
}

SyncSummary run_sync(Store& store, const SyncFetchers& fetchers, int session_gap_minutes) {
    SyncSummary summary;

    // 1. Subjects. The upsert loop is wrapped in one transaction: a first
    // sync inserts ~9000 rows, and without this each one would be its own
    // implicit WAL transaction.
    const std::string subjects_cursor = store.get_meta("subjects_cursor").value_or("");
    const std::vector<Subject> subjects = fetchers.subjects(subjects_cursor);
    store.begin_transaction();
    try {
        for (const auto& subject : subjects) {
            store.upsert_subject(subject);
        }
    } catch (...) {
        store.rollback_transaction();
        throw;
    }
    store.commit_transaction();
    advance_cursor(store, "subjects_cursor", subjects_cursor, subjects,
                    [](const Subject& s) -> const std::string& { return s.data_updated_at; });

    // 2. Review statistics: detect failures by diffing each new snapshot
    // against the previous one for that subject (or, if there is no
    // previous one, by checking for an already-broken streak — see
    // classify_failure in model.h). Batched into one transaction for the
    // same reason as the subjects loop above (another ~9000 rows, each
    // potentially writing a stat_snapshot plus a failure_event).
    const std::string stats_cursor = store.get_meta("stats_cursor").value_or("");
    const std::vector<ReviewStat> stats = fetchers.review_statistics(stats_cursor);
    store.begin_transaction();
    try {
        for (const auto& stat : stats) {
            if (!store.insert_stat_snapshot(stat)) {
                // Already seen this exact (subject_id, data_updated_at)
                // snapshot in a prior sync — this is the idempotency
                // gate, so skip delta detection entirely.
                continue;
            }
            const std::optional<ReviewStat> prev =
                store.previous_snapshot(stat.subject_id, stat.data_updated_at);
            for (const auto& event : classify_failure(prev, stat)) {
                store.insert_failure_event(event);
                ++summary.failures;
            }
        }
    } catch (...) {
        store.rollback_transaction();
        throw;
    }
    store.commit_transaction();
    advance_cursor(store, "stats_cursor", stats_cursor, stats,
                    [](const ReviewStat& s) -> const std::string& { return s.data_updated_at; });

    // 3. Assignments.
    const std::string assignments_cursor = store.get_meta("assignments_cursor").value_or("");
    const std::vector<Assignment> assignments = fetchers.assignments(assignments_cursor);
    for (const auto& assignment : assignments) {
        store.upsert_assignment(assignment);
    }
    advance_cursor(store, "assignments_cursor", assignments_cursor, assignments,
                    [](const Assignment& a) -> const std::string& { return a.data_updated_at; });

    // 4. Study materials.
    const std::string study_materials_cursor = store.get_meta("study_materials_cursor").value_or("");
    const std::vector<wk_api::StudyMaterial> study_materials =
        fetchers.study_materials(study_materials_cursor);
    for (const auto& material : study_materials) {
        store.upsert_study_material(material);
    }
    advance_cursor(
        store, "study_materials_cursor", study_materials_cursor, study_materials,
        [](const wk_api::StudyMaterial& m) -> const std::string& { return m.data_updated_at; });

    // 5. Session grouping: pull every failure event not yet assigned to a
    // session, group them by the configured gap, and persist the
    // grouping. group_into_sessions assigns each event a placeholder
    // session_id (an index into its returned vector); remap that to a
    // real database session id here.
    std::vector<FailureEvent> ungrouped = store.failure_events_without_session();
    const std::vector<Session> sessions = group_into_sessions(ungrouped, session_gap_minutes);

    std::vector<long long> real_session_ids;
    real_session_ids.reserve(sessions.size());
    for (const auto& session : sessions) {
        real_session_ids.push_back(store.insert_session(session));
    }
    for (const auto& event : ungrouped) {
        const long long real_session_id = real_session_ids.at(static_cast<size_t>(event.session_id));
        store.assign_failure_event_session(event.id, real_session_id);
    }
    // ended_at was already set correctly per-session when insert_session
    // ran (Session::ended_at reflects the last event seen in that group by
    // the time group_into_sessions returned), so no further per-event
    // UPDATE session is needed here.

    // 6. Similarity graph: a pure function of subject data, so normally
    // only worth rebuilding when the subjects sync above actually
    // inserted/replaced rows this run — an idle `wkr sync` with nothing
    // new shouldn't pay the rebuild cost every time.
    //
    // The second half of the gate is not an optimisation but a recovery
    // path: the subjects cursor is advanced immediately after step 1, so
    // if any later step throws (rate-limit exhaustion, transport error)
    // the run dies with the cursor already current but the graph never
    // built. Every subsequent sync would then fetch zero subjects and,
    // gated on `!subjects.empty()` alone, skip the rebuild forever —
    // silently leaving the similarity graph permanently empty. Rebuilding
    // whenever the table is empty makes the next sync repair that.
    if (!subjects.empty() || store.all_similarity_edges().empty()) {
        const std::vector<Subject> graph_subjects = store.all_subjects();
        const std::vector<SimilarityEdge> edges = build_similarity_graph(graph_subjects);
        store.replace_similarity_edges(edges);
        summary.similarity_edge_count = static_cast<int>(edges.size());
    }

    summary.subjects = subjects.size();
    summary.stats = stats.size();
    summary.assignments = assignments.size();
    summary.study_materials = study_materials.size();
    return summary;
}
