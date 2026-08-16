#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>

#include "model.h"
#include "store.h"

namespace {

// Returns a fresh temp db path per test invocation so tests don't
// interfere with each other or leave state behind.
std::string temp_db_path(const std::string& label) {
    static int counter = 0;
    std::ostringstream name;
    name << "wkr_test_" << label << "_" << (counter++) << ".db";
    return (std::filesystem::temp_directory_path() / name.str()).string();
}

Subject make_subject(long long id, const std::string& data_updated_at) {
    Subject subject;
    subject.id = id;
    subject.type = "kanji";
    subject.characters = "水";
    subject.slug = "water";
    subject.level = 1;
    subject.meanings = {Meaning{"Water", true, true}};
    subject.data_updated_at = data_updated_at;
    return subject;
}

}  // namespace

TEST_CASE("upsert_subject is idempotent", "[store]") {
    const std::string db_path = temp_db_path("idempotent");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject subject = make_subject(42, "2020-01-01T00:00:00.000000Z");

    store.upsert_subject(subject);
    store.upsert_subject(subject);
    store.upsert_subject(subject);

    REQUIRE(store.subject_count() == 1);
}

TEST_CASE("upsert_subject replaces the row on re-sync with updated data", "[store]") {
    const std::string db_path = temp_db_path("replace");
    std::filesystem::remove(db_path);
    Store store(db_path);

    Subject subject = make_subject(7, "2020-01-01T00:00:00.000000Z");
    store.upsert_subject(subject);

    subject.characters = "火";
    subject.data_updated_at = "2021-06-15T00:00:00.000000Z";
    store.upsert_subject(subject);

    REQUIRE(store.subject_count() == 1);
}

TEST_CASE("sync_meta get/set round-trips and is absent by default", "[store]") {
    const std::string db_path = temp_db_path("meta");
    std::filesystem::remove(db_path);
    Store store(db_path);

    REQUIRE_FALSE(store.get_meta("subjects_cursor").has_value());

    store.set_meta("subjects_cursor", "2020-01-01T00:00:00.000000Z");
    auto value = store.get_meta("subjects_cursor");
    REQUIRE(value.has_value());
    REQUIRE(*value == "2020-01-01T00:00:00.000000Z");

    store.set_meta("subjects_cursor", "2021-01-01T00:00:00.000000Z");
    value = store.get_meta("subjects_cursor");
    REQUIRE(*value == "2021-01-01T00:00:00.000000Z");
}
