#include "json_codec.h"

void to_json(nlohmann::json& j, const DrillQuestion& q) {
    j = {{"kind", q.kind == DrillQuestion::Kind::ForcedChoice ? "forced_choice" : "production"},
         {"prompt", q.prompt},
         {"index", q.index},
         {"total", q.total}};
    if (q.kind == DrillQuestion::Kind::ForcedChoice) {
        j["shown_text"] = q.shown_text;
        j["asks_reading"] = q.asks_reading;
        j["options"] = q.options;
    }
}

void to_json(nlohmann::json& j, const DrillFeedback& f) {
    j = {{"correct", f.correct}, {"retry", false}, {"detail", f.detail}, {"advice", f.advice}, {"reason", f.reason}};
}

void to_json(nlohmann::json& j, const BurnedQuestion& q) {
    j = {{"kind", q.is_reading ? "reading" : "meaning"},
         {"prompt", q.prompt},
         {"characters", q.characters},
         {"type_label", q.type_label},
         {"subject_id", q.subject_id}};
}

void to_json(nlohmann::json& j, const BurnedFeedback& f) {
    j = {{"correct", f.correct}, {"retry", f.retry}, {"detail", f.detail}};
}

void to_json(nlohmann::json& j, const ReportNeighbor& n) {
    j = {{"subject_id", n.subject_id}, {"characters", n.characters}, {"meaning", n.meaning}, {"srs_label", n.srs_label}};
}

void to_json(nlohmann::json& j, const ReportEntry& e) {
    j = {{"subject_id", e.subject_id},
         {"characters", e.characters},
         {"meanings", e.meanings},
         {"percentage_correct", e.percentage_correct},
         {"srs_stage", e.srs_stage_name},
         {"fails_meaning", e.fails_meaning},
         {"neighbors_heading", e.neighbors_heading},
         {"neighbors", e.neighbors},
         {"advice", e.advice}};
}

void to_json(nlohmann::json& j, const SyncSummary& s) {
    j = {{"subjects", s.subjects},
         {"stats", s.stats},
         {"assignments", s.assignments},
         {"study_materials", s.study_materials},
         {"failures", s.failures},
         {"similarity_edge_count", s.similarity_edge_count}};
}

void to_json(nlohmann::json& j, const Dashboard& d) {
    nlohmann::json missed = nlohmann::json::array();
    for (const auto& m : d.missed_pairs) {
        missed.push_back({{"subject", m.subject}, {"distractor", m.distractor}, {"miss_count", m.miss_count}});
    }
    nlohmann::json trend = nlohmann::json::array();
    for (const auto& t : d.trend) {
        trend.push_back({{"characters", t.characters},
                         {"snapshot_count", t.trend.snapshot_count},
                         {"first_percentage", t.trend.first_percentage},
                         {"last_percentage", t.trend.last_percentage},
                         {"delta", t.trend.delta}});
    }
    j = {{"has_data", d.has_data},
         {"leeches", d.leeches},
         {"due", {{"pairs", d.due_pairs}, {"burned", d.due_burned}}},
         {"drill", {{"total", d.drill.total}, {"correct", d.drill.correct}, {"current_streak", d.drill.current_streak}}},
         {"missed_pairs", missed},
         {"sessions",
          {{"session_count", d.sessions.session_count},
           {"avg_failures_per_session", d.sessions.avg_failures_per_session},
           {"sessions_last_7_days", d.sessions.sessions_last_7_days}}},
         {"trend", trend}};
}
