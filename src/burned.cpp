#include "burned.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "drill.h"
#include "kana.h"
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

std::set<long long> burned_subject_ids(const std::vector<Assignment>& assignments,
                                       const std::set<std::string>& included_types) {
    std::set<long long> ids;
    for (const auto& assignment : assignments) {
        const bool type_allowed =
            included_types.empty() || included_types.count(assignment.subject_type) > 0;
        if (assignment.srs_stage == kWkBurnedStage && type_allowed) {
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

std::string lower_trimmed(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = s.find_last_not_of(" \t");
    std::string out = s.substr(begin, end - begin + 1);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::set<std::string> parse_type_list(const std::string& list) {
    std::set<std::string> types;
    std::stringstream stream(list);
    std::string token;
    while (std::getline(stream, token, ',')) {
        const std::string name = lower_trimmed(token);
        if (name.empty()) {
            continue;
        }
        if (name == "radical" || name == "radicals") {
            types.insert("radical");
        } else if (name == "kanji") {
            types.insert("kanji");
        } else if (name == "vocab" || name == "vocabulary") {
            types.insert("vocabulary");
            types.insert("kana_vocabulary");
        } else {
            throw std::runtime_error("unknown subject type: " + name +
                                     " (expected radical, kanji or vocabulary)");
        }
    }
    return types;
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

std::string type_name_lower(const Subject& subject) {
    if (subject.type == "radical") {
        return "radical";
    }
    if (subject.type == "kanji") {
        return "kanji";
    }
    return "vocabulary";
}

std::string type_label(const Subject& subject) {
    std::string name = type_name_lower(subject);
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

std::vector<std::string> accepted_readings(const Subject& subject) {
    std::vector<std::string> readings;
    for (const auto& r : subject.readings) {
        if (r.accepted_answer) {
            readings.push_back(r.reading);
        }
    }
    return readings;
}

bool matches_reading(const std::string& input, const Subject& subject) {
    const std::string typed = to_hiragana(input);
    if (typed.empty()) {
        return false;
    }
    for (const auto& reading : accepted_readings(subject)) {
        if (to_hiragana(reading) == typed) {
            return true;
        }
    }
    return false;
}

bool is_unaccepted_reading(const std::string& input, const Subject& subject) {
    const std::string typed = to_hiragana(input);
    if (typed.empty()) {
        return false;
    }
    for (const auto& reading : subject.readings) {
        if (!reading.accepted_answer && to_hiragana(reading.reading) == typed) {
            return true;
        }
    }
    return false;
}

std::set<std::string> resolve_burned_types(const std::string& only, const std::string& exclude) {
    std::set<std::string> types = parse_type_list(only);
    if (types.empty()) {
        types = {"radical", "kanji", "vocabulary", "kana_vocabulary"};
    }
    for (const auto& excluded : parse_type_list(exclude)) {
        types.erase(excluded);
    }
    if (types.empty()) {
        throw std::runtime_error("no subject types left after applying --only/--exclude");
    }
    return types;
}

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
                                         const std::string& now_iso8601,
                                         const std::set<std::string>& included_types) {
    const auto index = index_states(states);

    std::vector<const BurnedSrsState*> overdue;
    std::vector<long long> unseen;
    for (long long id : burned_subject_ids(assignments, included_types)) {
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
                                       const std::string& now_iso8601,
                                       const std::set<std::string>& included_types) {
    const std::set<long long> ids = burned_subject_ids(assignments, included_types);
    if (ids.empty()) {
        return included_types.empty()
                   ? "No burned items yet -- keep going on WaniKani."
                   : "No burned items of the selected types yet -- keep going on WaniKani.";
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

        const std::string label = "[" + type_label(subject) + "] ";
        out << label << "What does \"" << display_name(subject) << "\" mean?\n> ";

        std::string line;
        if (!std::getline(in, line)) {
            break;
        }

        const bool meaning_correct = matches_answer(line, subject, material);
        if (meaning_correct) {
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

        bool reading_correct = true;
        const std::vector<std::string> readings = accepted_readings(subject);
        if (!readings.empty()) {
            bool eof = false;
            while (true) {
                out << label << "What is the reading of \"" << display_name(subject) << "\"?\n> ";
                if (!std::getline(in, line)) {
                    eof = true;
                    break;
                }
                if (!matches_reading(line, subject) && is_unaccepted_reading(line, subject)) {
                    out << "That is a valid reading, but it isn't the one WaniKani wants for this "
                        << type_name_lower(subject) << ". Try again.\n";
                    continue;
                }
                break;
            }
            if (eof) {
                break;
            }
            reading_correct = matches_reading(line, subject);
            if (reading_correct) {
                out << colorize("Correct!", kColorGreen, use_color) << "\n";
            } else {
                std::ostringstream joined;
                for (size_t i = 0; i < readings.size(); ++i) {
                    joined << (i > 0 ? ", " : "") << readings[i];
                }
                out << colorize("Incorrect.", kColorRed, use_color) << " Accepted reading"
                    << (readings.size() == 1 ? ": " : "s: ") << joined.str() << "\n";
            }
        }
        const bool is_correct = meaning_correct && reading_correct;

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
