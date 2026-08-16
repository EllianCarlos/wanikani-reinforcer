#include "drill.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "advice.h"

// NOTE ON THE "SENDS NOTHING TO WANIKANI" GUARANTEE: this file never
// includes/calls anything from http.h or wk_api.h's fetch_* functions.
// The only I/O here is std::istream/std::ostream (both injected by the
// caller) and Store's local-SQLite read/write methods. Grep this file
// for "http" or "fetch_" to confirm -- there are none.

namespace {

// Raw ANSI escape codes -- no ncurses/terminal-capability library, per
// the plan. Kept as file-local constants/helper (duplicated in
// report.cpp) rather than shared via a new header, consistent with how
// the rest of this codebase keeps small per-file helpers in anonymous
// namespaces (see display_name's comment just below for the same
// pattern).
constexpr const char* kColorReset = "\033[0m";
constexpr const char* kColorGreen = "\033[32m";
constexpr const char* kColorRed = "\033[31m";

// Wraps `text` in `code`...reset when use_color is true; returns `text`
// unchanged otherwise (and always for an empty string). Because this only
// ever wraps a whole segment rather than splicing into the middle of one,
// a substring check against the plain text inside `text` still finds it
// either way.
std::string colorize(const std::string& text, const char* code, bool use_color) {
    if (!use_color || text.empty()) {
        return text;
    }
    return std::string(code) + text + kColorReset;
}

// Radicals can be image-only (empty `characters`); fall back to the slug
// wherever a subject needs to be displayed. Mirrors the same small
// helper duplicated in report.cpp/advice.cpp -- kept as a per-file
// anonymous-namespace helper rather than shared via model.h, consistent
// with how the rest of this codebase does it.
std::string display_name(const Subject& subject) {
    return subject.characters.empty() ? subject.slug : subject.characters;
}

// The reading-axis counterpart of model.h's primary_meaning: the first
// Reading with primary == true, or the first reading if none is marked
// primary. Returns nullptr if the subject has no readings at all (e.g.
// radicals).
const Reading* primary_reading(const Subject& subject) {
    if (subject.readings.empty()) {
        return nullptr;
    }
    for (const auto& r : subject.readings) {
        if (r.primary) {
            return &r;
        }
    }
    return &subject.readings.front();
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// Current UTC time in WaniKani-style ISO-8601 ("2026-08-16T03:14:07Z").
// Mirrors the same small helper duplicated in store.cpp and main.cpp --
// each stays local rather than sharing one via model.h, consistent with
// the rest of this codebase.
std::string now_iso8601() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buf.data());
}

std::map<long long, const Subject*> index_subjects(const std::vector<Subject>& all_subjects) {
    std::map<long long, const Subject*> by_id;
    for (const auto& s : all_subjects) {
        by_id[s.id] = &s;
    }
    return by_id;
}

std::map<long long, const wk_api::StudyMaterial*> index_study_materials(
    const std::vector<wk_api::StudyMaterial>& materials) {
    std::map<long long, const wk_api::StudyMaterial*> by_id;
    for (const auto& m : materials) {
        by_id[m.subject_id] = &m;
    }
    return by_id;
}

// Finds a third multiple-choice option for a forced-choice question:
// a subject connected to `a_id` or `b_id` by any SimilarityEdge (read via
// store.edges_for, the only read the drill does beyond the data the
// caller already loaded), excluding `a_id`/`b_id` themselves. Candidates
// are collected into a std::set so the smallest subject id wins --
// deterministic regardless of edges_for's underlying row order, which
// keeps this testable without relying on SQLite iteration order.
//
// Falls back to any other subject at `level` (any WaniKani level, taken
// from the subject being quizzed) if no similarity-graph candidate
// exists, per the brief: "don't block the question on a missing third
// option". Returns nullptr only if neither source turns up anything
// (e.g. a two-subject fixture in tests) -- callers must handle that by
// falling back to a 2-option question.
const Subject* find_third_option(long long a_id, long long b_id, int level,
                                  const std::vector<Subject>& all_subjects,
                                  const std::map<long long, const Subject*>& subject_by_id,
                                  Store& store) {
    std::set<long long> candidates;
    for (const auto& edge : store.edges_for(a_id)) {
        const long long other = (edge.a_id == a_id) ? edge.b_id : edge.a_id;
        if (other != a_id && other != b_id) {
            candidates.insert(other);
        }
    }
    for (const auto& edge : store.edges_for(b_id)) {
        const long long other = (edge.a_id == b_id) ? edge.b_id : edge.a_id;
        if (other != a_id && other != b_id) {
            candidates.insert(other);
        }
    }
    for (long long id : candidates) {
        const auto it = subject_by_id.find(id);
        if (it != subject_by_id.end()) {
            return it->second;
        }
    }

    for (const auto& s : all_subjects) {
        if (s.id != a_id && s.id != b_id && s.level == level) {
            return &s;
        }
    }
    return nullptr;
}

// The first line of generate_advice()'s output (its rule 1, the
// failure-type line, always fires and is always first -- see advice.h),
// used for the "one line of advice" the brief asks for after a wrong
// answer.
std::string first_advice_line(const Subject& failed_subject, const Subject& neighbor_subject,
                               EdgeKind edge_kind, FailureKind failure_kind,
                               const std::optional<wk_api::StudyMaterial>& study_material,
                               const std::vector<Subject>& all_subjects) {
    const std::string advice =
        generate_advice(failed_subject, neighbor_subject, edge_kind, failure_kind, study_material, all_subjects);
    const size_t newline = advice.find('\n');
    return newline == std::string::npos ? advice : advice.substr(0, newline);
}

// Joins the display names of `options` with " / ", e.g. "太 / 犬 / 大".
std::string join_options(const std::vector<const Subject*>& options) {
    std::ostringstream out;
    for (size_t i = 0; i < options.size(); ++i) {
        if (i > 0) {
            out << " / ";
        }
        out << display_name(*options[i]);
    }
    return out.str();
}

// Parses the user's forced-choice answer against `options` (already
// rotated into display order): accepts either a 1-based index or the
// option's own display text, trimmed and case-insensitive. Returns -1 if
// nothing matches (treated as a wrong answer, not an error).
int parse_choice(const std::string& raw_input, const std::vector<const Subject*>& options) {
    const std::string trimmed = trim(raw_input);
    if (trimmed.empty()) {
        return -1;
    }

    bool all_digits = true;
    for (char c : trimmed) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        try {
            const int index = std::stoi(trimmed) - 1;
            if (index >= 0 && static_cast<size_t>(index) < options.size()) {
                return index;
            }
        } catch (const std::exception&) {
            // Overflowed int (e.g. a very long digit string) -- treat as
            // an invalid/wrong answer rather than crashing.
        }
        return -1;
    }

    const std::string lowered = lower(trimmed);
    for (size_t i = 0; i < options.size(); ++i) {
        if (lower(display_name(*options[i])) == lowered) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Runs one forced-choice question about `pair`, quizzing on `a_id` (the
// "correct" subject -- its meaning or reading is what's shown) with
// `b_id` as the confusable pair member that also appears as an option.
// `ask_reading` selects meaning vs reading; falls back to meaning if the
// quizzed subject has no readings (radicals, or an incomplete sync).
// Returns true iff the user answered correctly. Writes exactly one
// drill_result row.
bool run_forced_choice_question(const ConfusionPair& pair, bool ask_reading, int rotate_seed,
                                 const std::vector<Subject>& all_subjects,
                                 const std::map<long long, const Subject*>& subject_by_id,
                                 const std::map<long long, const wk_api::StudyMaterial*>& study_material_by_id,
                                 Store& store, std::istream& in, std::ostream& out, bool use_color,
                                 bool& eof_hit) {
    const Subject& correct_subject = *subject_by_id.at(pair.a_id);
    const Subject& confusable_subject = *subject_by_id.at(pair.b_id);

    bool used_reading = false;
    std::string shown_text;
    if (ask_reading) {
        if (const Reading* r = primary_reading(correct_subject); r != nullptr) {
            shown_text = r->reading;
            used_reading = true;
        }
    }
    if (!used_reading) {
        const Meaning* m = primary_meaning(correct_subject);
        shown_text = (m != nullptr) ? m->meaning : display_name(correct_subject);
    }

    const Subject* third = find_third_option(pair.a_id, pair.b_id, correct_subject.level, all_subjects,
                                              subject_by_id, store);

    std::vector<const Subject*> options = {&correct_subject, &confusable_subject};
    if (third != nullptr) {
        options.push_back(third);
    }
    // Rotate the option order deterministically (by question index)
    // rather than randomly, so the correct answer isn't always printed
    // first -- while staying fully reproducible for tests. Documented in
    // drill.h.
    const size_t rotate_amount = static_cast<size_t>(rotate_seed) % options.size();
    std::rotate(options.begin(), options.begin() + static_cast<long>(rotate_amount), options.end());

    size_t correct_index = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i]->id == correct_subject.id) {
            correct_index = i;
            break;
        }
    }

    out << "Which one " << (used_reading ? "is read " : "means ") << "\"" << shown_text << "\"?  "
        << join_options(options) << "\n> ";

    std::string line;
    if (!std::getline(in, line)) {
        eof_hit = true;
        return false;
    }

    const int chosen = parse_choice(line, options);
    const bool is_correct = (chosen >= 0 && static_cast<size_t>(chosen) == correct_index);

    if (is_correct) {
        out << colorize("Correct!", kColorGreen, use_color) << "\n";
    } else {
        const FailureKind failure_kind = used_reading ? FailureKind::Reading : FailureKind::Meaning;
        std::optional<wk_api::StudyMaterial> study_material;
        if (const auto it = study_material_by_id.find(correct_subject.id); it != study_material_by_id.end()) {
            study_material = *it->second;
        }
        out << colorize("Incorrect.", kColorRed, use_color) << " The correct answer was: "
            << display_name(correct_subject) << "\n";
        out << first_advice_line(correct_subject, confusable_subject, pair.dominant_kind, failure_kind,
                                  study_material, all_subjects)
            << "\n";
    }

    store.insert_drill_result({correct_subject.id, confusable_subject.id, is_correct, now_iso8601()});
    return is_correct;
}

// Runs one production question about `pair`: shows `a_id`'s characters
// and asks for its meaning, matched via matches_answer. `b_id` is always
// the distractor_id written to drill_result, per the brief. Returns true
// iff the user answered correctly.
bool run_production_question(const ConfusionPair& pair,
                              const std::map<long long, const Subject*>& subject_by_id,
                              const std::map<long long, const wk_api::StudyMaterial*>& study_material_by_id,
                              const std::vector<Subject>& all_subjects, Store& store, std::istream& in,
                              std::ostream& out, bool use_color, bool& eof_hit) {
    const Subject& quizzed_subject = *subject_by_id.at(pair.a_id);
    const Subject& confusable_subject = *subject_by_id.at(pair.b_id);

    std::optional<wk_api::StudyMaterial> study_material;
    if (const auto it = study_material_by_id.find(quizzed_subject.id); it != study_material_by_id.end()) {
        study_material = *it->second;
    }

    out << "What does \"" << display_name(quizzed_subject) << "\" mean?\n> ";

    std::string line;
    if (!std::getline(in, line)) {
        eof_hit = true;
        return false;
    }

    const bool is_correct = matches_answer(line, quizzed_subject, study_material);

    if (is_correct) {
        out << colorize("Correct!", kColorGreen, use_color) << "\n";
    } else {
        const Meaning* m = primary_meaning(quizzed_subject);
        out << colorize("Incorrect.", kColorRed, use_color) << " Accepted answer: "
            << (m != nullptr ? m->meaning : display_name(quizzed_subject)) << "\n";
        out << first_advice_line(quizzed_subject, confusable_subject, pair.dominant_kind, FailureKind::Meaning,
                                  study_material, all_subjects)
            << "\n";
    }

    store.insert_drill_result({quizzed_subject.id, confusable_subject.id, is_correct, now_iso8601()});
    return is_correct;
}

}  // namespace

int levenshtein_distance(const std::string& a, const std::string& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    std::vector<int> prev(m + 1);
    std::vector<int> curr(m + 1);
    for (size_t j = 0; j <= m; ++j) {
        prev[j] = static_cast<int>(j);
    }
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

bool matches_answer(const std::string& input, const Subject& subject,
                     const std::optional<wk_api::StudyMaterial>& study_material) {
    const std::string trimmed_input = lower(trim(input));
    if (trimmed_input.empty()) {
        return false;
    }

    std::vector<std::string> candidates;
    for (const auto& m : subject.meanings) {
        if (m.accepted_answer) {
            candidates.push_back(lower(trim(m.meaning)));
        }
    }
    if (study_material.has_value()) {
        for (const auto& synonym : study_material->meaning_synonyms) {
            candidates.push_back(lower(trim(synonym)));
        }
    }

    for (const auto& candidate : candidates) {
        if (candidate == trimmed_input) {
            return true;
        }
    }

    // Typo tolerance (matching WaniKani's own behavior): only kicks in
    // for longer typed answers, gated on the *input's* length so short
    // words (e.g. "cat"/"car") can't accidentally fuzzy-match each other.
    if (trimmed_input.size() > 4) {
        for (const auto& candidate : candidates) {
            if (levenshtein_distance(candidate, trimmed_input) <= 1) {
                return true;
            }
        }
    }

    return false;
}

void run_drill(const std::vector<ConfusionPair>& pairs, const std::vector<Subject>& all_subjects,
                const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                int question_count, std::istream& in, std::ostream& out, bool use_color) {
    if (pairs.empty() || question_count <= 0) {
        out << "Nothing to drill yet -- run 'wkr report' first to build up confusion pairs.\n";
        return;
    }

    const std::map<long long, const Subject*> subject_by_id = index_subjects(all_subjects);
    const std::map<long long, const wk_api::StudyMaterial*> study_material_by_id =
        index_study_materials(study_materials);

    const size_t total = std::min(static_cast<size_t>(question_count), pairs.size());

    int asked = 0;
    int correct_count = 0;
    int forced_choice_seen = 0;

    for (size_t i = 0; i < total; ++i) {
        const ConfusionPair& pair = pairs[i];
        if (subject_by_id.find(pair.a_id) == subject_by_id.end() ||
            subject_by_id.find(pair.b_id) == subject_by_id.end()) {
            continue;  // subject not synced (shouldn't happen); skip rather than crash
        }

        // Alternate question type by overall question index: forced-
        // choice, production, forced-choice, ... (see drill.h).
        const bool is_forced_choice = (i % 2 == 0);
        bool eof_hit = false;
        bool is_correct = false;

        if (is_forced_choice) {
            // Alternate meaning/reading prompts across forced-choice
            // questions specifically (a separate counter from the
            // type-alternation index above), documented in drill.h.
            const bool ask_reading = (forced_choice_seen % 2 == 1);
            ++forced_choice_seen;
            is_correct = run_forced_choice_question(pair, ask_reading, static_cast<int>(i), all_subjects,
                                                      subject_by_id, study_material_by_id, store, in, out,
                                                      use_color, eof_hit);
        } else {
            is_correct = run_production_question(pair, subject_by_id, study_material_by_id, all_subjects,
                                                   store, in, out, use_color, eof_hit);
        }

        if (eof_hit) {
            break;
        }

        ++asked;
        if (is_correct) {
            ++correct_count;
        }
    }

    out << correct_count << "/" << asked << " correct\n";
}
