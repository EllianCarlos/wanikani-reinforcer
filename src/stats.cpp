#include "stats.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace {

// Radicals can be image-only (empty `characters`); fall back to the slug
// wherever a subject needs to be displayed. Mirrors the same small helper
// duplicated in report.cpp/drill.cpp/advice.cpp -- kept as a per-file
// anonymous-namespace helper rather than shared via model.h, consistent
// with how the rest of this codebase does it.
std::string display_name(const Subject& subject) {
    return subject.characters.empty() ? subject.slug : subject.characters;
}

// Raw ANSI escape codes -- no ncurses/terminal-capability library, per
// the plan. Mirrors the same small helper duplicated in report.cpp/
// drill.cpp.
constexpr const char* kColorReset = "\033[0m";
constexpr const char* kColorCyan = "\033[36m";

std::string colorize(const std::string& text, const char* code, bool use_color) {
    if (!use_color || text.empty()) {
        return text;
    }
    return std::string(code) + text + kColorReset;
}

}  // namespace

DrillStats compute_drill_stats(std::vector<DrillResult> results) {
    std::sort(results.begin(), results.end(),
              [](const DrillResult& a, const DrillResult& b) { return a.answered_at < b.answered_at; });

    DrillStats stats;
    stats.total = static_cast<int>(results.size());
    for (const auto& r : results) {
        if (r.correct) {
            ++stats.correct;
        }
    }

    for (auto it = results.rbegin(); it != results.rend(); ++it) {
        if (!it->correct) {
            break;
        }
        ++stats.current_streak;
    }
    return stats;
}

std::vector<MissedPairCount> compute_missed_pairs(const std::vector<DrillResult>& results, size_t limit) {
    std::map<std::pair<long long, long long>, int> misses;
    std::vector<std::pair<long long, long long>> first_seen_order;
    for (const auto& r : results) {
        if (r.correct) {
            continue;
        }
        const auto key = std::make_pair(r.subject_id, r.distractor_id);
        if (misses.find(key) == misses.end()) {
            first_seen_order.push_back(key);
        }
        ++misses[key];
    }

    std::vector<MissedPairCount> result;
    result.reserve(first_seen_order.size());
    for (const auto& key : first_seen_order) {
        result.push_back(MissedPairCount{key.first, key.second, misses.at(key)});
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const MissedPairCount& a, const MissedPairCount& b) { return a.miss_count > b.miss_count; });
    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

SessionStats compute_session_stats(const std::vector<Session>& sessions,
                                    const std::vector<FailureEvent>& events, const std::string& now_iso8601) {
    SessionStats stats;
    stats.session_count = static_cast<int>(sessions.size());
    if (sessions.empty()) {
        return stats;
    }

    std::map<long long, int> failures_per_session;
    for (const auto& event : events) {
        if (event.session_id >= 0) {
            ++failures_per_session[event.session_id];
        }
    }
    int total_failures = 0;
    for (const auto& [session_id, count] : failures_per_session) {
        total_failures += count;
    }
    stats.avg_failures_per_session = static_cast<double>(total_failures) / static_cast<double>(sessions.size());

    const auto now = parse_wk_timestamp(now_iso8601);
    if (now.has_value()) {
        for (const auto& session : sessions) {
            const auto started = parse_wk_timestamp(session.started_at);
            if (started.has_value() && *started <= *now && (*now - *started) <= std::chrono::hours(24 * 7)) {
                ++stats.sessions_last_7_days;
            }
        }
    }
    return stats;
}

std::vector<LeechTrendEntry> compute_leech_trend(const std::vector<ReviewStat>& history) {
    // Group by subject_id preserving each subject's own encounter order
    // (Store::stat_snapshots_for already returns each subject's rows
    // chronologically, and main.cpp concatenates per-subject, so the
    // first/last element of each group below stays chronologically
    // correct without a second sort).
    std::map<long long, std::vector<const ReviewStat*>> by_subject;
    for (const auto& stat : history) {
        by_subject[stat.subject_id].push_back(&stat);
    }

    std::vector<LeechTrendEntry> result;
    for (const auto& [subject_id, stats] : by_subject) {
        LeechTrendEntry entry;
        entry.subject_id = subject_id;
        entry.snapshot_count = static_cast<int>(stats.size());
        entry.first_percentage = stats.front()->percentage_correct;
        entry.last_percentage = stats.back()->percentage_correct;
        entry.delta = entry.last_percentage - entry.first_percentage;
        result.push_back(entry);
    }

    std::sort(result.begin(), result.end(), [](const LeechTrendEntry& a, const LeechTrendEntry& b) {
        return std::abs(a.delta) > std::abs(b.delta);
    });
    return result;
}

void print_stats(const DrillStats& drill, const std::vector<MissedPairCount>& missed_pairs,
                  const SessionStats& sessions, const std::vector<LeechTrendEntry>& trend,
                  const std::vector<Subject>& all_subjects, std::ostream& out, bool use_color) {
    std::map<long long, const Subject*> subject_by_id;
    for (const auto& s : all_subjects) {
        subject_by_id[s.id] = &s;
    }
    auto name = [&](long long id) -> std::string {
        const auto it = subject_by_id.find(id);
        return it != subject_by_id.end() ? display_name(*it->second) : ("#" + std::to_string(id));
    };

    out << colorize("Drill history", kColorCyan, use_color) << "\n";
    if (drill.total == 0) {
        out << "  No drills recorded yet -- run 'wkr drill'.\n";
    } else {
        const double accuracy = 100.0 * static_cast<double>(drill.correct) / static_cast<double>(drill.total);
        out << "  " << drill.correct << "/" << drill.total << " correct (" << std::fixed
            << std::setprecision(1) << accuracy << "%), current streak " << drill.current_streak << "\n";
    }
    if (!missed_pairs.empty()) {
        out << "  Most-missed pairs:\n";
        for (const auto& p : missed_pairs) {
            out << "    " << name(p.subject_id) << " / " << name(p.distractor_id) << "  missed "
                << p.miss_count << "x\n";
        }
    }
    out << "\n";

    out << colorize("Sessions", kColorCyan, use_color) << "\n";
    if (sessions.session_count == 0) {
        out << "  No sessions recorded yet -- run 'wkr sync'.\n";
    } else {
        out << "  " << sessions.session_count << " sessions, " << std::fixed << std::setprecision(1)
            << sessions.avg_failures_per_session << " failures/session on average, "
            << sessions.sessions_last_7_days << " in the last 7 days\n";
    }
    out << "\n";

    out << colorize("Leech trend", kColorCyan, use_color) << "\n";
    bool any_trend_shown = false;
    for (const auto& t : trend) {
        if (t.snapshot_count < 2) {
            continue;  // not enough sync history to say anything -- see stats.h
        }
        any_trend_shown = true;
        out << "  " << name(t.subject_id) << "  " << t.first_percentage << "% -> " << t.last_percentage
            << "%  (" << (t.delta >= 0 ? "+" : "") << t.delta << ")\n";
    }
    if (!any_trend_shown) {
        out << "  Not enough sync history yet -- run 'wkr sync' again on a different day.\n";
    }
}
