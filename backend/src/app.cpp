#include "app.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <map>

#include "burned.h"
#include "confusion.h"
#include "srs.h"
#include "stats.h"

namespace {

std::string display_name(const Subject& subject) {
    return subject.characters.empty() ? subject.slug : subject.characters;
}

}  // namespace

std::string utc_now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buf.data());
}

AppData load_app_data(Store& store, const std::string& now_iso8601) {
    AppData data;
    data.subjects = store.all_subjects();
    data.latest_stats = store.latest_stat_per_subject();
    data.assignments = store.all_assignments();
    data.study_materials = store.all_study_materials();
    const std::vector<Session> sessions = store.all_sessions();
    const std::vector<FailureEvent> events = store.all_failure_events();
    const std::vector<SimilarityEdge> edges = store.all_similarity_edges();
    data.leeches = compute_leech_scores(data.latest_stats);
    data.pairs = compute_confusion_pairs(sessions, events, edges, data.leeches, now_iso8601);
    return data;
}

Dashboard build_dashboard(Store& store, const std::string& now_iso8601) {
    Dashboard d;
    const AppData data = load_app_data(store, now_iso8601);
    d.has_data = !data.subjects.empty();
    if (!d.has_data) {
        return d;
    }

    std::map<long long, std::string> names;
    for (const auto& s : data.subjects) {
        names[s.id] = display_name(s);
    }
    const auto name_of = [&names](long long id) {
        const auto it = names.find(id);
        return it != names.end() ? it->second : std::to_string(id);
    };

    d.leeches = build_report(data.pairs, data.leeches, data.subjects, data.assignments, data.study_materials,
                             data.latest_stats, store.all_pair_srs(), now_iso8601);
    d.due_pairs = static_cast<int>(select_due_pairs(data.pairs, store.all_pair_srs(), now_iso8601).size());
    d.due_burned = static_cast<int>(
        select_due_burned(data.assignments, store.all_burned_srs(), now_iso8601, resolve_burned_types("", ""))
            .size());

    const std::vector<DrillResult> drill_results = store.all_drill_results();
    d.drill = compute_drill_stats(drill_results);
    for (const auto& m : compute_missed_pairs(drill_results, 5)) {
        d.missed_pairs.push_back({name_of(m.subject_id), name_of(m.distractor_id), m.miss_count});
    }
    d.sessions = compute_session_stats(store.all_sessions(), store.all_failure_events(), now_iso8601);

    std::vector<ReviewStat> history;
    for (const auto& leech : data.leeches) {
        const std::vector<ReviewStat> snaps = store.stat_snapshots_for(leech.subject_id);
        history.insert(history.end(), snaps.begin(), snaps.end());
    }
    for (const auto& t : compute_leech_trend(history)) {
        d.trend.push_back({name_of(t.subject_id), t});
    }
    return d;
}
