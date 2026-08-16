#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <filesystem>
#include <sstream>
#include <tuple>

#include "drill.h"
#include "model.h"
#include "store.h"
#include "wk_api.h"

namespace {

// Returns a fresh temp db path per test invocation, mirroring
// test_store.cpp's helper of the same name/behavior.
std::string temp_db_path(const std::string& label) {
    static int counter = 0;
    std::ostringstream name;
    name << "wkr_drill_test_" << label << "_" << (counter++) << ".db";
    return (std::filesystem::temp_directory_path() / name.str()).string();
}

// Reads every drill_result row (subject_id, distractor_id, correct),
// ordered by id, bypassing Store (which intentionally has no read
// accessor for this table beyond what this task needs).
std::vector<std::tuple<long long, long long, bool>> read_drill_results(const std::string& db_path) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT subject_id, distractor_id, correct FROM drill_result ORDER BY id;",
                                -1, &stmt, nullptr) == SQLITE_OK);
    std::vector<std::tuple<long long, long long, bool>> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.emplace_back(sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1),
                           sqlite3_column_int(stmt, 2) != 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

// Fixture: 太 (fat, id=1) / 犬 (dog, id=2) confusion pair, plus 大 (big,
// id=3) at the same level as a same-level fallback distractor (the test
// store has no similarity_edge rows, so find_third_option always falls
// back to this same-level candidate).
std::vector<Subject> make_fixture_subjects() {
    Subject fat;
    fat.id = 1;
    fat.characters = "太";
    fat.slug = "fat";
    fat.level = 1;
    fat.meanings = {Meaning{"Fat", true, true}};
    fat.readings = {Reading{"たい", true, true}};

    Subject dog;
    dog.id = 2;
    dog.characters = "犬";
    dog.slug = "dog";
    dog.level = 1;
    dog.meanings = {Meaning{"Dog", true, true}};
    dog.readings = {Reading{"けん", true, true}};

    Subject big;
    big.id = 3;
    big.characters = "大";
    big.slug = "big";
    big.level = 1;
    big.meanings = {Meaning{"Big", true, true}};

    return {fat, dog, big};
}

ConfusionPair make_pair(long long a_id, long long b_id) {
    ConfusionPair pair;
    pair.a_id = a_id;
    pair.b_id = b_id;
    pair.score = 1.0;
    pair.dominant_kind = EdgeKind::Component;
    pair.likely = false;
    return pair;
}

}  // namespace

// --- Levenshtein helper ---------------------------------------------

TEST_CASE("levenshtein_distance matches known distance pairs", "[drill]") {
    CHECK(levenshtein_distance("fat", "fat") == 0);
    CHECK(levenshtein_distance("fat", "fats") == 1);
    CHECK(levenshtein_distance("fat", "dog") == 3);
}

// --- matches_answer (fuzzy meaning match) ----------------------------

TEST_CASE("matches_answer accepts exact and case-insensitive matches, rejects unrelated input",
          "[drill]") {
    Subject fat;
    fat.meanings = {Meaning{"fat", true, true}};

    CHECK(matches_answer("fat", fat, std::nullopt));
    CHECK(matches_answer("Fat", fat, std::nullopt));
    CHECK(matches_answer("  fat  ", fat, std::nullopt));  // trimmed
    CHECK_FALSE(matches_answer("dog", fat, std::nullopt));
    CHECK_FALSE(matches_answer("", fat, std::nullopt));
}

TEST_CASE("matches_answer accepts a 1-edit typo for answers longer than 4 characters", "[drill]") {
    Subject friendly;
    friendly.meanings = {Meaning{"friend", true, true}};

    // "friende" is "friend" with an extra trailing letter (distance 1)
    // and is longer than 4 characters, so the typo-tolerance rule fires.
    CHECK(matches_answer("friende", friendly, std::nullopt));
    // "dog" is nowhere near edit-distance-1 of "friend" -- must still be
    // rejected even though the typo-tolerance rule is active for inputs
    // this long.
    CHECK_FALSE(matches_answer("dogdogd", friendly, std::nullopt));
}

TEST_CASE("matches_answer does NOT forgive a 1-edit typo when the input is 4 characters or shorter",
          "[drill]") {
    Subject cat;
    cat.meanings = {Meaning{"cat", true, true}};

    // "cats" is edit-distance 1 from "cat", but only 4 characters long,
    // so the length gate (> 4) must reject it rather than fuzzy-match.
    CHECK_FALSE(matches_answer("cats", cat, std::nullopt));
}

TEST_CASE("matches_answer accepts a StudyMaterial meaning synonym, case-insensitively", "[drill]") {
    Subject fat;
    fat.meanings = {Meaning{"fat", true, true}};
    wk_api::StudyMaterial material;
    material.meaning_synonyms = {"porky"};

    CHECK(matches_answer("porky", fat, material));
    CHECK(matches_answer("Porky", fat, material));
    CHECK_FALSE(matches_answer("skinny", fat, material));
}

TEST_CASE("matches_answer ignores a non-accepted-answer meaning", "[drill]") {
    Subject fat;
    fat.meanings = {Meaning{"fat", true, true}, Meaning{"obese", false, false}};

    CHECK_FALSE(matches_answer("obese", fat, std::nullopt));
}

// --- run_drill end-to-end --------------------------------------------

TEST_CASE("run_drill guards against zero pairs without crashing", "[drill]") {
    const std::string db_path = temp_db_path("empty");
    std::filesystem::remove(db_path);
    Store store(db_path);

    std::istringstream in("");
    std::ostringstream out;
    run_drill({}, make_fixture_subjects(), {}, store, kDefaultDrillQuestionCount, in, out);

    CHECK(out.str().find("Nothing to drill") != std::string::npos);
    CHECK(read_drill_results(db_path).empty());
}

TEST_CASE("run_drill scripted end-to-end: correct/incorrect feedback, score line, and drill_result rows",
          "[drill]") {
    const std::string db_path = temp_db_path("e2e");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = make_fixture_subjects();
    // Alternates forced-choice/production by question index, and
    // meaning/reading within forced-choice questions -- see drill.h.
    // Q1 forced-choice (meaning), quizzing dog (id 2) vs fat (id 1).
    // Q2 production, quizzing fat (id 1) vs dog (id 2).
    // Q3 forced-choice (reading), quizzing fat (id 1) vs dog (id 2).
    // Q4 production, quizzing dog (id 2) vs fat (id 1).
    const std::vector<ConfusionPair> pairs = {
        make_pair(2, 1),
        make_pair(1, 2),
        make_pair(1, 2),
        make_pair(2, 1),
    };

    // Q1: options [dog, fat, big], no rotation (i=0) -> dog is choice 1.
    // Q2: production on "太" -- wrong answer.
    // Q3: options [fat, dog, big] rotated left by 2 (i=2) -> [big, fat, dog];
    //     fat is choice 2, but answer directly with its character instead.
    // Q4: production on "犬" -- correct answer.
    std::istringstream in("1\nnope\n太\nDog\n");
    std::ostringstream out;

    run_drill(pairs, subjects, /*study_materials=*/{}, store, /*question_count=*/4, in, out);

    const std::string output = out.str();
    CAPTURE(output);

    // Q1 forced-choice prompt and correct feedback.
    CHECK(output.find("Which one means \"Dog\"?") != std::string::npos);
    // Q2 production prompt and incorrect feedback (shows accepted answer).
    CHECK(output.find("What does \"太\" mean?") != std::string::npos);
    CHECK(output.find("Incorrect. Accepted answer: Fat") != std::string::npos);
    // Q3 forced-choice reading prompt.
    CHECK(output.find("Which one is read \"たい\"?") != std::string::npos);
    // Q4 production prompt.
    CHECK(output.find("What does \"犬\" mean?") != std::string::npos);
    // "Correct!" appears for Q1, Q3, Q4 (3 times) -- checked via the
    // final score line below, which is the authoritative running count.
    CHECK(output.find("3/4 correct") != std::string::npos);

    const auto rows = read_drill_results(db_path);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0] == std::make_tuple(2LL, 1LL, true));   // Q1: dog correct
    CHECK(rows[1] == std::make_tuple(1LL, 2LL, false));  // Q2: fat wrong
    CHECK(rows[2] == std::make_tuple(1LL, 2LL, true));   // Q3: fat correct (via character answer)
    CHECK(rows[3] == std::make_tuple(2LL, 1LL, true));   // Q4: dog correct
}

// --- forced-choice grading of legitimately-shared answers -------------

namespace {

// Fixture for the "unanswerable by construction" case: 校 (id 11) and 高
// (id 12) share こう as their PRIMARY reading -- exactly the pair
// similarity.cpp gives a Reading edge weight 0.8, the highest reading
// weight there is, so pairs like this sort to the top of the confusion
// list and are the ones the drill reaches most often. 大 (id 13) is the
// same-level third option and is NOT read こう.
std::vector<Subject> make_shared_reading_subjects() {
    Subject school;
    school.id = 11;
    school.characters = "校";
    school.slug = "school";
    school.level = 1;
    school.meanings = {Meaning{"School", true, true}};
    school.readings = {Reading{"こう", true, true}};

    Subject tall;
    tall.id = 12;
    tall.characters = "高";
    tall.slug = "tall";
    tall.level = 1;
    tall.meanings = {Meaning{"Tall", true, true}};
    tall.readings = {Reading{"こう", true, true}};

    Subject big;
    big.id = 13;
    big.characters = "大";
    big.slug = "big";
    big.level = 1;
    big.meanings = {Meaning{"Big", true, true}};
    big.readings = {Reading{"だい", true, true}};

    return {school, tall, big};
}

}  // namespace

TEST_CASE("forced-choice reading question accepts a distractor that genuinely shares the shown reading",
          "[drill][regression]") {
    // Question order (see drill.h): Q1 forced-choice/meaning, Q2
    // production, Q3 forced-choice/reading. Q3 shows 校's primary reading
    // こう with options rotated by 2 -> [大, 校, 高], so 高 is choice 3.
    // 高 IS read こう, so choosing it must be graded correct even though
    // 校 is the option the question designated "correct".
    const std::vector<Subject> subjects = make_shared_reading_subjects();
    const std::vector<ConfusionPair> pairs = {make_pair(11, 12), make_pair(11, 12), make_pair(11, 12)};

    SECTION("choosing the reading-sharing distractor is correct") {
        const std::string db_path = temp_db_path("shared_reading_ok");
        std::filesystem::remove(db_path);
        Store store(db_path);

        std::istringstream in("1\nSchool\n3\n");
        std::ostringstream out;
        run_drill(pairs, subjects, /*study_materials=*/{}, store, /*question_count=*/3, in, out);

        const std::string output = out.str();
        CAPTURE(output);
        CHECK(output.find("Which one is read \"こう\"?") != std::string::npos);
        CHECK(output.find("3/3 correct") != std::string::npos);

        const auto rows = read_drill_results(db_path);
        REQUIRE(rows.size() == 3);
        CHECK(std::get<2>(rows[2]) == true);  // Q3 recorded as correct
    }

    SECTION("choosing an option that is NOT read that way is still wrong") {
        const std::string db_path = temp_db_path("shared_reading_wrong");
        std::filesystem::remove(db_path);
        Store store(db_path);

        // Q3 choice 1 is 大 (read だい, not こう) -- the permissiveness
        // added for genuinely-shared readings must not grade everything
        // correct.
        std::istringstream in("1\nSchool\n1\n");
        std::ostringstream out;
        run_drill(pairs, subjects, /*study_materials=*/{}, store, /*question_count=*/3, in, out);

        const std::string output = out.str();
        CAPTURE(output);
        CHECK(output.find("2/3 correct") != std::string::npos);

        const auto rows = read_drill_results(db_path);
        REQUIRE(rows.size() == 3);
        CHECK(std::get<2>(rows[2]) == false);
    }
}

TEST_CASE("forced-choice meaning question accepts a distractor that also accepts the shown meaning",
          "[drill][regression]") {
    // The meaning-axis counterpart: 太 lists "Big" as a non-primary but
    // accepted meaning, so when the question shows 大's primary meaning
    // "Big", picking 太 is a legitimate answer.
    Subject big;
    big.id = 21;
    big.characters = "大";
    big.slug = "big";
    big.level = 1;
    big.meanings = {Meaning{"Big", true, true}};

    Subject fat;
    fat.id = 22;
    fat.characters = "太";
    fat.slug = "fat";
    fat.level = 1;
    fat.meanings = {Meaning{"Fat", true, true}, Meaning{"Big", false, true}};

    Subject dog;
    dog.id = 23;
    dog.characters = "犬";
    dog.slug = "dog";
    dog.level = 1;
    dog.meanings = {Meaning{"Dog", true, true}};

    const std::string db_path = temp_db_path("shared_meaning");
    std::filesystem::remove(db_path);
    Store store(db_path);

    // Q1 is forced-choice/meaning with no rotation: options [大, 太, 犬],
    // so 太 is choice 2.
    std::istringstream in("2\n");
    std::ostringstream out;
    run_drill({make_pair(21, 22)}, {big, fat, dog}, /*study_materials=*/{}, store, /*question_count=*/1, in,
              out);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("Which one means \"Big\"?") != std::string::npos);
    CHECK(output.find("1/1 correct") != std::string::npos);

    const auto rows = read_drill_results(db_path);
    REQUIRE(rows.size() == 1);
    CHECK(std::get<2>(rows[0]) == true);
}

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

TEST_CASE("run_drill stops early on EOF mid-drill and prints a partial score", "[drill]") {
    const std::string db_path = temp_db_path("eof");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = make_fixture_subjects();
    const std::vector<ConfusionPair> pairs = {make_pair(2, 1), make_pair(1, 2)};

    // Only one answer is available; the stream runs dry before the
    // second (production) question can be answered.
    std::istringstream in("1\n");
    std::ostringstream out;

    run_drill(pairs, subjects, /*study_materials=*/{}, store, /*question_count=*/2, in, out);

    const std::string output = out.str();
    CAPTURE(output);
    // Exactly one question was completed -- the score line's denominator
    // must reflect that, not the requested question_count of 2.
    CHECK(output.find("/1 correct") != std::string::npos);

    // Only the one completed question wrote a drill_result row; the
    // aborted second question must not have written a partial/garbage row.
    CHECK(read_drill_results(db_path).size() == 1);
}

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
