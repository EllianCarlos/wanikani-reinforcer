#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "model.h"
#include "store.h"
#include "wk_api.h"

// Quiz over the same confusion pairs `wkr report` already computes.
// Local-only: reads `pairs`/`all_subjects`/`study_materials` (already
// loaded by the caller from Store) and writes only `drill_result` /
// pair-SRS rows via `store`. No HTTP of any kind happens anywhere in this
// file. Question types alternate strictly by question index
// (forced-choice, production, ...); forced-choice prompts alternate
// meaning/reading by a separate counter; option order is rotated by
// question index. All deterministic, for testability.

// Default number of questions per drill session.
constexpr int kDefaultDrillQuestionCount = 5;

// Computes the Levenshtein (edit) distance between `a` and `b`: the
// minimum number of single-character insertions, deletions, or
// substitutions to turn one into the other. Operates byte-wise, which is
// exact for the ASCII meaning/synonym text it's used against here.
int levenshtein_distance(const std::string& a, const std::string& b);

// Answer-matching helper for the production question type, extracted as
// a standalone testable function rather than buried in run_drill's I/O
// loop. Trims and lowercases `input`, then checks it (case-insensitively)
// against every `Meaning` on `subject` with `accepted_answer == true`,
// plus every entry of `study_material`'s `meaning_synonyms` if present.
// If the trimmed input is longer than 4 characters, also accepts a match
// within Levenshtein distance 1 of any of those candidates (typo
// tolerance, matching WaniKani's own behavior) -- the length gate is
// applied to the trimmed *input*, not the candidate, so short inputs like
// "cat"/"car" can't accidentally fuzzy-match one another.
bool matches_answer(const std::string& input, const Subject& subject,
                     const std::optional<wk_api::StudyMaterial>& study_material);

// One drill question, ready to display. `prompt` is the exact text shown
// before the input prompt (for forced-choice it already includes the
// joined options); `options` is empty for production questions.
struct DrillQuestion {
    enum class Kind { ForcedChoice, Production };
    Kind kind = Kind::Production;
    std::string prompt;
    std::string shown_text;           // forced-choice only
    bool asks_reading = false;        // forced-choice only
    std::vector<std::string> options; // forced-choice only, in display order
    int index = 0;                    // position in the pair list (0-based)
    int total = 0;
};

// Grading result for one answered question. `detail` and `advice` are
// empty when `correct`; `reason` is always set.
struct DrillFeedback {
    bool correct = false;
    std::string detail;
    std::string advice;
    std::string reason;
};

// Step-wise drill engine: call next() for a question, then answer() with
// the user's raw input. answer() persists the drill_result row and the pair
// SRS update immediately, so abandoning a session loses nothing already
// answered. `all_subjects` and `study_materials` must outlive the session.
class DrillSession {
public:
    DrillSession(std::vector<ConfusionPair> pairs, const std::vector<Subject>& all_subjects,
                 const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                 int question_count, std::string now_override = "");

    // Next question, or nullopt when the session is finished.
    std::optional<DrillQuestion> next();
    // Grades `line` against the question last returned by next().
    DrillFeedback answer(const std::string& line);

    int asked() const { return asked_; }
    int correct() const { return correct_; }
    int total() const { return static_cast<int>(total_); }

private:
    struct Pending {
        size_t pair_index = 0;
        const Subject* correct = nullptr;
        const Subject* confusable = nullptr;
        bool forced_choice = false;
        bool used_reading = false;
        std::string shown_text;
        std::vector<const Subject*> options;
        size_t correct_index = 0;
    };

    std::vector<ConfusionPair> pairs_;
    const std::vector<Subject>& all_subjects_;
    Store& store_;
    std::string now_override_;
    std::map<long long, const Subject*> subject_by_id_;
    std::map<long long, const wk_api::StudyMaterial*> study_material_by_id_;
    size_t total_ = 0;
    size_t cursor_ = 0;
    int asked_ = 0;
    int correct_ = 0;
    int forced_choice_seen_ = 0;
    std::optional<Pending> pending_;
};
