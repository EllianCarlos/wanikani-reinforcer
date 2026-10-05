#include "burned.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "drill.h"
#include "srs.h"

namespace {

constexpr const char* kColorReset = "\033[0m";
constexpr const char* kColorGreen = "\033[32m";
constexpr const char* kColorRed = "\033[31m";

constexpr std::array<std::chrono::hours, kMaxBurnedStage + 1> kStageIntervals = {
    std::chrono::hours(24),      std::chrono::hours(24 * 30),  std::chrono::hours(24 * 60),
    std::chrono::hours(24 * 120), std::chrono::hours(24 * 240),
};

std::string colorize(const std::string& text, const char* code, bool use_color) {
    if (!use_color || text.empty()) {
        return text;
    }
    return std::string(code) + text + kColorReset;
}

std::string display_name(const Subject& subject) {
    return subject.characters.empty() ? subject.slug : subject.characters;
}

std::string now_or_clock(const std::string& now_iso8601) {
    if (!now_iso8601.empty()) {
        return now_iso8601;
    }
    return format_wk_timestamp(std::chrono::system_clock::now());
}

std::set<long long> burned_subject_ids(const std::vector<Assignment>& assignments) {
    std::set<long long> ids;
    for (const auto& assignment : assignments) {
        if (assignment.srs_stage == kWkBurnedStage) {
            ids.insert(assignment.subject_id);
        }
    }
    return ids;
}

std::map<long long, const BurnedSrsState*> index_states(const std::vector<BurnedSrsState>& states) {
    std::map<long long, const BurnedSrsState*> index;
    for (const auto& state : states) {
        index[state.subject_id] = &state;
    }
    return index;
}

std::vector<std::string> accepted_texts(const Subject& subject,
                                        const std::optional<wk_api::StudyMaterial>& material) {
    std::vector<std::string> texts;
    for (const auto& m : subject.meanings) {
        if (m.accepted_answer) {
            texts.push_back(m.meaning);
        }
    }
    if (material.has_value()) {
        for (const auto& synonym : material->meaning_synonyms) {
            texts.push_back(synonym);
        }
    }
    return texts;
}

}  // namespace

BurnedSrsState advance_burned_srs(const std::optional<BurnedSrsState>& current, long long subject_id,
                                  bool correct, const std::string& now_iso8601) {
    const int next_stage =
        correct ? std::min((current.has_value() ? current->stage : 0) + 1, kMaxBurnedStage) : 0;

    BurnedSrsState next;
    next.subject_id = subject_id;
    next.stage = next_stage;
    next.last_reviewed_at = now_iso8601;

    const auto now = parse_wk_timestamp(now_iso8601);
    if (now.has_value()) {
        next.due_at = format_wk_timestamp(*now + kStageIntervals[next_stage]);
    }
    return next;
}

std::vector<long long> select_due_burned(const std::vector<Assignment>& assignments,
                                         const std::vector<BurnedSrsState>& states,
                                         const std::string& now_iso8601) {
    const auto index = index_states(states);

    std::vector<const BurnedSrsState*> overdue;
    std::vector<long long> unseen;
    for (long long id : burned_subject_ids(assignments)) {
        const auto it = index.find(id);
        if (it == index.end()) {
            unseen.push_back(id);
        } else if (it->second->due_at <= now_iso8601) {
            overdue.push_back(it->second);
        }
    }

    std::sort(overdue.begin(), overdue.end(), [](const BurnedSrsState* a, const BurnedSrsState* b) {
        return a->due_at != b->due_at ? a->due_at < b->due_at : a->subject_id < b->subject_id;
    });

    std::vector<long long> due;
    for (const auto* state : overdue) {
        due.push_back(state->subject_id);
    }
    due.insert(due.end(), unseen.begin(), unseen.end());
    return due;
}

std::string burned_nothing_due_message(const std::vector<Assignment>& assignments,
                                       const std::vector<BurnedSrsState>& states,
                                       const std::string& now_iso8601) {
    const std::set<long long> ids = burned_subject_ids(assignments);
    if (ids.empty()) {
        return "No burned items yet -- keep going on WaniKani.";
    }

    const auto index = index_states(states);
    std::optional<std::string> earliest;
    for (long long id : ids) {
        const auto it = index.find(id);
        if (it != index.end() && (!earliest.has_value() || it->second->due_at < *earliest)) {
            earliest = it->second->due_at;
        }
    }
    if (!earliest.has_value()) {
        return "Nothing due.";
    }
    return "Nothing due. Next burned review in " + format_wait(*earliest, now_iso8601) + ".";
}

void run_burned_review(const std::vector<long long>& subject_ids, const std::vector<Subject>& all_subjects,
                       const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                       int question_count, std::istream& in, std::ostream& out, bool use_color,
                       const std::string& now_iso8601) {
    if (subject_ids.empty() || question_count <= 0) {
        out << "Nothing to review yet.\n";
        return;
    }

    std::map<long long, const Subject*> subject_by_id;
    for (const auto& subject : all_subjects) {
        subject_by_id[subject.id] = &subject;
    }
    std::map<long long, const wk_api::StudyMaterial*> material_by_id;
    for (const auto& material : study_materials) {
        material_by_id[material.subject_id] = &material;
    }
    std::map<long long, BurnedSrsState> state_by_id;
    for (const auto& state : store.all_burned_srs()) {
        state_by_id[state.subject_id] = state;
    }

    int asked = 0;
    int correct_count = 0;
    for (long long id : subject_ids) {
        if (asked >= question_count) {
            break;
        }
        const auto subject_it = subject_by_id.find(id);
        if (subject_it == subject_by_id.end()) {
            continue;
        }
        const Subject& subject = *subject_it->second;

        std::optional<wk_api::StudyMaterial> material;
        if (const auto it = material_by_id.find(id); it != material_by_id.end()) {
            material = *it->second;
        }

        out << "What does \"" << display_name(subject) << "\" mean?\n> ";

        std::string line;
        if (!std::getline(in, line)) {
            break;
        }

        const bool is_correct = matches_answer(line, subject, material);
        if (is_correct) {
            out << colorize("Correct!", kColorGreen, use_color) << "\n";
        } else {
            const std::vector<std::string> accepted = accepted_texts(subject, material);
            std::ostringstream joined;
            for (size_t i = 0; i < accepted.size(); ++i) {
                joined << (i > 0 ? ", " : "") << accepted[i];
            }
            out << colorize("Incorrect.", kColorRed, use_color) << " Accepted answer"
                << (accepted.size() == 1 ? ": " : "s: ")
                << (accepted.empty() ? display_name(subject) : joined.str()) << "\n";
        }

        std::optional<BurnedSrsState> current;
        if (const auto it = state_by_id.find(id); it != state_by_id.end()) {
            current = it->second;
        }
        store.upsert_burned_srs(advance_burned_srs(current, id, is_correct, now_or_clock(now_iso8601)));

        ++asked;
        if (is_correct) {
            ++correct_count;
        }
    }

    out << correct_count << "/" << asked << " correct\n";
}
