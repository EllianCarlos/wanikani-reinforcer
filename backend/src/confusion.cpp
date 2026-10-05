#include "confusion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <ratio>
#include <set>
#include <utility>

namespace {

using IdPair = std::pair<long long, long long>;

IdPair normalize_pair(long long a, long long b) { return a < b ? IdPair{a, b} : IdPair{b, a}; }

// True when an edge of `kind` matches a failure combination where
// `has_reading_failure`/`has_meaning_failure` summarize every failure
// kind observed (on either subject in the pair) within the session: a
// Reading edge matches a reading failure, WkVisual/Component/CharShape/
// Meaning edges match a meaning failure. A FailureKind::Both event sets
// both flags, so it naturally matches any edge kind per the brief.
bool kind_matches(EdgeKind kind, bool has_reading_failure, bool has_meaning_failure) {
    if (kind == EdgeKind::Reading) {
        return has_reading_failure;
    }
    return has_meaning_failure;
}

constexpr double kMatchBonus = 1.5;
constexpr double kMismatchBonus = 0.6;
constexpr double kLikelyDiscount = 0.5;
constexpr double kDecayDays = 30.0;

}  // namespace

std::vector<LeechEntry> compute_leech_scores(const std::vector<ReviewStat>& latest_stats) {
    std::vector<LeechEntry> result;
    for (const auto& stat : latest_stats) {
        const int total =
            stat.meaning_correct + stat.meaning_incorrect + stat.reading_correct + stat.reading_incorrect;
        if (total < kMinReviewsForLeech) {
            continue;
        }

        const double meaning_score =
            static_cast<double>(stat.meaning_incorrect) / std::pow(stat.meaning_current_streak + 1, 1.5);
        const double reading_score =
            static_cast<double>(stat.reading_incorrect) / std::pow(stat.reading_current_streak + 1, 1.5);

        LeechEntry entry;
        entry.subject_id = stat.subject_id;
        if (meaning_score >= reading_score) {
            entry.leech_score = meaning_score;
            entry.is_meaning = true;
        } else {
            entry.leech_score = reading_score;
            entry.is_meaning = false;
        }
        // A subject can clear the minimum-review bar with zero incorrect
        // answers on both axes, which scores 0.0 on both — it has never
        // been failed, so it is not a leech at all. Without this gate it
        // would still be reported (print_report prints the top N
        // unconditionally) as e.g. "You fail the MEANING, not the
        // reading" for something the user has never once got wrong.
        if (entry.leech_score <= 0.0) {
            continue;
        }
        result.push_back(entry);
    }

    std::sort(result.begin(), result.end(),
              [](const LeechEntry& a, const LeechEntry& b) { return a.leech_score > b.leech_score; });
    return result;
}

std::vector<ConfusionPair> compute_confusion_pairs(const std::vector<Session>& sessions,
                                                     const std::vector<FailureEvent>& events,
                                                     const std::vector<SimilarityEdge>& edges,
                                                     const std::vector<LeechEntry>& leeches,
                                                     const std::string& now_iso8601) {
    const auto now = parse_wk_timestamp(now_iso8601);

    // Index edges by normalized (a_id, b_id) so every edge kind between a
    // pair can be visited without an O(edges) scan per pair.
    std::map<IdPair, std::vector<SimilarityEdge>> edges_by_pair;
    for (const auto& edge : edges) {
        edges_by_pair[normalize_pair(edge.a_id, edge.b_id)].push_back(edge);
    }

    // session_id -> subject_id -> every FailureKind observed for that
    // subject within that session.
    std::map<long long, std::map<long long, std::vector<FailureKind>>> failures_by_session;
    for (const auto& event : events) {
        if (event.session_id < 0) {
            continue;  // not yet grouped into a session; nothing to pair it with
        }
        failures_by_session[event.session_id][event.subject_id].push_back(event.kind);
    }

    std::map<long long, const Session*> session_by_id;
    for (const auto& session : sessions) {
        session_by_id[session.id] = &session;
    }

    struct Accum {
        double score = 0.0;
        EdgeKind dominant_kind = EdgeKind::WkVisual;
        double dominant_term = -1.0;
    };
    std::map<IdPair, Accum> accum;

    for (const auto& [session_id, subject_kinds] : failures_by_session) {
        const auto it = session_by_id.find(session_id);
        if (it == session_by_id.end()) {
            continue;  // orphaned session_id; shouldn't happen but don't crash on it
        }
        const auto session_time = parse_wk_timestamp(it->second->started_at);
        if (!now.has_value() || !session_time.has_value()) {
            continue;  // can't compute a decay term without both timestamps
        }
        const double days_ago =
            std::chrono::duration<double, std::ratio<86400>>(*now - *session_time).count();
        const double decay = std::exp(-days_ago / kDecayDays);

        std::vector<long long> subject_ids;
        subject_ids.reserve(subject_kinds.size());
        for (const auto& entry : subject_kinds) {
            subject_ids.push_back(entry.first);
        }
        std::sort(subject_ids.begin(), subject_ids.end());

        for (size_t x = 0; x < subject_ids.size(); ++x) {
            for (size_t y = x + 1; y < subject_ids.size(); ++y) {
                const IdPair key{subject_ids[x], subject_ids[y]};
                const auto pair_edges_it = edges_by_pair.find(key);
                if (pair_edges_it == edges_by_pair.end()) {
                    continue;
                }

                bool has_reading_failure = false;
                bool has_meaning_failure = false;
                for (auto id : {key.first, key.second}) {
                    for (auto kind : subject_kinds.at(id)) {
                        has_reading_failure |= (kind == FailureKind::Reading || kind == FailureKind::Both);
                        has_meaning_failure |= (kind == FailureKind::Meaning || kind == FailureKind::Both);
                    }
                }

                for (const auto& edge : pair_edges_it->second) {
                    const double bonus = kind_matches(edge.kind, has_reading_failure, has_meaning_failure)
                                              ? kMatchBonus
                                              : kMismatchBonus;
                    const double term = edge.weight * bonus * decay;

                    Accum& a = accum[key];
                    a.score += term;
                    if (term > a.dominant_term) {
                        a.dominant_term = term;
                        a.dominant_kind = edge.kind;
                    }
                }
            }
        }
    }

    std::vector<ConfusionPair> result;
    std::set<long long> subjects_in_co_failure;
    for (const auto& [key, a] : accum) {
        result.push_back(ConfusionPair{key.first, key.second, a.score, a.dominant_kind, false});
        subjects_in_co_failure.insert(key.first);
        subjects_in_co_failure.insert(key.second);
    }

    // Single-sided "likely" candidates: leeches with no co-failure pair
    // at all, paired with their single strongest edge (any kind).
    for (const auto& leech : leeches) {
        if (subjects_in_co_failure.count(leech.subject_id) > 0) {
            continue;
        }

        const SimilarityEdge* best = nullptr;
        for (const auto& edge : edges) {
            if (edge.a_id != leech.subject_id && edge.b_id != leech.subject_id) {
                continue;
            }
            if (best == nullptr || edge.weight > best->weight) {
                best = &edge;
            }
        }
        if (best == nullptr) {
            continue;  // no edges at all: nothing to report
        }

        const long long neighbor_id = (best->a_id == leech.subject_id) ? best->b_id : best->a_id;
        result.push_back(ConfusionPair{leech.subject_id, neighbor_id, best->weight * kLikelyDiscount,
                                        best->kind, true});
    }

    std::sort(result.begin(), result.end(),
              [](const ConfusionPair& a, const ConfusionPair& b) { return a.score > b.score; });
    return result;
}
