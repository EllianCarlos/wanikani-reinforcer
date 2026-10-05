#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>

#include "burned.h"
#include "stream_runner.h"
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

TEST_CASE("select_due_burned honours the included subject types", "[burned]") {
    Assignment radical = make_assignment(1, 9);
    radical.subject_type = "radical";
    Assignment kanji = make_assignment(2, 9);
    kanji.subject_type = "kanji";
    Assignment vocab = make_assignment(3, 9);
    vocab.subject_type = "vocabulary";
    Assignment kana = make_assignment(4, 9);
    kana.subject_type = "kana_vocabulary";
    const std::vector<Assignment> assignments = {radical, kanji, vocab, kana};

    CHECK(select_due_burned(assignments, {}, kNow).size() == 4);
    CHECK(select_due_burned(assignments, {}, kNow, {"kanji"}) == std::vector<long long>{2});
    CHECK(select_due_burned(assignments, {}, kNow, {"radical", "vocabulary"}) ==
          std::vector<long long>{1, 3});
}

TEST_CASE("burned_nothing_due_message names the type filter when nothing matches", "[burned]") {
    Assignment kanji = make_assignment(2, 9);
    kanji.subject_type = "kanji";

    CHECK(burned_nothing_due_message({kanji}, {}, kNow, {"radical"}) ==
          "No burned items of the selected types yet -- keep going on WaniKani.");
}

TEST_CASE("resolve_burned_types defaults to every type", "[burned]") {
    const std::set<std::string> all = resolve_burned_types("", "");

    CHECK(all == std::set<std::string>{"radical", "kanji", "vocabulary", "kana_vocabulary"});
}

TEST_CASE("resolve_burned_types applies --only and --exclude, with vocab covering kana vocabulary",
          "[burned]") {
    CHECK(resolve_burned_types("kanji,radical", "") == std::set<std::string>{"kanji", "radical"});
    CHECK(resolve_burned_types("", "kanji,radical") ==
          std::set<std::string>{"vocabulary", "kana_vocabulary"});
    CHECK(resolve_burned_types("", "vocab") == std::set<std::string>{"radical", "kanji"});
    CHECK(resolve_burned_types("vocab,kanji", "kanji") ==
          std::set<std::string>{"vocabulary", "kana_vocabulary"});
    CHECK(resolve_burned_types(" Kanji , RADICAL ", "") == std::set<std::string>{"kanji", "radical"});
}

TEST_CASE("resolve_burned_types rejects unknown names and an empty result", "[burned]") {
    CHECK_THROWS_AS(resolve_burned_types("kanjis", ""), std::runtime_error);
    CHECK_THROWS_AS(resolve_burned_types("", "grammar"), std::runtime_error);
    CHECK_THROWS_AS(resolve_burned_types("kanji", "kanji"), std::runtime_error);
}

namespace {

Subject make_kanji(long long id, const std::string& characters, const std::string& meaning,
                   const std::string& reading) {
    Subject s = make_subject(id, characters, meaning);
    s.type = "kanji";
    s.readings = {Reading{reading, true, true}};
    return s;
}

}  // namespace

TEST_CASE("matches_reading accepts kana, katakana and romaji for an accepted reading", "[burned]") {
    const Subject dog = make_kanji(1, "犬", "Dog", "いぬ");

    CHECK(matches_reading("いぬ", dog));
    CHECK(matches_reading("イヌ", dog));
    CHECK(matches_reading("inu", dog));
    CHECK(matches_reading("  Inu ", dog));
    CHECK_FALSE(matches_reading("neko", dog));
    CHECK_FALSE(matches_reading("", dog));
}

TEST_CASE("matches_reading ignores readings that are not accepted answers", "[burned]") {
    Subject s = make_kanji(1, "日", "Day", "にち");
    s.readings.push_back(Reading{"ひ", false, false});

    CHECK(matches_reading("nichi", s));
    CHECK_FALSE(matches_reading("hi", s));
}

TEST_CASE("run_burned_review asks the meaning and then the reading of each item, in that order",
          "[burned]") {
    const std::string db_path = temp_db_path("reading_order");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = {make_kanji(1, "犬", "Dog", "いぬ"),
                                           make_kanji(2, "太", "Fat", "ふとい")};

    std::istringstream in("dog\ninu\nfat\nfutoi\n");
    std::ostringstream out;
    run_burned_review({1, 2}, subjects, {}, store, 5, in, out, false, kNow);

    const std::string output = out.str();
    CAPTURE(output);
    const size_t dog_meaning = output.find("What does \"犬\" mean?");
    const size_t dog_reading = output.find("What is the reading of \"犬\"?");
    const size_t fat_meaning = output.find("What does \"太\" mean?");
    const size_t fat_reading = output.find("What is the reading of \"太\"?");
    REQUIRE(dog_meaning != std::string::npos);
    REQUIRE(dog_reading != std::string::npos);
    REQUIRE(fat_meaning != std::string::npos);
    REQUIRE(fat_reading != std::string::npos);
    CHECK(dog_meaning < dog_reading);
    CHECK(dog_reading < fat_meaning);
    CHECK(fat_meaning < fat_reading);
    CHECK(output.find("2/2 correct") != std::string::npos);
}

TEST_CASE("a wrong reading fails the item even when the meaning was right", "[burned]") {
    const std::string db_path = temp_db_path("reading_wrong");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = {make_kanji(1, "犬", "Dog", "いぬ")};

    std::istringstream in("dog\nneko\n");
    std::ostringstream out;
    run_burned_review({1}, subjects, {}, store, 5, in, out, false, kNow);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("Incorrect. Accepted reading: いぬ") != std::string::npos);
    CHECK(output.find("0/1 correct") != std::string::npos);

    const std::vector<BurnedSrsState> states = store.all_burned_srs();
    REQUIRE(states.size() == 1);
    CHECK(states[0].stage == 0);
}

TEST_CASE("an item without readings gets only the meaning question", "[burned]") {
    const std::string db_path = temp_db_path("reading_none");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject radical = make_subject(1, "一", "Ground");
    radical.type = "radical";

    std::istringstream in("ground\n");
    std::ostringstream out;
    run_burned_review({1}, {radical}, {}, store, 5, in, out, false, kNow);

    CHECK(out.str().find("reading") == std::string::npos);
    CHECK(out.str().find("1/1 correct") != std::string::npos);
}

TEST_CASE("EOF before the reading answer leaves the item unscheduled", "[burned]") {
    const std::string db_path = temp_db_path("reading_eof");
    std::filesystem::remove(db_path);
    Store store(db_path);

    const std::vector<Subject> subjects = {make_kanji(1, "犬", "Dog", "いぬ")};

    std::istringstream in("dog\n");
    std::ostringstream out;
    run_burned_review({1}, subjects, {}, store, 5, in, out, false, kNow);

    CHECK(store.all_burned_srs().empty());
}

namespace {

Subject make_child_kanji() {
    Subject s = make_subject(462, "子", "Child");
    s.type = "kanji";
    s.readings = {Reading{"し", true, true}, Reading{"す", false, true}, Reading{"こ", false, false}};
    return s;
}

}  // namespace

TEST_CASE("is_unaccepted_reading recognises a real reading WaniKani does not accept", "[burned]") {
    const Subject child = make_child_kanji();

    CHECK(is_unaccepted_reading("こ", child));
    CHECK(is_unaccepted_reading("ko", child));
    CHECK_FALSE(is_unaccepted_reading("し", child));
    CHECK_FALSE(is_unaccepted_reading("neko", child));
    CHECK_FALSE(is_unaccepted_reading("", child));
}

TEST_CASE("a non-accepted reading is retried without counting as a miss", "[burned]") {
    const std::string db_path = temp_db_path("retry");
    std::filesystem::remove(db_path);
    Store store(db_path);

    std::istringstream in("child\nこ\nshi\n");
    std::ostringstream out;
    run_burned_review({462}, {make_child_kanji()}, {}, store, 5, in, out, false, kNow);

    const std::string output = out.str();
    CAPTURE(output);
    const size_t first_ask = output.find("What is the reading of \"子\"?");
    REQUIRE(first_ask != std::string::npos);
    CHECK(output.find("What is the reading of \"子\"?", first_ask + 1) != std::string::npos);
    CHECK(output.find("isn't the one WaniKani wants for this kanji") != std::string::npos);
    CHECK(output.find("Incorrect. Accepted reading") == std::string::npos);
    CHECK(output.find("1/1 correct") != std::string::npos);

    const std::vector<BurnedSrsState> states = store.all_burned_srs();
    REQUIRE(states.size() == 1);
    CHECK(states[0].stage == 1);
}

TEST_CASE("a wrong reading after a retry still fails the item", "[burned]") {
    const std::string db_path = temp_db_path("retry_wrong");
    std::filesystem::remove(db_path);
    Store store(db_path);

    std::istringstream in("child\nこ\nneko\n");
    std::ostringstream out;
    run_burned_review({462}, {make_child_kanji()}, {}, store, 5, in, out, false, kNow);

    CHECK(out.str().find("Incorrect. Accepted readings: し, す") != std::string::npos);
    CHECK(out.str().find("0/1 correct") != std::string::npos);
}

TEST_CASE("EOF during a reading retry leaves the item unscheduled", "[burned]") {
    const std::string db_path = temp_db_path("retry_eof");
    std::filesystem::remove(db_path);
    Store store(db_path);

    std::istringstream in("child\nこ\n");
    std::ostringstream out;
    run_burned_review({462}, {make_child_kanji()}, {}, store, 5, in, out, false, kNow);

    CHECK(store.all_burned_srs().empty());
}

TEST_CASE("every burned prompt names the subject type", "[burned]") {
    const std::string db_path = temp_db_path("type_label");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject radical = make_subject(34, "子", "Child");
    radical.type = "radical";
    Subject kanji = make_child_kanji();
    Subject vocab = make_subject(2513, "子", "Child");
    vocab.type = "vocabulary";
    vocab.readings = {Reading{"こ", true, true}};
    Subject kana = make_subject(9, "ありがとう", "Thank You");
    kana.type = "kana_vocabulary";

    std::istringstream in("child\nchild\nshi\nchild\nko\nthank you\n");
    std::ostringstream out;
    run_burned_review({34, 462, 2513, 9}, {radical, kanji, vocab, kana}, {}, store, 5, in, out, false,
                      kNow);

    const std::string output = out.str();
    CAPTURE(output);
    CHECK(output.find("[Radical] What does \"子\" mean?") != std::string::npos);
    CHECK(output.find("[Kanji] What does \"子\" mean?") != std::string::npos);
    CHECK(output.find("[Kanji] What is the reading of \"子\"?") != std::string::npos);
    CHECK(output.find("[Vocabulary] What does \"子\" mean?") != std::string::npos);
    CHECK(output.find("[Vocabulary] What is the reading of \"子\"?") != std::string::npos);
    CHECK(output.find("[Vocabulary] What does \"ありがとう\" mean?") != std::string::npos);
}
