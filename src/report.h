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
void print_report(const std::vector<ConfusionPair>& pairs, const std::vector<LeechEntry>& leeches,
                   const std::vector<Subject>& all_subjects,
                   const std::vector<Assignment>& all_assignments,
                   const std::vector<wk_api::StudyMaterial>& all_study_materials,
                   const std::vector<ReviewStat>& latest_stats, std::ostream& out);
