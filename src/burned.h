#pragma once

#include <iosfwd>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "model.h"
#include "store.h"
#include "wk_api.h"

constexpr int kWkBurnedStage = 9;
constexpr int kMaxBurnedStage = 4;
constexpr int kDefaultBurnedQuestionCount = 5;

BurnedSrsState advance_burned_srs(const std::optional<BurnedSrsState>& current, long long subject_id,
                                  bool correct, const std::string& now_iso8601);

bool matches_reading(const std::string& input, const Subject& subject);

bool is_unaccepted_reading(const std::string& input, const Subject& subject);

std::set<std::string> resolve_burned_types(const std::string& only, const std::string& exclude);

std::vector<long long> select_due_burned(const std::vector<Assignment>& assignments,
                                         const std::vector<BurnedSrsState>& states,
                                         const std::string& now_iso8601,
                                         const std::set<std::string>& included_types = {});

std::string burned_nothing_due_message(const std::vector<Assignment>& assignments,
                                       const std::vector<BurnedSrsState>& states,
                                       const std::string& now_iso8601,
                                       const std::set<std::string>& included_types = {});

void run_burned_review(const std::vector<long long>& subject_ids, const std::vector<Subject>& all_subjects,
                       const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                       int question_count, std::istream& in, std::ostream& out, bool use_color,
                       const std::string& now_iso8601);
