#pragma once

#include <ostream>
#include <vector>

#include "model.h"
#include "wk_api.h"

// How many leeches (top of the descending leech_score ranking) get a
// block printed. Named per the brief rather than left as a magic 10.
constexpr int kReportTopN = 10;

// Renders the `wkr report` terminal output: one block per leech (top
// kReportTopN, descending leech_score), each showing its meaning/reading
// stat line, its most relevant confusion pair(s), and a generated focus
// tip. Takes an ostream (not std::cout directly) so it's testable
// against a std::ostringstream.
//
// `latest_stats` supplies percentage_correct for the LEECH line;
// LeechEntry itself only carries the derived score, not the raw stat
// row, so this is an explicit addition beyond the brief's signature
// sketch (which also omits it) for the same reason
// compute_confusion_pairs needed `leeches` added — the data has to come
// from somewhere, and Store is the only place that has it.
//
// `use_color`, when true, wraps the LEECH label/characters (bold red) and
// the "Focus:" header (cyan) in raw ANSI escape codes; the surrounding
// text and structure are unchanged either way, so callers doing substring
// checks on the plain-text parts are unaffected. Defaults to false so
// every existing call site (in particular, unit tests asserting on exact
// output) keeps behaving exactly as before without needing to pass
// anything; main.cpp is the only caller expected to pass true, gated on
// an isatty() check of the real terminal.
void print_report(const std::vector<ConfusionPair>& pairs, const std::vector<LeechEntry>& leeches,
                   const std::vector<Subject>& all_subjects,
                   const std::vector<Assignment>& all_assignments,
                   const std::vector<wk_api::StudyMaterial>& all_study_materials,
                   const std::vector<ReviewStat>& latest_stats, std::ostream& out,
                   bool use_color = false);
