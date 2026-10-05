#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "config.h"
#include "store.h"

// Covers the split between path resolution and token resolution:
// `wkr report` and `wkr drill` are purely local (README: "they only read
// from the SQLite database `wkr sync` populated") and must work with no
// API token configured at all, while `wkr sync` — which does make HTTP
// calls — must still fail with a clear message when there is no token.
//
// These use Config::resolve() rather than Config::load() on purpose:
// load() caches its result for the whole process, so it can only observe
// one environment per test binary.

namespace {

// Saves an environment variable's value on construction and restores it
// (including "was unset") on destruction, so each test can point
// XDG_CONFIG_HOME/XDG_DATA_HOME/WANIKANI_API_TOKEN somewhere temporary
// without leaking that into the rest of the suite.
class ScopedEnv {
public:
    ScopedEnv(std::string name, const std::optional<std::string>& value) : name_(std::move(name)) {
        const char* existing = std::getenv(name_.c_str());
        if (existing != nullptr) {
            previous_ = std::string(existing);
        }
        apply(value);
    }
    ~ScopedEnv() { apply(previous_); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const std::optional<std::string>& value) {
        if (value.has_value()) {
            setenv(name_.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    std::string name_;
    std::optional<std::string> previous_;
};

std::string temp_dir(const std::string& label) {
    static int counter = 0;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("wkr_config_test_" + label + "_" + std::to_string(counter++));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path.string();
}

}  // namespace

TEST_CASE("Config::resolve succeeds with no token configured and still yields a usable db path",
          "[config]") {
    const std::string config_home = temp_dir("noconfig");
    const std::string data_home = temp_dir("nodata");
    const ScopedEnv no_token("WANIKANI_API_TOKEN", std::nullopt);
    const ScopedEnv config_env("XDG_CONFIG_HOME", config_home);
    const ScopedEnv data_env("XDG_DATA_HOME", data_home);

    const Config config = Config::resolve();

    CHECK_FALSE(config.has_token());
    CHECK(config.api_token.empty());
    CHECK(config.data_dir == data_home + "/wanikani-reinforcer");
    CHECK(config.db_path == data_home + "/wanikani-reinforcer/wkr.db");
    CHECK(std::filesystem::is_directory(config.data_dir));

    // The `report`/`drill` data-loading path: open the store at the
    // resolved path and read from it. This is everything those two
    // commands do before printing, and none of it needs a token.
    Store store(config.db_path);
    CHECK(store.all_subjects().empty());
    CHECK(store.latest_stat_per_subject().empty());
    CHECK(store.all_similarity_edges().empty());
}

TEST_CASE("Config::require_token throws a clear, token-free error when no token is configured",
          "[config]") {
    const std::string config_home = temp_dir("requires");
    const std::string data_home = temp_dir("requires_data");
    const ScopedEnv no_token("WANIKANI_API_TOKEN", std::nullopt);
    const ScopedEnv config_env("XDG_CONFIG_HOME", config_home);
    const ScopedEnv data_env("XDG_DATA_HOME", data_home);

    const Config config = Config::resolve();

    // This is the failure `wkr sync` (and only `wkr sync`) hits: it calls
    // require_token() up front, and http.cpp's cached_token() calls it
    // again for every request.
    REQUIRE_THROWS_AS(config.require_token(), std::runtime_error);
    try {
        config.require_token();
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        CHECK(message.find("no WaniKani API token found") != std::string::npos);
        CHECK(message.find(config_home + "/wanikani-reinforcer/token") != std::string::npos);
    }
}

TEST_CASE("Config::resolve reads the token from the environment and from the token file", "[config]") {
    const std::string config_home = temp_dir("withtoken");
    const std::string data_home = temp_dir("withtoken_data");

    SECTION("from WANIKANI_API_TOKEN") {
        const ScopedEnv token_env("WANIKANI_API_TOKEN", std::string("env-token"));
        const ScopedEnv config_env("XDG_CONFIG_HOME", config_home);
        const ScopedEnv data_env("XDG_DATA_HOME", data_home);

        const Config config = Config::resolve();
        CHECK(config.has_token());
        CHECK(config.require_token() == "env-token");
    }

    SECTION("from the token file when the environment variable is unset") {
        const ScopedEnv no_token("WANIKANI_API_TOKEN", std::nullopt);
        const ScopedEnv config_env("XDG_CONFIG_HOME", config_home);
        const ScopedEnv data_env("XDG_DATA_HOME", data_home);

        const std::filesystem::path dir = std::filesystem::path(config_home) / "wanikani-reinforcer";
        std::filesystem::create_directories(dir);
        {
            std::ofstream out(dir / "token");
            out << "file-token\n";
        }
        std::filesystem::permissions(dir / "token",
                                      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                      std::filesystem::perm_options::replace);

        const Config config = Config::resolve();
        CHECK(config.has_token());
        CHECK(config.require_token() == "file-token");
    }
}
