#include "drill.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <array>
#include <cstdio>
#include <iomanip>
#include <map>
#include <stdexcept>
#include <set>
#include <sstream>
#include <vector>

#include "advice.h"
#include "srs.h"

// NOTE ON THE "SENDS NOTHING TO WANIKANI" GUARANTEE: this file never
// includes/calls anything from http.h or wk_api.h's fetch_* functions.
// The only I/O here is std::istream/std::ostream (both injected by the
// caller) and Store's local-SQLite read/write methods. Grep this file
// for "http" or "fetch_" to confirm -- there are none.

namespace {

// Human-readable EdgeKind label for the "why was this drilled" line.
// Deliberately separate from store.cpp's edge_kind_to_text (a DB
// serialization format, not prose) -- this is user-facing copy and is
// free to diverge from that column's exact wording.
std::string edge_kind_label(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::WkVisual:
            return "WaniKani flags these as visually similar";
        case EdgeKind::Component:
            return "these share a component";
        case EdgeKind::Reading:
            return "these share a reading";
        case EdgeKind::Meaning:
            return "these share a meaning";
        case EdgeKind::CharShape:
            return "these have a similar character shape";
    }
    return "these are linked in the similarity graph";  // unreachable; keeps -Wall happy
}

// One line explaining why `pair` was picked for this drill question,
// printed once per question regardless of whether the answer was right
// or wrong. `pair.likely` (see model.h's ConfusionPair) tells an observed
// co-failure apart from a single-sided similarity-graph guess.
std::string drill_reason_line(const ConfusionPair& pair) {
    std::ostringstream out;
    out << "Drilled because: " << edge_kind_label(pair.dominant_kind) << ", "
        << (pair.likely ? "a likely guess (not yet observed together)" : "you missed these together")
        << " -- score " << std::fixed << std::setprecision(2) << pair.score;
    return out.str();
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

size_t whitespace_length_at(const std::string& s, size_t pos) {
    if (std::isspace(static_cast<unsigned char>(s[pos])) != 0) {
        return 1;
    }
    if (s.compare(pos, 2, "\xC2\xA0") == 0) {
        return 2;
    }
    if (s.compare(pos, 3, "\xE3\x80\x80") == 0) {
        return 3;
    }
    return 0;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string current;
    size_t i = 0;
    while (i < s.size()) {
        const size_t skip = whitespace_length_at(s, i);
        if (skip > 0) {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
            i += skip;
        } else {
            current += s[i++];
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

std::string trim(const std::string& s) {
    const std::vector<std::string> words = split_words(s);
    std::string out;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            out += ' ';
        }
        out += words[i];
    }
    return out;
}

std::string answer_key(const std::string& s) {
    std::vector<std::string> words = split_words(lower(s));
    std::sort(words.begin(), words.end());
    std::string key;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) {
            key += ' ';
        }
        key += words[i];
    }
    return key;
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

// True when `option` legitimately also answers to `shown_text` on this
// question's axis -- i.e. picking it is not actually wrong.
//
// Forced-choice distractors are drawn from the same similarity cluster
// on purpose, and similarity.cpp gives a Reading edge its top weight
// (0.8) exactly when BOTH subjects share that reading as their primary
// reading. Those pairs sort highest in the confusion list, so they are
// the ones the drill reaches most often -- and for them the distractor
// genuinely is read the way the prompt says. Grading purely on "did you
// pick the index we designated correct" marks such an answer wrong
// despite it being right, which this predicate prevents.
//
// Readings are compared verbatim (kana; no case to fold). Meanings are
// compared case-insensitively against the option's accepted meanings
// plus its whitelisted auxiliary meanings -- the same set WaniKani
// itself would accept.
bool option_also_has_shown_text(const Subject& option, const std::string& shown_text, bool reading_axis,
                                 const std::optional<wk_api::StudyMaterial>& option_study_material) {
    const std::string wanted = trim(shown_text);
    if (wanted.empty()) {
        return false;
    }

    if (reading_axis) {
        for (const auto& r : option.readings) {
            if (trim(r.reading) == wanted) {
                return true;
            }
        }
        return false;
    }

    const std::string lowered = lower(wanted);
    for (const auto& m : option.meanings) {
        if (m.accepted_answer && lower(trim(m.meaning)) == lowered) {
            return true;
        }
    }
    for (const auto& am : option.auxiliary_meanings) {
        if (am.type == "whitelist" && lower(trim(am.meaning)) == lowered) {
            return true;
        }
    }
    // A custom meaning synonym on the option's own StudyMaterial is just
    // as legitimate an answer as its accepted_answer meanings -- this is
    // the same set matches_answer already honors for production
    // questions; forced-choice grading was missing it (see the
    // "StudyMaterial synonym" regression test in test_drill.cpp).
    if (option_study_material.has_value()) {
        for (const auto& synonym : option_study_material->meaning_synonyms) {
            if (lower(trim(synonym)) == lowered) {
                return true;
            }
        }
    }
    return false;
}

// Every text that would grade a production answer correct for `subject`:
// its accepted-answer meanings (primary and non-primary) followed by
// every StudyMaterial synonym, in that order. Feeds the wrong-answer
// feedback in DrillSession::answer, so a user can see everything that
// would have counted -- including their own custom synonyms.
std::vector<std::string> accepted_answer_texts(const Subject& subject,
                                                const std::optional<wk_api::StudyMaterial>& study_material) {
    std::vector<std::string> texts;
    for (const auto& m : subject.meanings) {
        if (m.accepted_answer) {
            texts.push_back(m.meaning);
        }
    }
    if (study_material.has_value()) {
        for (const auto& synonym : study_material->meaning_synonyms) {
            texts.push_back(synonym);
        }
    }
    return texts;
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
    const std::string input_key = answer_key(input);
    if (input_key.empty()) {
        return false;
    }

    std::vector<std::string> candidates;
    for (const auto& m : subject.meanings) {
        if (m.accepted_answer) {
            candidates.push_back(answer_key(m.meaning));
        }
    }
    if (study_material.has_value()) {
        for (const auto& synonym : study_material->meaning_synonyms) {
            candidates.push_back(answer_key(synonym));
        }
    }

    for (const auto& candidate : candidates) {
        if (candidate == input_key) {
            return true;
        }
    }

    if (input_key.size() > 4) {
        for (const auto& candidate : candidates) {
            if (levenshtein_distance(candidate, input_key) <= 1) {
                return true;
            }
        }
    }

    return false;
}

DrillSession::DrillSession(std::vector<ConfusionPair> pairs, const std::vector<Subject>& all_subjects,
                           const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                           int question_count, std::string now_override)
    : pairs_(std::move(pairs)),
      all_subjects_(all_subjects),
      store_(store),
      now_override_(std::move(now_override)),
      subject_by_id_(index_subjects(all_subjects)),
      study_material_by_id_(index_study_materials(study_materials)) {
    total_ = (pairs_.empty() || question_count <= 0)
                 ? 0
                 : std::min(static_cast<size_t>(question_count), pairs_.size());
}

std::optional<DrillQuestion> DrillSession::next() {
    pending_.reset();
    while (cursor_ < total_) {
        const size_t i = cursor_++;
        const ConfusionPair& pair = pairs_[i];
        if (subject_by_id_.find(pair.a_id) == subject_by_id_.end() ||
            subject_by_id_.find(pair.b_id) == subject_by_id_.end()) {
            continue;  // subject not synced (shouldn't happen); skip rather than crash
        }

        Pending p;
        p.pair_index = i;
        p.correct = subject_by_id_.at(pair.a_id);
        p.confusable = subject_by_id_.at(pair.b_id);
        // Alternate question type by overall question index: forced-
        // choice, production, forced-choice, ... (see drill.h).
        p.forced_choice = (i % 2 == 0);

        DrillQuestion q;
        q.index = static_cast<int>(i);
        q.total = static_cast<int>(total_);
        if (p.forced_choice) {
            // Alternate meaning/reading prompts across forced-choice
            // questions specifically (a separate counter from the
            // type-alternation index above), documented in drill.h.
            const bool ask_reading = (forced_choice_seen_ % 2 == 1);
            ++forced_choice_seen_;

            if (ask_reading) {
                if (const Reading* r = primary_reading(*p.correct); r != nullptr) {
                    p.shown_text = r->reading;
                    p.used_reading = true;
                }
            }
            if (!p.used_reading) {
                const Meaning* m = primary_meaning(*p.correct);
                p.shown_text = (m != nullptr) ? m->meaning : display_name(*p.correct);
            }

            const Subject* third = find_third_option(pair.a_id, pair.b_id, p.correct->level, all_subjects_,
                                                      subject_by_id_, store_);
            p.options = {p.correct, p.confusable};
            if (third != nullptr) {
                p.options.push_back(third);
            }
            // Rotate the option order deterministically (by question
            // index) rather than randomly, so the correct answer isn't
            // always printed first -- while staying fully reproducible
            // for tests. Documented in drill.h.
            const size_t rotate_amount = i % p.options.size();
            std::rotate(p.options.begin(), p.options.begin() + static_cast<long>(rotate_amount),
                        p.options.end());
            for (size_t k = 0; k < p.options.size(); ++k) {
                if (p.options[k]->id == p.correct->id) {
                    p.correct_index = k;
                    break;
                }
            }

            q.kind = DrillQuestion::Kind::ForcedChoice;
            q.asks_reading = p.used_reading;
            q.shown_text = p.shown_text;
            for (const Subject* o : p.options) {
                q.options.push_back(display_name(*o));
            }
            q.prompt = std::string("Which one ") + (p.used_reading ? "is read " : "means ") + "\"" +
                       p.shown_text + "\"?  " + join_options(p.options);
        } else {
            q.kind = DrillQuestion::Kind::Production;
            q.prompt = "What does \"" + display_name(*p.correct) + "\" mean?";
        }
        pending_ = std::move(p);
        return q;
    }
    return std::nullopt;
}

DrillFeedback DrillSession::answer(const std::string& line) {
    if (!pending_.has_value()) {
        throw std::logic_error("DrillSession::answer called with no pending question");
    }
    const Pending p = *pending_;
    pending_.reset();
    const ConfusionPair& pair = pairs_[p.pair_index];

    std::optional<wk_api::StudyMaterial> study_material;
    if (const auto it = study_material_by_id_.find(p.correct->id); it != study_material_by_id_.end()) {
        study_material = *it->second;
    }

    DrillFeedback fb;
    if (p.forced_choice) {
        const int chosen = parse_choice(line, p.options);
        std::optional<wk_api::StudyMaterial> chosen_study_material;
        if (chosen >= 0) {
            const auto it = study_material_by_id_.find(p.options[static_cast<size_t>(chosen)]->id);
            if (it != study_material_by_id_.end()) {
                chosen_study_material = *it->second;
            }
        }
        // Accept either the designated correct option or any other option
        // that legitimately shares the shown reading/meaning -- see
        // option_also_has_shown_text.
        fb.correct =
            chosen >= 0 && (static_cast<size_t>(chosen) == p.correct_index ||
                            option_also_has_shown_text(*p.options[static_cast<size_t>(chosen)], p.shown_text,
                                                        p.used_reading, chosen_study_material));
        if (!fb.correct) {
            fb.detail = "The correct answer was: " + display_name(*p.correct);
            fb.advice = first_advice_line(*p.correct, *p.confusable, pair.dominant_kind,
                                          p.used_reading ? FailureKind::Reading : FailureKind::Meaning,
                                          study_material, all_subjects_);
        }
    } else {
        fb.correct = matches_answer(line, *p.correct, study_material);
        if (!fb.correct) {
            const std::vector<std::string> accepted = accepted_answer_texts(*p.correct, study_material);
            std::ostringstream joined;
            for (size_t k = 0; k < accepted.size(); ++k) {
                if (k > 0) {
                    joined << ", ";
                }
                joined << accepted[k];
            }
            fb.detail = std::string("Accepted answer") + (accepted.size() == 1 ? ": " : "s: ") +
                        (accepted.empty() ? display_name(*p.correct) : joined.str());
            fb.advice = first_advice_line(*p.correct, *p.confusable, pair.dominant_kind, FailureKind::Meaning,
                                          study_material, all_subjects_);
        }
    }
    fb.reason = drill_reason_line(pair);

    store_.insert_drill_result({p.correct->id, p.confusable->id, fb.correct, now_iso8601()});

    ++asked_;
    if (fb.correct) {
        ++correct_;
    }
    const std::string now = now_override_.empty() ? now_iso8601() : now_override_;
    std::optional<PairSrsState> current;
    for (const auto& state : store_.all_pair_srs()) {
        if (state.a_id == std::min(pair.a_id, pair.b_id) && state.b_id == std::max(pair.a_id, pair.b_id)) {
            current = state;
            break;
        }
    }
    store_.upsert_pair_srs(advance_pair_srs(current, pair.a_id, pair.b_id, fb.correct, now));
    return fb;
}
