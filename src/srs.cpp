#include "srs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <utility>

namespace {

constexpr std::array<std::chrono::hours, kSrsBurnedStage> kStageIntervals = {
    std::chrono::hours(0),   std::chrono::hours(4),    std::chrono::hours(8),
    std::chrono::hours(24),  std::chrono::hours(48),   std::chrono::hours(24 * 7),
    std::chrono::hours(24 * 14), std::chrono::hours(24 * 30), std::chrono::hours(24 * 120),
};

using PairKey = std::pair<long long, long long>;

PairKey key_for(long long a, long long b) {
    return {std::min(a, b), std::max(a, b)};
}

std::map<PairKey, const PairSrsState*> index_states(const std::vector<PairSrsState>& states) {
    std::map<PairKey, const PairSrsState*> index;
    for (const auto& state : states) {
        index[key_for(state.a_id, state.b_id)] = &state;
    }
    return index;
}

bool is_due(const PairSrsState& state, const std::string& now_iso8601) {
    return state.stage != kSrsBurnedStage && state.due_at <= now_iso8601;
}

}  // namespace

PairSrsState advance_pair_srs(const std::optional<PairSrsState>& current, long long subject_id,
                              long long other_id, bool correct, const std::string& now_iso8601) {
    const int current_stage = current.has_value() ? current->stage : 0;
    const int next_stage =
        correct ? std::min(current_stage + 1, kSrsBurnedStage) : std::max(1, current_stage - 2);

    PairSrsState next;
    next.a_id = std::min(subject_id, other_id);
    next.b_id = std::max(subject_id, other_id);
    next.stage = next_stage;
    next.last_reviewed_at = now_iso8601;

    if (next_stage != kSrsBurnedStage) {
        const auto now = parse_wk_timestamp(now_iso8601);
        if (now.has_value()) {
            next.due_at = format_wk_timestamp(*now + kStageIntervals[next_stage]);
        }
    }
    return next;
}

std::vector<ConfusionPair> select_due_pairs(const std::vector<ConfusionPair>& pairs,
                                            const std::vector<PairSrsState>& states,
                                            const std::string& now_iso8601) {
    const auto index = index_states(states);

    std::vector<ConfusionPair> due;
    for (const auto& pair : pairs) {
        const auto it = index.find(key_for(pair.a_id, pair.b_id));
        if (it == index.end() || is_due(*it->second, now_iso8601)) {
            due.push_back(pair);
        }
    }
    return due;
}

std::optional<std::string> next_due_at(const std::vector<ConfusionPair>& pairs,
                                       const std::vector<PairSrsState>& states) {
    const auto index = index_states(states);

    std::optional<std::string> earliest;
    for (const auto& pair : pairs) {
        const auto it = index.find(key_for(pair.a_id, pair.b_id));
        if (it == index.end() || it->second->stage == kSrsBurnedStage) {
            continue;
        }
        if (!earliest.has_value() || it->second->due_at < *earliest) {
            earliest = it->second->due_at;
        }
    }
    return earliest;
}

std::string format_wait(const std::string& due_at, const std::string& now_iso8601) {
    const auto due = parse_wk_timestamp(due_at);
    const auto now = parse_wk_timestamp(now_iso8601);
    if (!due.has_value() || !now.has_value()) {
        return "unknown";
    }

    const auto total_minutes =
        std::chrono::duration_cast<std::chrono::minutes>(*due - *now).count();
    if (total_minutes < 1) {
        return "<1m";
    }

    const long long days = total_minutes / (24 * 60);
    const long long hours = (total_minutes / 60) % 24;
    const long long minutes = total_minutes % 60;

    const std::array<std::pair<long long, const char*>, 3> units = {
        {{days, "d"}, {hours, "h"}, {minutes, "m"}}};

    std::string out;
    int shown = 0;
    for (const auto& [value, suffix] : units) {
        if (value == 0 && shown == 0) {
            continue;
        }
        if (value == 0) {
            break;
        }
        if (shown == 2) {
            break;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += std::to_string(value) + suffix;
        ++shown;
    }
    return out;
}

std::string describe_pair_srs(const std::optional<PairSrsState>& state,
                              const std::string& now_iso8601) {
    if (!state.has_value() || state->stage == 0) {
        return "new";
    }
    if (state->stage == kSrsBurnedStage) {
        return "burned";
    }
    const std::string prefix = "stage " + std::to_string(state->stage) + ", ";
    if (state->due_at <= now_iso8601) {
        return prefix + "due now";
    }
    return prefix + "due in " + format_wait(state->due_at, now_iso8601);
}

std::string nothing_due_message(const std::vector<ConfusionPair>& pairs,
                                const std::vector<PairSrsState>& states,
                                const std::string& now_iso8601) {
    const std::optional<std::string> next = next_due_at(pairs, states);
    if (!next.has_value()) {
        return "Nothing due -- every confusion pair is burned.";
    }
    return "Nothing due. Next review in " + format_wait(*next, now_iso8601) + ".";
}
