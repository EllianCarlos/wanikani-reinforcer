#pragma once

#include <string>
#include <vector>

#include "model.h"

// Scores confusion pairs and leeches from failure events, sessions, and
// the similarity graph. Every function here is pure (no DB or HTTP
// access) — report.cpp loads data from Store and passes it in, which
// keeps this file testable with small hand-built fixtures.

// Below this many total reviews (meaning + reading, correct + incorrect)
// on a subject's most recent stat_snapshot, it isn't ranked as a leech —
// there isn't enough signal yet to call it one.
constexpr int kMinReviewsForLeech = 5;

// Ranks every subject with enough review history (see kMinReviewsForLeech)
// by leech_score = incorrect_total / pow(current_streak + 1, 1.5),
// computed once for the meaning axis and once for the reading axis, with
// the larger of the two reported (and is_meaning set accordingly).
// `latest_stats` must already be resolved to one ReviewStat per subject —
// its most recent stat_snapshot row (Store::latest_stat_per_subject does
// this resolution). Sorted descending by leech_score.
std::vector<LeechEntry> compute_leech_scores(const std::vector<ReviewStat>& latest_stats);

// Scores confusion pairs two ways:
//
// 1. Co-failure: for every session, every pair of subjects that both
//    failed within that session and are linked by at least one
//    similarity edge accumulates
//        edge_weight * kind_bonus * exp(-days_ago / 30)
//    per matching edge, summed across every session and every edge
//    between that pair. Emitted with likely = false.
//
// 2. Single-sided "likely" candidates: for every leech (from `leeches`)
//    that never appears in any co-failure pair above, its single
//    strongest-weighted edge (of any kind) in `edges` is emitted as a
//    ConfusionPair with score = edge_weight * 0.5 and likely = true —
//    a discounted guess rather than an observation, and what makes the
//    very first `wkr report` useful before enough sessions exist for
//    real co-failure data. A leech with no edges at all is skipped.
//
// `events` must include every failure event with session_id already
// set (Store::all_failure_events, not the without-session variant).
// `now_iso8601` drives the decay term; it is passed in explicitly
// rather than read from the system clock so this stays deterministic
// and testable.
//
// The brief's sketch of this function's signature omits `leeches`, but
// the single-sided step it describes is defined in terms of "every
// leech" — there is no way to know which subjects are leeches without
// it, so it's an explicit parameter here rather than recomputed
// internally (which would also break the "pure function, caller loads
// from Store" contract this file follows throughout).
//
// Returns the combined co-failure + likely list sorted by score
// descending; likely pairs are not forced below co-failure pairs, so a
// strong likely candidate can outrank a stale, weak co-failure one.
std::vector<ConfusionPair> compute_confusion_pairs(const std::vector<Session>& sessions,
                                                     const std::vector<FailureEvent>& events,
                                                     const std::vector<SimilarityEdge>& edges,
                                                     const std::vector<LeechEntry>& leeches,
                                                     const std::string& now_iso8601);
