# Drill Metrics, Synonym Parity, and Local Install — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close three gaps a real user of `wkr` hit: custom meaning synonyms aren't honored in forced-choice grading, drill questions never say why they were picked, and drill/session/leech history is recorded but never surfaced — then install the built binary as a proper Nix package in the user's dotfiles.

**Architecture:** Every new piece of logic is a pure function over data the caller already loaded from `Store` (the same split `confusion.cpp`/`advice.cpp`/`report.cpp` already use) — no new file does its own DB or HTTP I/O. `Store` only grows two new read methods; the schema already has every table these tasks need. A new `stats.{h,cpp}` pair follows the exact shape of `confusion.h`/`report.h`: compute functions + one `print_*` renderer, wired into a new `wkr stats` subcommand in `main.cpp`. Task 10 is infrastructure only (a different repo, `~/Projects/.dotfiles`) — no C++ changes.

**Tech Stack:** C++20, CMake + Ninja, SQLite3 (via a hand-rolled `Store` wrapper, no ORM), nlohmann_json, Catch2 3, raw ANSI escape codes for colour. Packaging: Nix flakes / home-manager, `pkgs.stdenv.mkDerivation` + `pkgs.fetchFromGitHub`.

**Spec:** No separate spec doc — this plan's four tasks were scoped directly in conversation against the existing codebase (see `README.md`'s "Design notes" section and `git log --oneline` for Tasks 1–6, the established convention this plan continues as Tasks 7–10).

## Global Constraints

- C++20; build is `-Wall -Wextra -Werror` (`CMakeLists.txt`) — a warning anywhere is a build failure. Compile after every step that touches C++.
- No `POST`/`PUT`/`PATCH`/`DELETE` HTTP call anywhere in `src/` — verified by `grep -rn "CURLOPT_POST\|CUSTOMREQUEST\|CURLOPT_UPLOAD" src/` printing nothing (README's own guarantee). None of these tasks touch `http.{h,cpp}` or `wk_api.{h,cpp}`, so this stays true automatically — do not add a network call to satisfy any of these tasks.
- Every compute function this plan adds takes already-loaded data as parameters and does no DB/HTTP I/O itself (matches `confusion.cpp`, `advice.cpp`). Only `Store` methods and `main.cpp`'s `run_*_command` functions may call into SQLite.
- Every new `print_*`/renderer function takes `std::ostream&` (not `std::cout` directly) and an optional trailing `bool use_color = false`, so it stays testable via `std::ostringstream` and every existing call site keeps compiling unchanged (matches `print_report`, `run_drill`).
- Tests are Catch2 3, one `TEST_CASE` per behavior, tagged `"[drill]"`/`"[store]"`/`"[stats]"` per the existing per-file tag convention. Run via `ctest --test-dir build` (or the specific binary directly for faster iteration, e.g. `./build/wkr_tests "[stats]"`).
- This codebase's existing files carry substantial doc comments on every function (see `model.h`, `drill.h`, `confusion.h`) — that is this repo's established style, so new/modified functions in this plan follow the same density, not a minimal-comment style.
- Existing test assertions must keep passing unless a step explicitly says to change one; grep for the exact old string before editing a shared code path to know what else depends on its wording.

---

### Task 7: Synonym parity in forced-choice grading, and a full accepted-answer list on a wrong production answer

**Files:**
- Modify: `src/drill.cpp` (`option_also_has_shown_text`, `run_forced_choice_question`, `run_production_question`, all in the anonymous namespace)
- Test: `tests/test_drill.cpp`

**Interfaces:**
- Consumes: `wk_api::StudyMaterial` (`src/wk_api.h:25`, has `meaning_synonyms`), `Subject`/`Meaning` (`src/model.h:13-17`, `30-44`).
- Produces: no public signature changes — `matches_answer` and `run_drill` (both declared in `drill.h`) keep their exact existing signatures. This task only changes behavior inside `drill.cpp`'s anonymous namespace.

- [ ] **Step 1: Write the failing test for forced-choice synonym grading**

Add to `tests/test_drill.cpp`, after the existing `TEST_CASE("forced-choice meaning question accepts a distractor that also accepts the shown meaning", ...)` block (ends around line 355):

```cpp
TEST_CASE("forced-choice meaning question accepts a distractor via its StudyMaterial synonym, not just "
          "its own accepted-answer meanings",
          "[drill][regression]") {
    // Same shape as the "accepts a distractor that also accepts the shown
    // meaning" test above, except the shared answer lives in a
    // StudyMaterial synonym rather than being one of the distractor's own
    // Meaning entries -- the gap this task closes.
    Subject big;
    big.id = 31;
    big.characters = "大";
    big.slug = "big";
    big.level = 1;
    big.meanings = {Meaning{"Big", true, true}};

    Subject fat;
    fat.id = 32;
    fat.characters = "太";
    fat.slug = "fat";
    fat.level = 1;
    fat.meanings = {Meaning{"Fat", true, true}};  // no "Big" meaning at all

    Subject dog;
    dog.id = 33;
    dog.characters = "犬";
    dog.slug = "dog";
    dog.level = 1;
    dog.meanings = {Meaning{"Dog", true, true}};

    wk_api::StudyMaterial fat_material;
    fat_material.subject_id = fat.id;
    fat_material.meaning_synonyms = {"Big"};

    const std::string db_path = temp_db_path("forced_choice_synonym");
    std::filesystem::remove(db_path);
    Store store(db_path);

    // Q1 is forced-choice/meaning with no rotation: options [大, 太, 犬],
    // so 太 is choice 2. The question shows 大's meaning "Big"; 太 only
    // matches via its synonym.
    std::istringstream in("2\n");
    std::ostringstream out;
    run_drill({make_pair(31, 32)}, {big, fat, dog}, {fat_material}, store, /*question_count=*/1, in, out);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("Which one means \"Big\"?") != std::string::npos);
    CHECK(output.find("1/1 correct") != std::string::npos);

    const auto rows = read_drill_results(db_path);
    REQUIRE(rows.size() == 1);
    CHECK(std::get<2>(rows[0]) == true);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```sh
cmake --build build && ./build/wkr_tests "forced-choice meaning question accepts a distractor via its StudyMaterial synonym*"
```
Expected: FAIL — `1/1 correct` is not in the output (the answer grades wrong today, since `option_also_has_shown_text` never looks at `meaning_synonyms`).

- [ ] **Step 3: Give `option_also_has_shown_text` the option's `StudyMaterial` and check its synonyms**

In `src/drill.cpp`, replace the function (currently at line 247):

```cpp
bool option_also_has_shown_text(const Subject& option, const std::string& shown_text, bool reading_axis) {
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
    return false;
}
```

with:

```cpp
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
```

Then update its only call site, inside `run_forced_choice_question` (currently lines 336-343):

```cpp
    const int chosen = parse_choice(line, options);
    // Accept either the designated correct option or any other option
    // that legitimately shares the shown reading/meaning -- see
    // option_also_has_shown_text.
    const bool is_correct =
        chosen >= 0 && (static_cast<size_t>(chosen) == correct_index ||
                        option_also_has_shown_text(*options[static_cast<size_t>(chosen)], shown_text,
                                                    used_reading));
```

with:

```cpp
    const int chosen = parse_choice(line, options);
    std::optional<wk_api::StudyMaterial> chosen_study_material;
    if (chosen >= 0) {
        const auto it = study_material_by_id.find(options[static_cast<size_t>(chosen)]->id);
        if (it != study_material_by_id.end()) {
            chosen_study_material = *it->second;
        }
    }
    // Accept either the designated correct option or any other option
    // that legitimately shares the shown reading/meaning -- see
    // option_also_has_shown_text.
    const bool is_correct =
        chosen >= 0 && (static_cast<size_t>(chosen) == correct_index ||
                        option_also_has_shown_text(*options[static_cast<size_t>(chosen)], shown_text,
                                                    used_reading, chosen_study_material));
```

`study_material_by_id` is already in scope in `run_forced_choice_question` (it's a parameter — see its signature at line 283-288), so no signature change is needed there.

- [ ] **Step 4: Run the test to verify it passes**

Run:
```sh
cmake --build build && ./build/wkr_tests "[drill]"
```
Expected: PASS — every `[drill]` test, including the new one and the two existing "shared reading"/"shared meaning" regression tests (which pass `chosen_study_material` as `std::nullopt` implicitly, since their fixtures pass no study materials, and their behavior must be unchanged).

- [ ] **Step 5: Commit**

```bash
git add src/drill.cpp tests/test_drill.cpp
git commit -m "Task 7a: honor StudyMaterial meaning synonyms in forced-choice grading

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Write the failing test for the full accepted-answer list on a wrong production answer**

Add to `tests/test_drill.cpp`, right after the test added in Step 1:

```cpp
TEST_CASE("production wrong-answer feedback lists every accepted meaning and synonym, not just the "
          "primary one",
          "[drill][regression]") {
    Subject fat;
    fat.id = 41;
    fat.characters = "太";
    fat.slug = "fat";
    fat.level = 1;
    fat.meanings = {Meaning{"Fat", true, true}, Meaning{"Chubby", false, true}};

    wk_api::StudyMaterial material;
    material.subject_id = fat.id;
    material.meaning_synonyms = {"Porky"};

    Subject dog;
    dog.id = 42;
    dog.characters = "犬";
    dog.slug = "dog";
    dog.level = 1;
    dog.meanings = {Meaning{"Dog", true, true}};

    const std::string db_path = temp_db_path("full_accepted_list");
    std::filesystem::remove(db_path);
    Store store(db_path);

    // Q1 forced-choice, Q2 production (see drill.h's alternation rule) --
    // quiz dog first just to reach Q2 as a production question on fat.
    std::istringstream in("1\nnope\n");
    std::ostringstream out;
    run_drill({make_pair(42, 41), make_pair(41, 42)}, {fat, dog}, {material}, store, /*question_count=*/2, in,
              out);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("Incorrect. Accepted answers: Fat, Chubby, Porky") != std::string::npos);
}
```

- [ ] **Step 7: Run the test to verify it fails**

Run:
```sh
cmake --build build && ./build/wkr_tests "production wrong-answer feedback lists every accepted meaning*"
```
Expected: FAIL — actual output is `Incorrect. Accepted answer: Fat` (primary meaning only).

- [ ] **Step 8: Add an `accepted_answer_texts` helper and use it in the production feedback line**

In `src/drill.cpp`, add this helper to the anonymous namespace, next to `matches_answer`'s other helpers (before `matches_answer`'s definition, or anywhere else in the same anonymous namespace):

```cpp
// Every text that would grade a production answer correct for `subject`:
// its accepted-answer meanings (primary and non-primary) followed by
// every StudyMaterial synonym, in that order. Feeds the wrong-answer
// feedback line in run_production_question, so a user can see everything
// that would have counted -- including their own custom synonyms.
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
```

Then in `run_production_question`, replace the wrong-answer feedback block (currently lines 393-396):

```cpp
    } else {
        const Meaning* m = primary_meaning(quizzed_subject);
        out << colorize("Incorrect.", kColorRed, use_color) << " Accepted answer: "
            << (m != nullptr ? m->meaning : display_name(quizzed_subject)) << "\n";
```

with:

```cpp
    } else {
        const std::vector<std::string> accepted = accepted_answer_texts(quizzed_subject, study_material);
        std::ostringstream accepted_joined;
        for (size_t i = 0; i < accepted.size(); ++i) {
            if (i > 0) {
                accepted_joined << ", ";
            }
            accepted_joined << accepted[i];
        }
        out << colorize("Incorrect.", kColorRed, use_color) << " Accepted answer"
            << (accepted.size() == 1 ? ": " : "s: ")
            << (accepted.empty() ? display_name(quizzed_subject) : accepted_joined.str()) << "\n";
```

This keeps the exact old wording (`Accepted answer: Fat`, singular) for every subject with exactly one accepted meaning and no synonyms — which is every existing fixture except the new one added in Step 6 — so no other test needs to change.

- [ ] **Step 9: Run the test to verify it passes**

Run:
```sh
cmake --build build && ./build/wkr_tests "[drill]"
```
Expected: PASS — every `[drill]` test, including the existing `"run_drill scripted end-to-end..."` test whose Q2 assertion (`"Incorrect. Accepted answer: Fat"`) must still match verbatim.

- [ ] **Step 10: Full test suite and commit**

Run: `ctest --test-dir build`
Expected: 100% tests passed.

```bash
git add src/drill.cpp tests/test_drill.cpp
git commit -m "Task 7b: list every accepted meaning/synonym on a wrong production answer

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 8: Show why a drill question was picked (edge kind, likely vs. co-failure, score)

**Files:**
- Modify: `src/drill.cpp` (new anonymous-namespace helpers, plus one new line each in `run_forced_choice_question` and `run_production_question`)
- Test: `tests/test_drill.cpp`

**Interfaces:**
- Consumes: `ConfusionPair` (`src/model.h:138-143`: `score`, `dominant_kind`, `likely`), `EdgeKind` (`src/model.h:121`).
- Produces: no public signature changes — `run_drill`'s signature is unchanged; this only adds output text.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_drill.cpp`, after the `TEST_CASE("run_drill stops early on EOF mid-drill...")` block at the end of the file:

```cpp
TEST_CASE("run_drill prints why each question was picked: edge kind, likely vs co-failure, and score",
          "[drill]") {
    const std::string db_path = temp_db_path("reason_line");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = make_fixture_subjects();

    ConfusionPair co_failure = make_pair(2, 1);
    co_failure.dominant_kind = EdgeKind::Reading;
    co_failure.score = 4.2;
    co_failure.likely = false;

    ConfusionPair likely_guess = make_pair(1, 2);
    likely_guess.dominant_kind = EdgeKind::WkVisual;
    likely_guess.score = 0.4;
    likely_guess.likely = true;

    // Q1 forced-choice on co_failure, Q2 production on likely_guess (see
    // drill.h's alternation rule) -- the reason line must appear
    // regardless of whether the answer was right or wrong.
    std::istringstream in("1\nnope\n");
    std::ostringstream out;
    run_drill({co_failure, likely_guess}, subjects, /*study_materials=*/{}, store, /*question_count=*/2, in,
              out);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("Drilled because: these share a reading, you missed these together -- score 4.20") !=
          std::string::npos);
    CHECK(output.find("Drilled because: WaniKani flags these as visually similar, a likely guess (not yet "
                       "observed together) -- score 0.40") != std::string::npos);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```sh
cmake --build build && ./build/wkr_tests "run_drill prints why each question was picked*"
```
Expected: FAIL — `Drilled because:` does not appear anywhere in the output.

- [ ] **Step 3: Add `edge_kind_label` and `drill_reason_line`, and print the reason line from both question types**

Add `#include <iomanip>` to `src/drill.cpp`'s include block (needed for `std::fixed`/`std::setprecision`).

Add these two helpers to the anonymous namespace, next to `colorize`:

```cpp
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
```

In `run_forced_choice_question`, print the reason line right after the `Correct!`/`Incorrect.` block (after the `if (is_correct) { ... } else { ... }` block, before `store.insert_drill_result(...)`, currently around line 358-360):

```cpp
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
    out << drill_reason_line(pair) << "\n";

    store.insert_drill_result({correct_subject.id, confusable_subject.id, is_correct, now_iso8601()});
```

In `run_production_question`, make the same addition right before its own `store.insert_drill_result(...)` call (currently around line 402):

```cpp
    if (is_correct) {
        out << colorize("Correct!", kColorGreen, use_color) << "\n";
    } else {
        const std::vector<std::string> accepted = accepted_answer_texts(quizzed_subject, study_material);
        std::ostringstream accepted_joined;
        for (size_t i = 0; i < accepted.size(); ++i) {
            if (i > 0) {
                accepted_joined << ", ";
            }
            accepted_joined << accepted[i];
        }
        out << colorize("Incorrect.", kColorRed, use_color) << " Accepted answer"
            << (accepted.size() == 1 ? ": " : "s: ")
            << (accepted.empty() ? display_name(quizzed_subject) : accepted_joined.str()) << "\n";
        out << first_advice_line(quizzed_subject, confusable_subject, pair.dominant_kind, FailureKind::Meaning,
                                  study_material, all_subjects)
            << "\n";
    }
    out << drill_reason_line(pair) << "\n";

    store.insert_drill_result({quizzed_subject.id, confusable_subject.id, is_correct, now_iso8601()});
```

(This block already reflects Task 7b's rewritten wrong-answer feedback — Task 8 is written assuming Task 7 landed first, per this plan's task order.)

- [ ] **Step 4: Run the test to verify it passes**

Run:
```sh
cmake --build build && ./build/wkr_tests "[drill]"
```
Expected: PASS — every `[drill]` test. The reason line is new text appended after each question's existing feedback, and every other test asserts via `.find()` substring checks, not exact-length equality, so none of them need edits.

- [ ] **Step 5: Full test suite and commit**

Run: `ctest --test-dir build`
Expected: 100% tests passed.

```bash
git add src/drill.cpp tests/test_drill.cpp
git commit -m "Task 8: print why each drill question was picked (edge kind, likely vs co-failure, score)

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 9: `wkr stats` — drill history, session summary, leech trend

**Files:**
- Create: `src/stats.h`
- Create: `src/stats.cpp`
- Create: `tests/test_stats.cpp`
- Modify: `src/model.h` (four new structs: `DrillStats`, `MissedPairCount`, `SessionStats`, `LeechTrendEntry`)
- Modify: `src/store.h` (two new method declarations)
- Modify: `src/store.cpp` (two new method definitions)
- Modify: `tests/test_store.cpp` (three new tests for the two new Store methods)
- Modify: `src/main.cpp` (`run_stats_command`, dispatch, usage string)
- Modify: `CMakeLists.txt` (`src/stats.cpp` in `wkr_lib`, `tests/test_stats.cpp` in `wkr_tests`)
- Modify: `README.md` (document `wkr stats`)

**Interfaces:**
- Consumes: `DrillResult`, `Session`, `FailureEvent`, `ReviewStat`, `Subject` (all `src/model.h`); `Store::all_drill_results()`, `Store::stat_snapshots_for(long long)` (new, this task); `Store::all_sessions()`, `Store::all_failure_events()`, `Store::latest_stat_per_subject()`, `Store::all_subjects()` (existing); `compute_leech_scores` (`confusion.h`, existing).
- Produces (for any later task): `stats.h`'s `compute_drill_stats(std::vector<DrillResult>) -> DrillStats`, `compute_missed_pairs(const std::vector<DrillResult>&, size_t) -> std::vector<MissedPairCount>`, `compute_session_stats(const std::vector<Session>&, const std::vector<FailureEvent>&, const std::string&) -> SessionStats`, `compute_leech_trend(const std::vector<ReviewStat>&) -> std::vector<LeechTrendEntry>`, `print_stats(const DrillStats&, const std::vector<MissedPairCount>&, const SessionStats&, const std::vector<LeechTrendEntry>&, const std::vector<Subject>&, std::ostream&, bool use_color = false)`.

#### Step group A — model.h structs

- [ ] **Step A1: Add the four result structs to `model.h`**

Add to `src/model.h`, after the `DrillResult` struct (currently ending at line 166, right before the `parse_wk_timestamp` comment):

```cpp
// Aggregate drill_result-table stats, produced by stats.{h,cpp}'s
// compute_drill_stats. `total`/`correct` cover every recorded
// drill_result row; `current_streak` counts consecutive correct answers
// counting backward from the most recently answered row (0 if the most
// recent answer was wrong, or if there are no rows at all).
struct DrillStats {
    int total = 0;
    int correct = 0;
    int current_streak = 0;
};

// One (subject_id, distractor_id) pair from drill_result, ranked by how
// often that exact pairing was answered incorrectly. Produced by
// stats.{h,cpp}'s compute_missed_pairs.
struct MissedPairCount {
    long long subject_id;
    long long distractor_id;
    int miss_count;
};

// Aggregate session/failure_event stats, produced by stats.{h,cpp}'s
// compute_session_stats.
struct SessionStats {
    int session_count = 0;
    double avg_failures_per_session = 0.0;
    int sessions_last_7_days = 0;
};

// One subject's accuracy trend across every stat_snapshot Store has for
// it, produced by stats.{h,cpp}'s compute_leech_trend. `first_percentage`/
// `last_percentage` are percentage_correct from the chronologically
// oldest and newest snapshot passed in; `delta` = last - first (positive
// means improving). `snapshot_count` lets callers apply the "needs 2+
// syncs to say anything" honesty gate themselves before printing.
struct LeechTrendEntry {
    long long subject_id;
    int snapshot_count = 0;
    int first_percentage = 0;
    int last_percentage = 0;
    int delta = 0;
};
```

No test for this step alone — these are plain data structs with no behavior; Step group B's tests exercise them.

- [ ] **Step A2: Compile to confirm the header is still valid**

Run: `cmake --build build`
Expected: builds clean (nothing references the new structs yet, so this only checks for syntax errors in the header).

#### Step group B — `Store::all_drill_results` and `Store::stat_snapshots_for`

- [ ] **Step B1: Write the failing tests**

Add to `tests/test_store.cpp`, after the last `TEST_CASE` (`"all_assignments and all_study_materials return every row"`):

```cpp
TEST_CASE("all_drill_results returns every recorded drill_result row", "[store]") {
    const std::string db_path = temp_db_path("all_drill_results");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.insert_drill_result(DrillResult{1, 2, true, "2026-08-16T00:00:00Z"});
    store.insert_drill_result(DrillResult{2, 1, false, "2026-08-16T00:05:00Z"});

    const std::vector<DrillResult> results = store.all_drill_results();
    REQUIRE(results.size() == 2);
    CHECK(results[0].subject_id == 1);
    CHECK(results[0].distractor_id == 2);
    CHECK(results[0].correct == true);
    CHECK(results[1].subject_id == 2);
    CHECK(results[1].correct == false);
}

TEST_CASE("stat_snapshots_for returns every snapshot for a subject, ordered by data_updated_at ascending",
          "[store]") {
    const std::string db_path = temp_db_path("stat_snapshots_for");
    std::filesystem::remove(db_path);
    Store store(db_path);

    store.insert_stat_snapshot(make_stat(1, "2026-08-16T02:00:00.000000Z", 5));
    store.insert_stat_snapshot(make_stat(1, "2026-08-16T00:00:00.000000Z", 1));
    store.insert_stat_snapshot(make_stat(2, "2026-08-16T01:00:00.000000Z", 9));

    const std::vector<ReviewStat> history = store.stat_snapshots_for(1);
    REQUIRE(history.size() == 2);
    CHECK(history[0].meaning_incorrect == 1);
    CHECK(history[0].data_updated_at == "2026-08-16T00:00:00.000000Z");
    CHECK(history[1].meaning_incorrect == 5);
    CHECK(history[1].data_updated_at == "2026-08-16T02:00:00.000000Z");
}

TEST_CASE("stat_snapshots_for returns empty for a subject with no snapshots", "[store]") {
    const std::string db_path = temp_db_path("stat_snapshots_for_empty");
    std::filesystem::remove(db_path);
    Store store(db_path);

    CHECK(store.stat_snapshots_for(999).empty());
}
```

- [ ] **Step B2: Run the tests to verify they fail**

Run:
```sh
cmake --build build && ./build/wkr_tests "[store]"
```
Expected: FAIL to compile — `Store` has no `all_drill_results` or `stat_snapshots_for` members yet.

- [ ] **Step B3: Declare the two methods on `Store`**

In `src/store.h`, add after `insert_drill_result`'s declaration (the last method before the `private:` section):

```cpp
    // Every drill_result row, unordered (callers that need chronological
    // order, like stats.cpp's compute_drill_stats, sort their own copy).
    std::vector<DrillResult> all_drill_results();

    // Every stat_snapshot row for `subject_id`, ordered by
    // data_updated_at ascending -- the full history
    // Store::latest_stat_per_subject discards down to one row per
    // subject. subject_type is left empty (stat_snapshot does not
    // persist it; see latest_stat_per_subject's same limitation).
    std::vector<ReviewStat> stat_snapshots_for(long long subject_id);
```

- [ ] **Step B4: Implement both methods in `store.cpp`**

In `src/store.cpp`, add after `Store::insert_drill_result`'s definition (the last function in the file):

```cpp
std::vector<DrillResult> Store::all_drill_results() {
    static const char* sql = "SELECT subject_id, distractor_id, correct, answered_at FROM drill_result;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare all_drill_results failed: ") + sqlite3_errmsg(db_));
    }

    std::vector<DrillResult> results;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DrillResult result;
        result.subject_id = sqlite3_column_int64(stmt, 0);
        result.distractor_id = sqlite3_column_int64(stmt, 1);
        result.correct = sqlite3_column_int(stmt, 2) != 0;
        const unsigned char* answered = sqlite3_column_text(stmt, 3);
        result.answered_at = answered != nullptr ? reinterpret_cast<const char*>(answered) : "";
        results.push_back(std::move(result));
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("all_drill_results failed: " + message);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<ReviewStat> Store::stat_snapshots_for(long long subject_id) {
    static const char* sql =
        "SELECT data_updated_at, meaning_correct, meaning_incorrect, reading_correct, reading_incorrect, "
        "meaning_current_streak, reading_current_streak, percentage_correct FROM stat_snapshot "
        "WHERE subject_id = ? ORDER BY data_updated_at ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("prepare stat_snapshots_for failed: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, subject_id);

    std::vector<ReviewStat> stats;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ReviewStat stat;
        stat.subject_id = subject_id;
        const unsigned char* updated = sqlite3_column_text(stmt, 0);
        stat.data_updated_at = updated != nullptr ? reinterpret_cast<const char*>(updated) : "";
        stat.meaning_correct = sqlite3_column_int(stmt, 1);
        stat.meaning_incorrect = sqlite3_column_int(stmt, 2);
        stat.reading_correct = sqlite3_column_int(stmt, 3);
        stat.reading_incorrect = sqlite3_column_int(stmt, 4);
        stat.meaning_current_streak = sqlite3_column_int(stmt, 5);
        stat.reading_current_streak = sqlite3_column_int(stmt, 6);
        stat.percentage_correct = sqlite3_column_int(stmt, 7);
        stats.push_back(stat);
    }
    if (rc != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        throw std::runtime_error("stat_snapshots_for failed: " + message);
    }
    sqlite3_finalize(stmt);
    return stats;
}
```

- [ ] **Step B5: Run the tests to verify they pass**

Run:
```sh
cmake --build build && ./build/wkr_tests "[store]"
```
Expected: PASS — every `[store]` test.

- [ ] **Step B6: Commit**

```bash
git add src/store.h src/store.cpp tests/test_store.cpp src/model.h
git commit -m "Task 9a: add Store::all_drill_results and Store::stat_snapshots_for

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

#### Step group C — `stats.cpp`'s compute functions

- [ ] **Step C1: Create `src/stats.h`**

```cpp
#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "model.h"

// Computes local-only metrics from data already loaded from Store: drill
// history, session summary, and per-leech accuracy trend. Every function
// here is pure (no DB or HTTP access) -- main.cpp loads from Store and
// passes the result in, the same split confusion.h and report.cpp already
// use.

// Overall accuracy plus the current correct-streak (consecutive correct
// answers counting backward from the most recently answered row) from
// every recorded drill_result. `results` need not be pre-sorted; this
// sorts its own copy by answered_at ascending to compute the streak.
DrillStats compute_drill_stats(std::vector<DrillResult> results);

// The top `limit` (subject_id, distractor_id) pairs by how often that
// exact pairing was answered incorrectly, descending by miss_count. Pairs
// tied on miss_count keep the order they first appear in `results`.
std::vector<MissedPairCount> compute_missed_pairs(const std::vector<DrillResult>& results, size_t limit);

// Session count, average failure_event count per session, and how many
// sessions started within 7 days of `now_iso8601`.
SessionStats compute_session_stats(const std::vector<Session>& sessions,
                                    const std::vector<FailureEvent>& events, const std::string& now_iso8601);

// One LeechTrendEntry per distinct subject_id present in `history`,
// sorted descending by |delta| (the biggest swings, worsening or
// improving, first). `history` is every stat_snapshot row for whichever
// subjects the caller cares about -- main.cpp builds it by calling
// Store::stat_snapshots_for once per current leech and concatenating, so
// this scores leeches, not all ~9000 WaniKani subjects.
std::vector<LeechTrendEntry> compute_leech_trend(const std::vector<ReviewStat>& history);

// Renders the `wkr stats` terminal output: a drill-history section (with
// its most-missed pairs), a session-summary section, and a leech-trend
// section -- each with an honest "nothing yet" fallback line rather than
// printing an empty/misleading section. `use_color`, when true, wraps
// each section heading in cyan, matching print_report's convention.
// Defaults to false so it's testable via std::ostringstream without
// passing anything extra.
void print_stats(const DrillStats& drill, const std::vector<MissedPairCount>& missed_pairs,
                  const SessionStats& sessions, const std::vector<LeechTrendEntry>& trend,
                  const std::vector<Subject>& all_subjects, std::ostream& out, bool use_color = false);
```

- [ ] **Step C2: Write the failing tests for `compute_drill_stats` and `compute_missed_pairs`**

Create `tests/test_stats.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "stats.h"

TEST_CASE("compute_drill_stats totals correct/total and current streak from the tail", "[stats]") {
    const std::vector<DrillResult> results = {
        DrillResult{1, 2, true, "2026-08-10T00:00:00Z"},
        DrillResult{2, 1, false, "2026-08-11T00:00:00Z"},
        DrillResult{1, 2, true, "2026-08-12T00:00:00Z"},
        DrillResult{2, 1, true, "2026-08-13T00:00:00Z"},
    };

    const DrillStats stats = compute_drill_stats(results);
    CHECK(stats.total == 4);
    CHECK(stats.correct == 3);
    // Streak counts backward from the most recent (2026-08-13) result:
    // 08-13 correct, 08-12 correct, 08-11 wrong -- stops there, streak 2.
    CHECK(stats.current_streak == 2);
}

TEST_CASE("compute_drill_stats reports zero streak when the most recent answer was wrong", "[stats]") {
    const std::vector<DrillResult> results = {
        DrillResult{1, 2, true, "2026-08-10T00:00:00Z"},
        DrillResult{1, 2, false, "2026-08-11T00:00:00Z"},
    };

    CHECK(compute_drill_stats(results).current_streak == 0);
}

TEST_CASE("compute_drill_stats on an empty history returns all zeros", "[stats]") {
    const DrillStats stats = compute_drill_stats({});
    CHECK(stats.total == 0);
    CHECK(stats.correct == 0);
    CHECK(stats.current_streak == 0);
}

TEST_CASE("compute_missed_pairs ranks by miss count, descending, capped at limit", "[stats]") {
    const std::vector<DrillResult> results = {
        DrillResult{1, 2, false, "t1"}, DrillResult{1, 2, false, "t2"}, DrillResult{3, 4, false, "t3"},
        DrillResult{1, 2, true, "t4"},  // correct: not a miss
        DrillResult{5, 6, false, "t5"},
    };

    const std::vector<MissedPairCount> top2 = compute_missed_pairs(results, 2);
    REQUIRE(top2.size() == 2);
    CHECK(top2[0].subject_id == 1);
    CHECK(top2[0].distractor_id == 2);
    CHECK(top2[0].miss_count == 2);
    // (3,4) and (5,6) are tied at 1 miss each; the plan's stable_sort
    // keeps (3,4) first since it appeared earlier in `results`.
    CHECK(top2[1].subject_id == 3);
    CHECK(top2[1].miss_count == 1);
}

TEST_CASE("compute_missed_pairs with no misses at all returns empty", "[stats]") {
    const std::vector<DrillResult> results = {DrillResult{1, 2, true, "t1"}};
    CHECK(compute_missed_pairs(results, 5).empty());
}
```

- [ ] **Step C3: Run the tests to verify they fail**

Run: `cmake --build build`
Expected: FAIL to compile — `stats.h` has no matching `.cpp`/target yet. (This step and Step C2's "verify it fails" are combined because there's nothing to link against until the CMakeLists wiring in Step group E — see the note at the top of Step group E.)

- [ ] **Step C4: Implement `compute_drill_stats` and `compute_missed_pairs`**

Create `src/stats.cpp`:

```cpp
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
```

(`compute_session_stats`, `compute_leech_trend`, and `print_stats` are added in later steps of this same file — this step only needs to compile on its own once the CMakeLists wiring lands in Step group E, so leave the rest of the functions declared in `stats.h` unimplemented for now; the file won't link until Step C6/C8 add them. To avoid a broken intermediate state, implement all four compute functions plus `print_stats` in one pass before ever building — continue straight to Steps C5-C8 before building for the first time.)

- [ ] **Step C5: Write the failing tests for `compute_session_stats`**

Add to `tests/test_stats.cpp`, after the `compute_missed_pairs` tests:

```cpp
TEST_CASE("compute_session_stats averages failures per session and counts recent ones", "[stats]") {
    const std::vector<Session> sessions = {
        Session{1, "2026-08-01T00:00:00Z", "2026-08-01T00:10:00Z"},
        Session{2, "2026-08-15T00:00:00Z", "2026-08-15T00:10:00Z"},
    };
    const std::vector<FailureEvent> events = {
        FailureEvent{1, 100, "2026-08-01T00:00:00Z", FailureKind::Meaning, 1, false},
        FailureEvent{2, 101, "2026-08-01T00:05:00Z", FailureKind::Meaning, 1, false},
        FailureEvent{3, 102, "2026-08-15T00:00:00Z", FailureKind::Meaning, 2, false},
    };

    // "now" is 2026-08-16: one day after session 2 (within 7 days) and 15
    // days after session 1 (outside 7 days).
    const SessionStats stats = compute_session_stats(sessions, events, "2026-08-16T00:00:00Z");
    CHECK(stats.session_count == 2);
    CHECK(std::abs(stats.avg_failures_per_session - 1.5) < 0.001);
    CHECK(stats.sessions_last_7_days == 1);
}

TEST_CASE("compute_session_stats on no sessions returns all zeros", "[stats]") {
    const SessionStats stats = compute_session_stats({}, {}, "2026-08-16T00:00:00Z");
    CHECK(stats.session_count == 0);
    CHECK(stats.avg_failures_per_session == 0.0);
    CHECK(stats.sessions_last_7_days == 0);
}
```

- [ ] **Step C6: Implement `compute_session_stats`**

Append to `src/stats.cpp`:

```cpp
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
```

- [ ] **Step C7: Write the failing tests for `compute_leech_trend`**

Add to `tests/test_stats.cpp`, after the `compute_session_stats` tests:

```cpp
TEST_CASE("compute_leech_trend groups by subject and reports first/last percentage and delta", "[stats]") {
    ReviewStat s1;
    s1.subject_id = 1;
    s1.data_updated_at = "2026-08-01T00:00:00Z";
    s1.percentage_correct = 40;

    ReviewStat s2;
    s2.subject_id = 1;
    s2.data_updated_at = "2026-08-10T00:00:00Z";
    s2.percentage_correct = 70;

    ReviewStat s3;
    s3.subject_id = 2;
    s3.data_updated_at = "2026-08-01T00:00:00Z";
    s3.percentage_correct = 90;

    ReviewStat s4;
    s4.subject_id = 2;
    s4.data_updated_at = "2026-08-10T00:00:00Z";
    s4.percentage_correct = 85;

    const std::vector<LeechTrendEntry> trend = compute_leech_trend({s1, s2, s3, s4});
    REQUIRE(trend.size() == 2);
    // Sorted by |delta| descending: subject 1 improved by 30, subject 2
    // worsened by 5 -- 30 > 5, so subject 1 comes first.
    CHECK(trend[0].subject_id == 1);
    CHECK(trend[0].snapshot_count == 2);
    CHECK(trend[0].first_percentage == 40);
    CHECK(trend[0].last_percentage == 70);
    CHECK(trend[0].delta == 30);
    CHECK(trend[1].subject_id == 2);
    CHECK(trend[1].delta == -5);
}

TEST_CASE("compute_leech_trend on a single snapshot reports zero delta", "[stats]") {
    ReviewStat s1;
    s1.subject_id = 1;
    s1.data_updated_at = "2026-08-01T00:00:00Z";
    s1.percentage_correct = 50;

    const std::vector<LeechTrendEntry> trend = compute_leech_trend({s1});
    REQUIRE(trend.size() == 1);
    CHECK(trend[0].snapshot_count == 1);
    CHECK(trend[0].delta == 0);
}
```

- [ ] **Step C8: Implement `compute_leech_trend` and `print_stats`**

Append to `src/stats.cpp`:

```cpp
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
```

- [ ] **Step C9: Write the failing tests for `print_stats`**

Add to `tests/test_stats.cpp`, after the `compute_leech_trend` tests:

```cpp
#include <sstream>

TEST_CASE("print_stats renders the no-history fallback line in every section", "[stats]") {
    std::ostringstream out;
    print_stats(DrillStats{}, {}, SessionStats{}, {}, {}, out);
    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("No drills recorded yet") != std::string::npos);
    CHECK(output.find("No sessions recorded yet") != std::string::npos);
    CHECK(output.find("Not enough sync history yet") != std::string::npos);
}

TEST_CASE("print_stats renders populated drill stats, missed pairs, sessions, and a shown leech trend",
          "[stats]") {
    Subject fat;
    fat.id = 1;
    fat.characters = "太";
    fat.slug = "fat";

    Subject dog;
    dog.id = 2;
    dog.characters = "犬";
    dog.slug = "dog";

    const DrillStats drill{10, 7, 3};
    const std::vector<MissedPairCount> missed = {MissedPairCount{1, 2, 4}};
    const SessionStats sessions{5, 1.5, 2};
    const std::vector<LeechTrendEntry> trend = {LeechTrendEntry{1, 2, 40, 70, 30}};

    std::ostringstream out;
    print_stats(drill, missed, sessions, trend, {fat, dog}, out);
    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("7/10 correct (70.0%), current streak 3") != std::string::npos);
    CHECK(output.find("太 / 犬  missed 4x") != std::string::npos);
    CHECK(output.find("5 sessions") != std::string::npos);
    CHECK(output.find("太  40% -> 70%  (+30)") != std::string::npos);
}
```

Move the `#include <sstream>` this step adds to the top of `tests/test_stats.cpp` with the other includes rather than mid-file.

- [ ] **Step C10: Full unit build and test run**

This is the first point `stats.cpp`/`test_stats.cpp` can actually build (see Step group E for the CMakeLists wiring, which must land before this step can run) — do Step group E now, then come back and run:
```sh
cmake --build build && ./build/wkr_tests "[stats]"
```
Expected: PASS — every `[stats]` test.

- [ ] **Step C11: Commit**

```bash
git add src/stats.h src/stats.cpp tests/test_stats.cpp
git commit -m "Task 9b: add stats.{h,cpp} -- drill/session/leech-trend compute + print_stats

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

#### Step group D — wire `wkr stats` into `main.cpp`

- [ ] **Step D1: Add `run_stats_command` and dispatch**

In `src/main.cpp`, add `#include "stats.h"` to the include block.

Add, in the anonymous namespace, right after `run_drill_command` (before `print_usage`):

```cpp
// Named distinctly from stats.h's print_stats (which this wraps), same
// reasoning as run_drill_command/run_report above.
int run_stats_command() {
    try {
        Config config = Config::load();
        Store store(config.db_path);

        const std::vector<Subject> subjects = store.all_subjects();
        if (subjects.empty()) {
            std::cout << "No data yet — run 'wkr sync' first." << std::endl;
            return 0;
        }

        const std::vector<DrillResult> drill_results = store.all_drill_results();
        const DrillStats drill_stats = compute_drill_stats(drill_results);
        const std::vector<MissedPairCount> missed_pairs = compute_missed_pairs(drill_results, 5);

        const std::vector<Session> sessions = store.all_sessions();
        const std::vector<FailureEvent> events = store.all_failure_events();
        const SessionStats session_stats = compute_session_stats(sessions, events, now_iso8601());

        const std::vector<ReviewStat> latest_stats = store.latest_stat_per_subject();
        const std::vector<LeechEntry> leeches = compute_leech_scores(latest_stats);
        std::vector<ReviewStat> leech_history;
        for (const auto& leech : leeches) {
            const std::vector<ReviewStat> subject_history = store.stat_snapshots_for(leech.subject_id);
            leech_history.insert(leech_history.end(), subject_history.begin(), subject_history.end());
        }
        const std::vector<LeechTrendEntry> trend = compute_leech_trend(leech_history);

        print_stats(drill_stats, missed_pairs, session_stats, trend, subjects, std::cout, stdout_is_tty());
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
```

Update `print_usage` (currently line 145):
```cpp
void print_usage() {
    std::cerr << "usage: wkr <sync [--session-gap-minutes N]|report|drill [--count N]|stats>" << std::endl;
}
```

Add dispatch in `main`, after the `drill` block and before `print_usage(); return 1;`:
```cpp
    if (command == "stats") {
        return run_stats_command();
    }
```

- [ ] **Step D2: Add an integration test in `tests/test_integration.cpp`**

Read `tests/test_integration.cpp` first to match its existing fixture-building conventions exactly (it drives the real sync pipeline against fake fetchers, so the exact helper names/shapes must match what's already there — do not invent new ones). Add one `TEST_CASE` there asserting that after running the existing sync fixture and one scripted `run_drill` call, `store.all_drill_results()` returns the expected rows and `compute_drill_stats`/`compute_session_stats` on that same store's data produce non-crashing, sane results (`total >= 0`, `session_count >= 0`). This is an integration smoke test, not a re-test of stats.cpp's own logic (already covered by Step group C) — keep it to one `TEST_CASE`.

- [ ] **Step D3: Build and run the full suite**

Run:
```sh
cmake --build build && ctest --test-dir build
```
Expected: 100% tests passed, including the new integration test.

- [ ] **Step D4: Manual smoke test against a real local DB**

Run:
```sh
./build/wkr stats
```
Expected (on a DB with no data): `No data yet — run 'wkr sync' first.` Run again after `wkr sync`/`wkr drill` have populated the DB and confirm the three sections print without crashing.

- [ ] **Step D5: Commit**

```bash
git add src/main.cpp tests/test_integration.cpp
git commit -m "Task 9c: wire 'wkr stats' into main.cpp

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

#### Step group E — CMakeLists.txt wiring (do this before Step C10)

- [ ] **Step E1: Add the new files to the build**

In `CMakeLists.txt`, add `src/stats.cpp` to `wkr_lib`'s source list (after `src/drill.cpp`):
```cmake
add_library(wkr_lib STATIC
    src/config.cpp
    src/http.cpp
    src/wk_api.cpp
    src/store.cpp
    src/sync.cpp
    src/similarity.cpp
    src/confusion.cpp
    src/width.cpp
    src/advice.cpp
    src/report.cpp
    src/drill.cpp
    src/stats.cpp
)
```

Add `tests/test_stats.cpp` to `wkr_tests`'s source list (after `tests/test_drill.cpp`):
```cmake
add_executable(wkr_tests
    tests/test_model.cpp
    tests/test_config.cpp
    tests/test_store.cpp
    tests/test_failure_detection.cpp
    tests/test_similarity.cpp
    tests/test_width.cpp
    tests/test_confusion.cpp
    tests/test_advice.cpp
    tests/test_drill.cpp
    tests/test_stats.cpp
    tests/test_integration.cpp
)
```

- [ ] **Step E2: Re-run cmake's configure step and build**

Run:
```sh
cmake -B build && cmake --build build
```
Expected: configures and builds clean (this is the point where Step group C's `stats.cpp`/`test_stats.cpp` actually get compiled for the first time — go back to Step C10 now).

- [ ] **Step E3: Commit**

```bash
git add CMakeLists.txt
git commit -m "Task 9d: add stats.cpp and test_stats.cpp to the build

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

#### Step group F — README

- [ ] **Step F1: Document `wkr stats`**

In `README.md`'s "Usage" section, add a fourth command line after `wkr drill`:
```
wkr stats                            # print local drill history, session summary, and leech accuracy trend
```
And a bullet under it:
```
- `wkr stats` is purely local, like `wkr report`/`wkr drill`. It needs at
  least one `wkr drill` run for drill-history stats, at least one `wkr
  sync` for session stats, and at least two separate `wkr sync` runs on
  different days for a leech's accuracy trend to show anything — each
  section prints its own honest "nothing yet" line until then.
```

- [ ] **Step F2: Commit**

```bash
git add README.md
git commit -m "Task 9e: document wkr stats in README

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 10: Install `wkr` via home-manager (`fetchFromGitHub`)

This task has no automated test cycle — it is infrastructure in a different repo (`~/Projects/.dotfiles`), not C++ covered by `ctest`. Each step's "verify" is a real command against the user's live system instead of a unit test.

**Files (all in `~/Projects/.dotfiles`, not this repo):**
- Modify: `.config/home-manager/pins.nix`
- Modify: `.config/home-manager/home.nix`

- [ ] **Step 1: Push wanikani-reinforcer and record the commit to pin**

Run (in `/home/elliancarlos/Projects/wanikani-reinforcer`):
```sh
git push origin main
git rev-parse HEAD
```
Record the printed commit SHA — it's `<rev>` in the next step.

- [ ] **Step 2: Add the pin**

In `~/Projects/.dotfiles/.config/home-manager/pins.nix`, add before the closing `}`:
```nix
  wanikani-reinforcer = {
    owner = "EllianCarlos";
    repo = "wanikani-reinforcer";
    rev = "<rev-from-step-1>";
  };
```

- [ ] **Step 3: Add the `wkr` derivation**

In `~/Projects/.dotfiles/.config/home-manager/home.nix`, add to the `let` block, after the `herdr` derivation (before the closing `in`):
```nix
  # wkr (github.com/EllianCarlos/wanikani-reinforcer) -- WaniKani review
  # analysis CLI. Unlike mnemon/tuicr/herdr above, upstream ships no
  # release binary, so this builds from source via fetchFromGitHub rather
  # than fetchurl. catch2_3 stays a build input even though only the
  # `wkr` binary target is built below: CMakeLists.txt calls
  # `find_package(Catch2 3 REQUIRED)` unconditionally at configure time.
  wkr = pkgs.stdenv.mkDerivation {
    pname = "wkr";
    version = "0-unstable-2026-08-16";
    src = pkgs.fetchFromGitHub {
      inherit (pins.wanikani-reinforcer) owner repo rev;
      hash = "";
    };
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config ];
    buildInputs = [ pkgs.curl.dev pkgs.sqlite.dev pkgs.nlohmann_json pkgs.catch2_3 ];
    cmakeFlags = [ "-GNinja" ];
    buildPhase = "ninja wkr";
    installPhase = ''
      mkdir -p $out/bin
      install -m755 wkr $out/bin/wkr
    '';
  };
```

- [ ] **Step 4: Add `wkr` to `home.packages`**

In the same file's `home.packages = with pkgs; [ ... ]` list, add `wkr` (it is not `pkgs.wkr` — it's the `let`-bound derivation above, so reference it bare as `wkr`, the same way `mnemon`/`tuicr`/`herdr` are referenced in that list already).

- [ ] **Step 5: First apply to get the real hash**

Run the user's usual apply flow (the `nixapply` skill, or the `update` shell alias). Expected: it fails with a hash mismatch, reporting a `got: sha256-...` line. Copy that hash into Step 3's `hash = "";` field.

- [ ] **Step 6: Re-apply and verify**

Run the apply flow again.
Expected: succeeds. Then run:
```sh
wkr sync
wkr report
```
Expected: `wkr sync` connects using the already-configured `WANIKANI_API_TOKEN`/token file (done — see conversation) and completes; `wkr report` prints real output (or "No data yet" only if this is a brand new DB).

- [ ] **Step 7: Commit in the dotfiles repo**

```bash
cd ~/Projects/.dotfiles
git add .config/home-manager/pins.nix .config/home-manager/home.nix
git commit -m "Add wkr (wanikani-reinforcer) as a home-manager package, built from source

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

- [ ] **Step 8: Confirm check-nix-pins picks it up automatically**

Run whatever `check-nix-pins` invocation this dotfiles repo already uses (see `pin-check.nix`). Expected: `wanikani-reinforcer` appears in its report alongside the other pins — no separate registration step, since it reads every entry in `pins.nix`.

---

## Self-Review

**1. Spec coverage** — every item scoped in conversation has a task: forced-choice synonym parity (Task 7a), full accepted-answer list on a miss (Task 7b), drill-selection reason line (Task 8), drill-history stats (Task 9, step group C's `compute_drill_stats`/`compute_missed_pairs`), sync/session summary stats (Task 9, `compute_session_stats`), leech trend over time (Task 9, `compute_leech_trend`), and install via `fetchFromGitHub` in dotfiles (Task 10). No gaps.

**2. Placeholder scan** — no `TBD`/`TODO`/"add error handling" left in any step; every code block is complete, copy-pasteable content, not a description of what to write.

**3. Type consistency** — checked across tasks: `ConfusionPair.score`/`dominant_kind`/`likely` (Task 8) match `model.h:138-143` exactly. `option_also_has_shown_text`'s new 4th parameter type (`const std::optional<wk_api::StudyMaterial>&`) matches `study_material_by_id`'s value type (`const wk_api::StudyMaterial*`, dereferenced) at its call site. `stats.h`'s four compute-function signatures match their `stats.cpp` implementations and their `main.cpp` call sites exactly (`compute_drill_stats(std::vector<DrillResult>)` by value — intentional, since it sorts its own copy — versus the other three by `const&`). `Store::all_drill_results`/`stat_snapshots_for` signatures match their `store.h` declarations, `store.cpp` definitions, and `test_store.cpp`/`main.cpp` call sites.

Task order matters: Task 8's code blocks assume Task 7b's rewritten wrong-answer feedback block already landed (noted explicitly in Task 8 Step 3). Task 9 assumes Tasks 7 and 8 are unrelated to it and can land in any order relative to them. Task 10 has no code dependency on Tasks 7-9 but should come last since it packages whatever commit is HEAD at that point.
