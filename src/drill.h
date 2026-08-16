#pragma once

#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "model.h"
#include "store.h"
#include "wk_api.h"

// Interactive terminal quiz over the same confusion pairs `wkr report`
// already computes. Local-only: reads `pairs`/`all_subjects`/
// `study_materials` (already loaded by the caller from Store), reads
// answers from `in`, writes only to `out` and `drill_result` rows via
// `store`. No HTTP of any kind happens anywhere in this file.

// Default number of questions per `wkr drill` run when `--count` is not
// given on the command line.
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

// Runs an interactive drill over the top `question_count` entries of
// `pairs` (already sorted descending by score -- see
// confusion.h::compute_confusion_pairs), one question per pair, reading
// answers from `in` and writing feedback/the running score to `out`.
//
// Question types alternate strictly by question index (forced-choice,
// production, forced-choice, ...) rather than being chosen randomly --
// simpler and deterministic/testable, per the brief's own preference.
// Within forced-choice questions, the meaning/reading prompt similarly
// alternates by a separate counter (see drill.cpp).
//
// For each question, `store.edges_for()` is used (read-only) to find a
// third multiple-choice distractor from the similarity graph; `store`
// is otherwise only written to, once per answered question, via
// `store.insert_drill_result`.
//
// Handles EOF on `in` (e.g. piped input running out mid-drill) by
// stopping the drill early and printing whatever partial score has
// accumulated, rather than crashing or looping forever.
void run_drill(const std::vector<ConfusionPair>& pairs, const std::vector<Subject>& all_subjects,
                const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                int question_count, std::istream& in, std::ostream& out);
