#pragma once

#include <optional>
#include <string>
#include <vector>

#include "model.h"
#include "wk_api.h"

// Generates a deterministic, multi-line focus-advice string for one
// confusion pair: `failed_subject` is the leech, `neighbor_subject` is
// the subject it was paired with (either an observed co-failure or a
// "likely" similarity-graph guess — see confusion.h). This function does
// no I/O and calls no LLM/NLP library: it is pure string building from
// stored component/reading/meaning/mnemonic data, directly testable.
//
// `all_subjects` resolves component_subject_ids to their slugs/meanings
// (rule 2) — passed in rather than looked up from a Store, to keep this
// function pure. `study_material` is `failed_subject`'s study material,
// if any.
//
// Rules are applied in order and each appends its own line(s), skipping
// itself when it has nothing to say (see the .cpp for each rule's skip
// condition); rule 1 (naming the failure type) always fires and is
// always first.
std::string generate_advice(const Subject& failed_subject, const Subject& neighbor_subject,
                             EdgeKind edge_kind, FailureKind failure_kind,
                             const std::optional<wk_api::StudyMaterial>& study_material,
                             const std::vector<Subject>& all_subjects);
