#pragma once

#include <optional>
#include <string>
#include <vector>

#include "model.h"

constexpr int kSrsBurnedStage = 9;

PairSrsState advance_pair_srs(const std::optional<PairSrsState>& current, long long subject_id,
                              long long other_id, bool correct, const std::string& now_iso8601);

std::vector<ConfusionPair> select_due_pairs(const std::vector<ConfusionPair>& pairs,
                                            const std::vector<PairSrsState>& states,
                                            const std::string& now_iso8601);

std::optional<std::string> next_due_at(const std::vector<ConfusionPair>& pairs,
                                       const std::vector<PairSrsState>& states);

std::string format_wait(const std::string& due_at, const std::string& now_iso8601);

std::string describe_pair_srs(const std::optional<PairSrsState>& state,
                              const std::string& now_iso8601);
