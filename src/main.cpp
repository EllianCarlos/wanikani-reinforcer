#include <unistd.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "confusion.h"
#include "drill.h"
#include "model.h"
#include "report.h"
#include "store.h"
#include "sync.h"
#include "wk_api.h"

namespace {

// The `wkr sync` command: resolves config, opens the store, and runs the
// real sync pipeline (sync.cpp — the same code tests/test_integration.cpp
// drives with fixture fetchers), then prints a one-line summary. The
// ordering logic itself deliberately does NOT live here; see sync.h.
int run_sync_command(int session_gap_minutes) {
    try {
        Config config = Config::load();
        // Unlike `report`/`drill`, sync makes HTTP calls, so fail fast and
        // clearly here rather than partway through the first fetch.
        config.require_token();
        Store store(config.db_path);

        const SyncSummary summary = run_sync(store, live_sync_fetchers(), session_gap_minutes);

        std::cout << "synced " << summary.subjects << " subjects, " << summary.stats << " stats ("
                  << summary.failures << " failures), " << summary.assignments << " assignments, "
                  << summary.study_materials << " study materials";
        if (summary.similarity_edge_count >= 0) {
            std::cout << ", rebuilt similarity graph (" << summary.similarity_edge_count << " edges)";
        }
        std::cout << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

// Current UTC time in the same format model.h's parse_wk_timestamp
// expects ("2026-08-16T03:14:07Z"), used as compute_confusion_pairs'
// decay reference point. Mirrors store.cpp's now_iso8601 (kept private
// there), since main.cpp needs the same "now, as WaniKani-style text"
// value but confusion.cpp deliberately takes it as a parameter rather
// than reading the clock itself, to stay testable.
std::string now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buf.data());
}

// True when stdout is an interactive terminal (not piped/redirected) --
// the gate for emitting ANSI colour codes, so piping `wkr report`/`wkr
// drill` output to a file or another program never fills it with raw
// escape codes.
bool stdout_is_tty() { return isatty(fileno(stdout)) != 0; }

int run_report() {
    try {
        Config config = Config::load();
        Store store(config.db_path);

        const std::vector<Subject> subjects = store.all_subjects();
        if (subjects.empty()) {
            std::cout << "No data yet — run 'wkr sync' first." << std::endl;
            return 0;
        }

        const std::vector<ReviewStat> latest_stats = store.latest_stat_per_subject();
        const std::vector<Session> sessions = store.all_sessions();
        const std::vector<FailureEvent> events = store.all_failure_events();
        const std::vector<SimilarityEdge> edges = store.all_similarity_edges();
        const std::vector<Assignment> assignments = store.all_assignments();
        const std::vector<wk_api::StudyMaterial> study_materials = store.all_study_materials();

        const std::vector<LeechEntry> leeches = compute_leech_scores(latest_stats);
        const std::vector<ConfusionPair> pairs =
            compute_confusion_pairs(sessions, events, edges, leeches, now_iso8601());

        print_report(pairs, leeches, subjects, assignments, study_materials, latest_stats, std::cout,
                     stdout_is_tty());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

// Named distinctly from drill.h's run_drill (which this wraps) to avoid
// two same-named functions with different signatures living in the same
// translation unit -- confusing even though legal overloading.
int run_drill_command(int question_count) {
    try {
        Config config = Config::load();
        Store store(config.db_path);

        const std::vector<Subject> subjects = store.all_subjects();
        if (subjects.empty()) {
            std::cout << "No data yet — run 'wkr sync' first." << std::endl;
            return 0;
        }

        const std::vector<ReviewStat> latest_stats = store.latest_stat_per_subject();
        const std::vector<Session> sessions = store.all_sessions();
        const std::vector<FailureEvent> events = store.all_failure_events();
        const std::vector<SimilarityEdge> edges = store.all_similarity_edges();
        const std::vector<Assignment> assignments = store.all_assignments();
        (void)assignments;  // loaded for parity with `report`'s data-loading pattern; unused by drill
        const std::vector<wk_api::StudyMaterial> study_materials = store.all_study_materials();

        const std::vector<LeechEntry> leeches = compute_leech_scores(latest_stats);
        const std::vector<ConfusionPair> pairs =
            compute_confusion_pairs(sessions, events, edges, leeches, now_iso8601());

        if (pairs.empty()) {
            std::cout << "No confusion pairs yet — keep reviewing and run 'wkr sync' again." << std::endl;
            return 0;
        }

        run_drill(pairs, subjects, study_materials, store, question_count, std::cin, std::cout,
                  stdout_is_tty());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

void print_usage() {
    std::cerr << "usage: wkr <sync [--session-gap-minutes N]|report|drill [--count N]>" << std::endl;
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

// Parses `--count N` out of argv[2..], following the same convention as
// parse_session_gap_minutes above. Returns kDefaultDrillQuestionCount if
// the flag is absent.
int parse_drill_count(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--count") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--count requires a value");
            }
            const std::string value = argv[i + 1];
            try {
                return std::stoi(value);
            } catch (const std::exception&) {
                throw std::runtime_error("--count value must be an integer, got: " + value);
            }
        }
    }
    return kDefaultDrillQuestionCount;
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
            return run_sync_command(parse_session_gap_minutes(argc, argv));
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }
    if (command == "report") {
        return run_report();
    }
    if (command == "drill") {
        try {
            return run_drill_command(parse_drill_count(argc, argv));
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
            return 1;
        }
    }

    print_usage();
    return 1;
}
