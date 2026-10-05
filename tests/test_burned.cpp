#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>

#include "burned.h"
#include "model.h"
#include "store.h"

namespace {

const std::string kNow = "2026-08-16T00:00:00Z";

std::string temp_db_path(const std::string& label) {
    static int counter = 0;
    std::ostringstream name;
    name << "wkr_burned_test_" << label << "_" << (counter++) << ".db";
    return (std::filesystem::temp_directory_path() / name.str()).string();
}

Assignment make_assignment(long long subject_id, int stage) {
    Assignment a;
    a.id = subject_id * 10;
    a.subject_id = subject_id;
    a.srs_stage = stage;
    return a;
}

BurnedSrsState make_state(long long subject_id, int stage, const std::string& due_at) {
    return BurnedSrsState{subject_id, stage, due_at, "2026-07-01T00:00:00Z"};
}

Subject make_subject(long long id, const std::string& characters, const std::string& meaning) {
    Subject s;
    s.id = id;
    s.type = "vocabulary";
    s.characters = characters;
    s.slug = meaning;
    s.meanings = {Meaning{meaning, true, true}};
    return s;
}

}  // namespace

TEST_CASE("a first correct burned review lands on stage 1 due in 30 days", "[burned]") {
    const BurnedSrsState next = advance_burned_srs(std::nullopt, 5, true, kNow);

    CHECK(next.subject_id == 5);
    CHECK(next.stage == 1);
    CHECK(next.due_at == "2026-09-15T00:00:00Z");
    CHECK(next.last_reviewed_at == kNow);
}

TEST_CASE("a miss sends a burned item back to stage 0 due in one day", "[burned]") {
    const BurnedSrsState next =
        advance_burned_srs(make_state(5, 3, "2026-08-01T00:00:00Z"), 5, false, kNow);

    CHECK(next.stage == 0);
    CHECK(next.due_at == "2026-08-17T00:00:00Z");
}

TEST_CASE("correct answers climb 30d, 60d, 120d, 240d and then stay at the top stage", "[burned]") {
    BurnedSrsState state = advance_burned_srs(std::nullopt, 5, true, kNow);
    CHECK(state.due_at == "2026-09-15T00:00:00Z");
    state = advance_burned_srs(state, 5, true, kNow);
    CHECK(state.stage == 2);
    CHECK(state.due_at == "2026-10-15T00:00:00Z");
    state = advance_burned_srs(state, 5, true, kNow);
    CHECK(state.stage == 3);
    CHECK(state.due_at == "2026-12-14T00:00:00Z");
    state = advance_burned_srs(state, 5, true, kNow);
    CHECK(state.stage == kMaxBurnedStage);
    CHECK(state.due_at == "2027-04-13T00:00:00Z");
    state = advance_burned_srs(state, 5, true, kNow);
    CHECK(state.stage == kMaxBurnedStage);
}

TEST_CASE("a correct answer after a miss climbs from stage 0 to stage 1", "[burned]") {
    const BurnedSrsState next =
        advance_burned_srs(make_state(5, 0, "2026-08-10T00:00:00Z"), 5, true, kNow);

    CHECK(next.stage == 1);
}

TEST_CASE("select_due_burned only considers WaniKani-burned assignments", "[burned]") {
    const std::vector<Assignment> assignments = {make_assignment(1, 8), make_assignment(2, 9),
                                                 make_assignment(3, 9), make_assignment(4, 0)};

    const std::vector<long long> due = select_due_burned(assignments, {}, kNow);

    REQUIRE(due.size() == 2);
    CHECK(due[0] == 2);
    CHECK(due[1] == 3);
}

TEST_CASE("select_due_burned skips future-due items and lists overdue before unseen", "[burned]") {
    const std::vector<Assignment> assignments = {make_assignment(1, 9), make_assignment(2, 9),
                                                 make_assignment(3, 9), make_assignment(4, 9)};
    const std::vector<BurnedSrsState> states = {
        make_state(2, 1, "2026-08-10T00:00:00Z"),
        make_state(3, 2, "2026-09-10T00:00:00Z"),
        make_state(4, 0, "2026-08-01T00:00:00Z"),
    };

    const std::vector<long long> due = select_due_burned(assignments, states, kNow);

    REQUIRE(due.size() == 3);
    CHECK(due[0] == 4);
    CHECK(due[1] == 2);
    CHECK(due[2] == 1);
}

TEST_CASE("select_due_burned lists a subject once even with duplicate assignments", "[burned]") {
    const std::vector<Assignment> assignments = {make_assignment(1, 9), make_assignment(1, 9)};

    CHECK(select_due_burned(assignments, {}, kNow).size() == 1);
}

TEST_CASE("burned_nothing_due_message distinguishes no burned items from none due", "[burned]") {
    CHECK(burned_nothing_due_message({make_assignment(1, 8)}, {}, kNow) ==
          "No burned items yet -- keep going on WaniKani.");

    CHECK(burned_nothing_due_message({make_assignment(1, 9)},
                                     {make_state(1, 1, "2026-08-16T03:20:00Z")}, kNow) ==
          "Nothing due. Next burned review in 3h 20m.");
}

TEST_CASE("run_burned_review asks the meaning, grades it, and schedules the subject", "[burned]") {
    const std::string db_path = temp_db_path("run");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = {make_subject(1, "犬", "Dog"), make_subject(2, "太", "Fat")};

    std::istringstream in("  dog \nnope\n");
    std::ostringstream out;
    run_burned_review({1, 2}, subjects, {}, store, 5, in, out, false, kNow);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("What does \"犬\" mean?") != std::string::npos);
    CHECK(output.find("Correct!") != std::string::npos);
    CHECK(output.find("Incorrect. Accepted answer: Fat") != std::string::npos);
    CHECK(output.find("1/2 correct") != std::string::npos);

    const std::vector<BurnedSrsState> states = store.all_burned_srs();
    REQUIRE(states.size() == 2);
    CHECK(states[0].subject_id == 1);
    CHECK(states[0].stage == 1);
    CHECK(states[1].subject_id == 2);
    CHECK(states[1].stage == 0);
}

TEST_CASE("run_burned_review stops at the question count and on EOF without scheduling", "[burned]") {
    const std::string db_path = temp_db_path("limit");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = {make_subject(1, "犬", "Dog"), make_subject(2, "太", "Fat"),
                                           make_subject(3, "大", "Big")};

    std::istringstream limited_in("dog\nfat\nbig\n");
    std::ostringstream limited_out;
    run_burned_review({1, 2, 3}, subjects, {}, store, 1, limited_in, limited_out, false, kNow);
    CHECK(store.all_burned_srs().size() == 1);

    std::istringstream eof_in("");
    std::ostringstream eof_out;
    run_burned_review({2}, subjects, {}, store, 5, eof_in, eof_out, false, kNow);
    CHECK(store.all_burned_srs().size() == 1);
}
