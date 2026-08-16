#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "model.h"
#include "store.h"
#include "wk_api.h"

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

int run_sync(int session_gap_minutes) {
    try {
        Config config = Config::load();
        Store store(config.db_path);

        // 1. Subjects.
        const std::string subjects_cursor = store.get_meta("subjects_cursor").value_or("");
        const std::vector<Subject> subjects = wk_api::fetch_all_subjects(subjects_cursor);
        for (const auto& subject : subjects) {
            store.upsert_subject(subject);
        }
        advance_cursor(store, "subjects_cursor", subjects_cursor, subjects,
                        [](const Subject& s) -> const std::string& { return s.data_updated_at; });

        // 2. Review statistics: detect failures by diffing each new
        // snapshot against the previous one for that subject (or, if
        // there is no previous one, by checking for an already-broken
        // streak — see classify_failure in model.h).
        const std::string stats_cursor = store.get_meta("stats_cursor").value_or("");
        const std::vector<ReviewStat> stats = wk_api::fetch_all_review_statistics(stats_cursor);
        int failure_count = 0;
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
                ++failure_count;
            }
        }
        advance_cursor(store, "stats_cursor", stats_cursor, stats,
                        [](const ReviewStat& s) -> const std::string& { return s.data_updated_at; });

        // 3. Assignments.
        const std::string assignments_cursor = store.get_meta("assignments_cursor").value_or("");
        const std::vector<Assignment> assignments = wk_api::fetch_all_assignments(assignments_cursor);
        for (const auto& assignment : assignments) {
            store.upsert_assignment(assignment);
        }
        advance_cursor(store, "assignments_cursor", assignments_cursor, assignments,
                        [](const Assignment& a) -> const std::string& { return a.data_updated_at; });

        // 4. Study materials.
        const std::string study_materials_cursor = store.get_meta("study_materials_cursor").value_or("");
        const std::vector<wk_api::StudyMaterial> study_materials =
            wk_api::fetch_all_study_materials(study_materials_cursor);
        for (const auto& material : study_materials) {
            store.upsert_study_material(material);
        }
        advance_cursor(
            store, "study_materials_cursor", study_materials_cursor, study_materials,
            [](const wk_api::StudyMaterial& m) -> const std::string& { return m.data_updated_at; });

        // 5. Session grouping: pull every failure event not yet assigned
        // to a session, group them by the configured gap, and persist
        // the grouping. group_into_sessions assigns each event a
        // placeholder session_id (an index into its returned vector);
        // remap that to a real database session id here.
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
        // ran (Session::ended_at reflects the last event seen in that
        // group by the time group_into_sessions returned), so no further
        // per-event UPDATE session is needed here.

        std::cout << "synced " << subjects.size() << " subjects, " << stats.size() << " stats ("
                  << failure_count << " failures), " << assignments.size() << " assignments, "
                  << study_materials.size() << " study materials" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

int run_report() {
    std::cout << "not implemented yet" << std::endl;
    return 0;
}

int run_drill() {
    std::cout << "not implemented yet" << std::endl;
    return 0;
}

void print_usage() {
    std::cerr << "usage: wkr <sync [--session-gap-minutes N]|report|drill>" << std::endl;
}

constexpr int kDefaultSessionGapMinutes = 45;

// Parses `--session-gap-minutes N` out of argv[2..]. Returns the default
// if the flag is absent. Throws std::runtime_error on a malformed value
// (missing argument or not an integer) so the user gets a clear error
// instead of silently falling back to the default.
int parse_session_gap_minutes(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--session-gap-minutes") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--session-gap-minutes requires a value");
            }
            const std::string value = argv[i + 1];
            try {
                return std::stoi(value);
            } catch (const std::exception&) {
                throw std::runtime_error("--session-gap-minutes value must be an integer, got: " + value);
            }
        }
    }
    return kDefaultSessionGapMinutes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string command = argv[1];
    if (command == "sync") {
        try {
            return run_sync(parse_session_gap_minutes(argc, argv));
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }
    if (command == "report") {
        return run_report();
    }
    if (command == "drill") {
        return run_drill();
    }

    print_usage();
    return 1;
}
