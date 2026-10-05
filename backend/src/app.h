#pragma once

#include <string>
#include <vector>

#include "model.h"
#include "report.h"
#include "store.h"
#include "wk_api.h"

// Shared assembly of the Store reads + pure computations that several
// frontends (CLI commands, the RPC server) all need.

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ", the format model.h's
// parse_wk_timestamp expects.
std::string utc_now_iso8601();

// Everything `report`/`drill` load before computing confusion pairs.
struct AppData {
    std::vector<Subject> subjects;
    std::vector<ReviewStat> latest_stats;
    std::vector<Assignment> assignments;
    std::vector<wk_api::StudyMaterial> study_materials;
    std::vector<LeechEntry> leeches;
    std::vector<ConfusionPair> pairs;  // descending by score
};
AppData load_app_data(Store& store, const std::string& now_iso8601);

struct NamedMissedPair {
    std::string subject;
    std::string distractor;
    int miss_count = 0;
};

struct NamedTrend {
    std::string characters;
    LeechTrendEntry trend;
};

// What the TUI home screen shows: the report blocks, due counts, and the
// `stats` sections. `has_data` is false before the first sync.
struct Dashboard {
    bool has_data = false;
    std::vector<ReportEntry> leeches;
    int due_pairs = 0;
    int due_burned = 0;
    DrillStats drill;
    std::vector<NamedMissedPair> missed_pairs;
    SessionStats sessions;
    std::vector<NamedTrend> trend;
};
Dashboard build_dashboard(Store& store, const std::string& now_iso8601);
