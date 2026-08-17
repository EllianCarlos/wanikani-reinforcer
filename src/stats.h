#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "model.h"

// Computes local-only metrics from data already loaded from Store: drill
// history, session summary, and per-leech accuracy trend. Every function
// here is pure (no DB or HTTP access) -- main.cpp loads from Store and
// passes the result in, the same split confusion.h and report.cpp already
// use.

// Overall accuracy plus the current correct-streak (consecutive correct
// answers counting backward from the most recently answered row) from
// every recorded drill_result. `results` need not be pre-sorted; this
// sorts its own copy by answered_at ascending to compute the streak.
DrillStats compute_drill_stats(std::vector<DrillResult> results);

// The top `limit` (subject_id, distractor_id) pairs by how often that
// exact pairing was answered incorrectly, descending by miss_count. Pairs
// tied on miss_count keep the order they first appear in `results`.
std::vector<MissedPairCount> compute_missed_pairs(const std::vector<DrillResult>& results, size_t limit);

// Session count, average failure_event count per session, and how many
// sessions started within 7 days of `now_iso8601`.
SessionStats compute_session_stats(const std::vector<Session>& sessions,
                                    const std::vector<FailureEvent>& events, const std::string& now_iso8601);

// One LeechTrendEntry per distinct subject_id present in `history`,
// sorted descending by |delta| (the biggest swings, worsening or
// improving, first). `history` is every stat_snapshot row for whichever
// subjects the caller cares about -- main.cpp builds it by calling
// Store::stat_snapshots_for once per current leech and concatenating, so
// this scores leeches, not all ~9000 WaniKani subjects.
std::vector<LeechTrendEntry> compute_leech_trend(const std::vector<ReviewStat>& history);

// Renders the `wkr stats` terminal output: a drill-history section (with
// its most-missed pairs), a session-summary section, and a leech-trend
// section -- each with an honest "nothing yet" fallback line rather than
// printing an empty/misleading section. `use_color`, when true, wraps
// each section heading in cyan, matching print_report's convention.
// Defaults to false so it's testable via std::ostringstream without
// passing anything extra.
void print_stats(const DrillStats& drill, const std::vector<MissedPairCount>& missed_pairs,
                  const SessionStats& sessions, const std::vector<LeechTrendEntry>& trend,
                  const std::vector<Subject>& all_subjects, std::ostream& out, bool use_color = false);
