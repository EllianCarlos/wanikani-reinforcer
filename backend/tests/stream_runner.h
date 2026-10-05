#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "burned.h"
#include "drill.h"

// Test-only adapters that drive DrillSession / BurnedSession from a
// scripted std::istream and render the same terminal text the old inline
// `wkr drill` / `wkr burned` commands printed. Production code speaks the
// RPC protocol instead (rpc.h); these keep the session behaviour testable
// with plain scripted input.

void run_drill(const std::vector<ConfusionPair>& pairs, const std::vector<Subject>& all_subjects,
               const std::vector<wk_api::StudyMaterial>& study_materials, Store& store, int question_count,
               std::istream& in, std::ostream& out, bool use_color = false,
               const std::string& now_iso8601 = "");

void run_burned_review(const std::vector<long long>& subject_ids, const std::vector<Subject>& all_subjects,
                       const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                       int question_count, std::istream& in, std::ostream& out, bool use_color,
                       const std::string& now_iso8601);
