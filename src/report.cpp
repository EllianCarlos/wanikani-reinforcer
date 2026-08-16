#include "report.h"

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>

#include "advice.h"
#include "width.h"

namespace {

// Radicals can be image-only (empty `characters`); fall back to the slug
// wherever a subject needs to be printed. Mirrors advice.cpp's helper of
// the same name/behavior — each stays a small self-contained file-local
// helper rather than sharing one via model.h, consistent with how the
// rest of this codebase keeps per-file helpers in anonymous namespaces.
std::string display_name(const Subject& subject) {
    return subject.characters.empty() ? subject.slug : subject.characters;
}

// WaniKani's standard SRS stage names. 0 covers both "Locked" (assignment
// not yet unlocked) and a brand new item never reviewed.
std::string srs_stage_name(int stage) {
    switch (stage) {
        case 1:
            return "Apprentice 1";
        case 2:
            return "Apprentice 2";
        case 3:
            return "Apprentice 3";
        case 4:
            return "Apprentice 4";
        case 5:
            return "Guru 1";
        case 6:
            return "Guru 2";
        case 7:
            return "Master";
        case 8:
            return "Enlightened";
        case 9:
            return "Burned";
        default:
            return "Locked/New";
    }
}

std::string pad_to_width(const std::string& s, int target_width) {
    const int w = display_width(s);
    if (w >= target_width) {
        return s;
    }
    return s + std::string(static_cast<size_t>(target_width - w), ' ');
}

// Primary meaning, plus up to 2 more distinct meanings, comma-separated
// (e.g. "fat, plump"). Empty if the subject has no meanings at all.
std::string meanings_summary(const Subject& subject) {
    const Meaning* primary = primary_meaning(subject);
    if (primary == nullptr) {
        return "";
    }
    std::ostringstream out;
    out << primary->meaning;
    int extra = 0;
    for (const auto& m : subject.meanings) {
        if (&m == primary) {
            continue;
        }
        if (extra >= 2) {
            break;
        }
        out << ", " << m.meaning;
        ++extra;
    }
    return out.str();
}

// Just the primary meaning (single word/phrase), for the terser
// side-by-side "look-alikes" listing.
std::string primary_meaning_text(const Subject& subject) {
    const Meaning* primary = primary_meaning(subject);
    return primary != nullptr ? primary->meaning : "";
}

// Prints up to kMaxShown neighbors from `list` (already sorted
// descending by score) as a column-aligned "chars meaning" line,
// indented 4 spaces, e.g. "    大 big    犬 dog".
void print_neighbor_list(std::ostream& out, const std::vector<const ConfusionPair*>& list,
                          long long leech_id, const std::map<long long, const Subject*>& subject_by_id) {
    constexpr size_t kMaxShown = 4;
    std::vector<const Subject*> neighbors;
    for (size_t i = 0; i < list.size() && i < kMaxShown; ++i) {
        const long long neighbor_id = (list[i]->a_id == leech_id) ? list[i]->b_id : list[i]->a_id;
        const auto it = subject_by_id.find(neighbor_id);
        if (it != subject_by_id.end()) {
            neighbors.push_back(it->second);
        }
    }
    if (neighbors.empty()) {
        return;
    }

    int max_width = 0;
    for (const auto* s : neighbors) {
        max_width = std::max(max_width, display_width(display_name(*s)));
    }

    out << "   ";
    for (size_t i = 0; i < neighbors.size(); ++i) {
        if (i > 0) {
            out << "  ";
        }
        out << " " << pad_to_width(display_name(*neighbors[i]), max_width) << " "
            << primary_meaning_text(*neighbors[i]);
    }
    out << "\n";
}

// Indents every line of a multi-line string by `prefix` and writes it to
// `out`, so generate_advice()'s output nests visually under "Focus:".
void print_indented(std::ostream& out, const std::string& text, const std::string& prefix) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        out << prefix << line << "\n";
    }
}

}  // namespace

void print_report(const std::vector<ConfusionPair>& pairs, const std::vector<LeechEntry>& leeches,
                   const std::vector<Subject>& all_subjects,
                   const std::vector<Assignment>& all_assignments,
                   const std::vector<wk_api::StudyMaterial>& all_study_materials,
                   const std::vector<ReviewStat>& latest_stats, std::ostream& out) {
    if (leeches.empty()) {
        out << "No leeches found yet. Keep reviewing and run 'wkr sync' again.\n";
        return;
    }

    std::map<long long, const Subject*> subject_by_id;
    for (const auto& s : all_subjects) {
        subject_by_id[s.id] = &s;
    }
    std::map<long long, int> srs_stage_by_subject;
    for (const auto& a : all_assignments) {
        srs_stage_by_subject[a.subject_id] = a.srs_stage;
    }
    std::map<long long, const wk_api::StudyMaterial*> study_material_by_subject;
    for (const auto& m : all_study_materials) {
        study_material_by_subject[m.subject_id] = &m;
    }
    std::map<long long, int> percentage_by_subject;
    for (const auto& stat : latest_stats) {
        percentage_by_subject[stat.subject_id] = stat.percentage_correct;
    }

    const size_t shown = std::min(leeches.size(), static_cast<size_t>(kReportTopN));

    // Column-align the LEECH line's `characters` field across every
    // block being printed this run, not just within one line.
    int max_chars_width = 0;
    for (size_t i = 0; i < shown; ++i) {
        const auto it = subject_by_id.find(leeches[i].subject_id);
        if (it != subject_by_id.end()) {
            max_chars_width = std::max(max_chars_width, display_width(display_name(*it->second)));
        }
    }

    for (size_t i = 0; i < shown; ++i) {
        const LeechEntry& leech = leeches[i];
        const auto subject_it = subject_by_id.find(leech.subject_id);
        if (subject_it == subject_by_id.end()) {
            continue;  // subject not synced (shouldn't happen); skip rather than crash
        }
        const Subject& subject = *subject_it->second;

        const int percentage =
            percentage_by_subject.count(leech.subject_id) ? percentage_by_subject.at(leech.subject_id) : 0;
        const int srs_stage =
            srs_stage_by_subject.count(leech.subject_id) ? srs_stage_by_subject.at(leech.subject_id) : 0;

        out << "LEECH  " << pad_to_width(display_name(subject), max_chars_width) << "  "
            << meanings_summary(subject) << "   " << percentage << "% correct   "
            << srs_stage_name(srs_stage) << "\n";
        out << "  You fail the " << (leech.is_meaning ? "MEANING" : "READING") << ", not the "
            << (leech.is_meaning ? "reading" : "meaning") << ".\n";

        std::vector<const ConfusionPair*> relevant;
        for (const auto& p : pairs) {
            if (p.a_id == leech.subject_id || p.b_id == leech.subject_id) {
                relevant.push_back(&p);
            }
        }
        std::vector<const ConfusionPair*> co_failure;
        std::vector<const ConfusionPair*> likely_list;
        for (const auto* p : relevant) {
            (p->likely ? likely_list : co_failure).push_back(p);
        }

        if (!co_failure.empty()) {
            out << "  Look-alikes you also failed today:\n";
            print_neighbor_list(out, co_failure, leech.subject_id, subject_by_id);
        } else if (!likely_list.empty()) {
            out << "  Likely confused with:\n";
            print_neighbor_list(out, likely_list, leech.subject_id, subject_by_id);
        }

        // `pairs` (and therefore `relevant`, built by a single forward
        // pass over it) is sorted descending by score, so the first
        // relevant entry is the top-scoring neighbor pair for this leech.
        if (!relevant.empty()) {
            const ConfusionPair* top = relevant.front();
            const long long neighbor_id = (top->a_id == leech.subject_id) ? top->b_id : top->a_id;
            const auto neighbor_it = subject_by_id.find(neighbor_id);
            if (neighbor_it != subject_by_id.end()) {
                const FailureKind failure_kind = leech.is_meaning ? FailureKind::Meaning : FailureKind::Reading;
                std::optional<wk_api::StudyMaterial> study_material;
                const auto sm_it = study_material_by_subject.find(leech.subject_id);
                if (sm_it != study_material_by_subject.end()) {
                    study_material = *sm_it->second;
                }
                const std::string advice = generate_advice(subject, *neighbor_it->second, top->dominant_kind,
                                                             failure_kind, study_material, all_subjects);
                out << "  Focus:\n";
                print_indented(out, advice, "    ");
            }
        }

        out << "\n";
    }
}
