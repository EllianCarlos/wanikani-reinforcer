#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "model.h"
#include "rpc.h"
#include "store.h"
#include "sync.h"

// Drives RpcServer through its line protocol against a seeded fixture DB:
// the same synced 太/犬 data test_integration.cpp uses, with no network.

namespace {

using nlohmann::json;

constexpr const char* kNow = "2026-08-16T00:00:00Z";

std::string temp_db_path() {
    static int counter = 0;
    std::ostringstream name;
    name << "wkr_rpc_test_" << (counter++) << ".db";
    return (std::filesystem::temp_directory_path() / name.str()).string();
}

std::vector<Subject> fixture_subjects() {
    Subject big;
    big.id = 10;
    big.type = "radical";
    big.characters = "大";
    big.slug = "big";
    big.level = 1;
    big.meanings = {Meaning{"Big", true, true}};
    big.data_updated_at = "2026-08-01T00:00:00Z";

    Subject fat;
    fat.id = 1;
    fat.type = "kanji";
    fat.characters = "太";
    fat.slug = "fat";
    fat.level = 1;
    fat.meanings = {Meaning{"Fat", true, true}};
    fat.readings = {Reading{"たい", true, true}};
    fat.component_subject_ids = {10};
    fat.data_updated_at = "2026-08-01T00:00:00Z";

    Subject dog;
    dog.id = 2;
    dog.type = "kanji";
    dog.characters = "犬";
    dog.slug = "dog";
    dog.level = 1;
    dog.meanings = {Meaning{"Dog", true, true}};
    dog.readings = {Reading{"けん", true, true}};
    dog.component_subject_ids = {10};
    dog.data_updated_at = "2026-08-01T00:00:00Z";
    return {big, fat, dog};
}

std::vector<ReviewStat> fixture_stats() {
    std::vector<ReviewStat> out;
    for (long long id : {1LL, 2LL}) {
        ReviewStat s;
        s.subject_id = id;
        s.subject_type = "kanji";
        s.meaning_correct = 2;
        s.meaning_incorrect = 3;
        s.meaning_current_streak = 0;
        s.reading_current_streak = 5;
        s.percentage_correct = 40;
        s.data_updated_at = "2026-08-15T10:00:00.000000Z";
        out.push_back(s);
    }
    return out;
}

struct Fixture {
    std::string db_path = temp_db_path();
    std::unique_ptr<Store> store;
    int sync_calls = 0;
    std::unique_ptr<RpcServer> server;

    Fixture() {
        std::filesystem::remove(db_path);
        store = std::make_unique<Store>(db_path);
        SyncFetchers fetchers;
        const auto subjects = fixture_subjects();
        const auto stats = fixture_stats();
        fetchers.subjects = [subjects](const std::string&) { return subjects; };
        fetchers.review_statistics = [stats](const std::string&) { return stats; };
        fetchers.assignments = [](const std::string&) { return std::vector<Assignment>{}; };
        fetchers.study_materials = [](const std::string&) { return std::vector<wk_api::StudyMaterial>{}; };
        run_sync(*store, fetchers, 45);

        server = std::make_unique<RpcServer>(
            *store,
            [this](int) {
                ++sync_calls;
                SyncSummary s;
                s.subjects = 3;
                return s;
            },
            [] { return std::string(kNow); });
    }

    int next_id = 1;
    json call(const std::string& method, json params = json::object()) {
        const json req = {{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", method}, {"params", params}};
        return json::parse(server->handle_line(req.dump()));
    }
    void make_burned(long long id) {
        Assignment a;
        a.id = id * 10;
        a.subject_id = id;
        a.subject_type = "kanji";
        a.srs_stage = kWkBurnedStage;
        store->upsert_assignment(a);
    }
};

}  // namespace

TEST_CASE("rpc: protocol errors", "[rpc]") {
    Fixture f;
    CHECK(json::parse(f.server->handle_line("{not json"))["error"]["code"] == -32700);
    CHECK(json::parse(f.server->handle_line("[1,2]"))["error"]["code"] == -32600);
    CHECK(f.call("nope")["error"]["code"] == -32601);
    CHECK(f.call("session.next")["error"]["code"] == -32000);
    CHECK(f.call("session.answer", {{"answer", "x"}})["error"]["code"] == -32000);
    CHECK(f.call("session.start", {{"kind", "bogus"}})["error"]["code"] == -32602);
    // Notifications (no id) get no response, even when they would fail.
    CHECK(f.server->handle_line(R"({"jsonrpc":"2.0","method":"nope"})").empty());
}

TEST_CASE("rpc: serve reads lines until EOF and skips blanks", "[rpc]") {
    Fixture f;
    std::istringstream in("\n{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"kana.convert\",\"params\":{\"text\":\"tai\"}}\n");
    std::ostringstream out;
    f.server->serve(in, out);
    const json r = json::parse(out.str());
    CHECK(r["id"] == 7);
    CHECK(r["result"]["hiragana"] == "たい");
}

TEST_CASE("rpc: dashboard.get reports leeches and due counts", "[rpc]") {
    Fixture f;
    const json r = f.call("dashboard.get")["result"];
    CHECK(r["has_data"] == true);
    CHECK(r["leeches"].size() == 2);
    CHECK(r["due"]["pairs"] == 1);
    CHECK(r["drill"]["total"] == 0);
}

TEST_CASE("rpc: sync.run calls the injected sync", "[rpc]") {
    Fixture f;
    const json r = f.call("sync.run", {{"session_gap_minutes", 30}})["result"];
    CHECK(f.sync_calls == 1);
    CHECK(r["subjects"] == 3);
}

TEST_CASE("rpc: a drill session persists answers and then has nothing due", "[rpc]") {
    Fixture f;
    const json start = f.call("session.start", {{"kind", "drill"}, {"count", 5}})["result"];
    REQUIRE(start["total"] == 1);

    const json next = f.call("session.next")["result"];
    REQUIRE(next["done"] == false);
    CHECK(next["question"]["kind"] == "forced_choice");
    CHECK(next["question"]["options"].size() >= 2);
    // Asking again before answering repeats the same question.
    CHECK(f.call("session.next")["result"]["question"] == next["question"]);

    const json fb = f.call("session.answer", {{"answer", "1"}})["result"];
    CHECK(fb.contains("correct"));
    CHECK(f.store->all_drill_results().size() == 1);
    CHECK(f.store->all_pair_srs().size() == 1);

    const json done = f.call("session.next")["result"];
    CHECK(done["done"] == true);
    CHECK(done["asked"] == 1);

    const json again = f.call("session.start", {{"kind", "drill"}})["result"];
    CHECK(again.contains("nothing_due"));
}

TEST_CASE("rpc: aborting keeps what was already answered", "[rpc]") {
    Fixture f;
    f.call("session.start", {{"kind", "drill"}});
    f.call("session.next");
    f.call("session.answer", {{"answer", "1"}});
    const json r = f.call("session.abort")["result"];
    CHECK(r["asked"] == 1);
    CHECK(f.store->all_drill_results().size() == 1);
    CHECK(f.call("session.next")["error"]["code"] == -32000);
}

TEST_CASE("rpc: a burned session asks meaning then reading with romaji input", "[rpc]") {
    Fixture f;
    f.make_burned(1);
    const json start = f.call("session.start", {{"kind", "burned"}, {"count", 5}})["result"];
    REQUIRE(start["total"] == 1);

    const json meaning = f.call("session.next")["result"]["question"];
    CHECK(meaning["kind"] == "meaning");
    const json m = f.call("session.answer", {{"answer", "fat"}})["result"];
    CHECK(m["correct"] == true);
    CHECK(f.store->all_burned_srs().empty());  // written only after the last question

    const json reading = f.call("session.next")["result"]["question"];
    CHECK(reading["kind"] == "reading");
    const json r = f.call("session.answer", {{"answer", "tai"}})["result"];  // romaji
    CHECK(r["correct"] == true);
    CHECK(f.store->all_burned_srs().size() == 1);

    const json done = f.call("session.next")["result"];
    CHECK(done["done"] == true);
    CHECK(done["correct"] == 1);
}

TEST_CASE("rpc: burned type filters that leave nothing are invalid params", "[rpc]") {
    Fixture f;
    f.make_burned(1);
    const json r = f.call("session.start", {{"kind", "burned"}, {"only", "kanji"}, {"exclude", "kanji"}});
    CHECK(r["error"]["code"] == -32602);
    const json none = f.call("session.start", {{"kind", "burned"}, {"only", "radical"}})["result"];
    CHECK(none.contains("nothing_due"));
}
