#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    if (v != nullptr && v[0] != '\0') {
        return std::string(v);
    }
    return fallback;
}

std::string home_dir() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::string(home);
    }
    throw std::runtime_error("HOME environment variable is not set");
}

std::string resolve_config_dir() {
    std::string base = env_or("XDG_CONFIG_HOME", home_dir() + "/.config");
    return base + "/wanikani-reinforcer";
}

std::string resolve_data_dir() {
    std::string base = env_or("XDG_DATA_HOME", home_dir() + "/.local/share");
    return base + "/wanikani-reinforcer";
}

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto start = s.find_first_not_of(ws);
    if (start == std::string::npos) {
        return "";
    }
    const auto end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

// Returns the token read from the token file, or empty string if the file
// does not exist. Warns on stderr (never including the token) if the file
// mode is not 0600.
std::string read_token_file(const std::string& config_dir) {
    fs::path token_path = fs::path(config_dir) / "token";
    std::error_code ec;
    if (!fs::exists(token_path, ec) || ec) {
        return "";
    }

    fs::perms perms = fs::status(token_path, ec).permissions();
    if (!ec) {
        fs::perms expected = fs::perms::owner_read | fs::perms::owner_write;
        if ((perms & fs::perms::mask) != expected) {
            std::cerr << "warning: token file mode is not 0600" << std::endl;
        }
    }

    std::ifstream file(token_path);
    if (!file) {
        return "";
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return trim(contents.str());
}

// Resolves the token and paths from scratch: reads the token file (and
// its permission mode) and creates the data dir. Config::load() below
// caches the result so this only runs once per process — otherwise every
// http::get() call (which resolves its own token) would re-read the
// token file and re-print the 0600 warning.
Config resolve() {
    Config config;

    std::string token = env_or("WANIKANI_API_TOKEN", "");
    std::string config_dir = resolve_config_dir();
    if (token.empty()) {
        token = read_token_file(config_dir);
    }
    if (token.empty()) {
        throw std::runtime_error(
            "no WaniKani API token found: set WANIKANI_API_TOKEN or create "
            + config_dir + "/token");
    }

    config.api_token = token;
    config.data_dir = resolve_data_dir();
    std::filesystem::create_directories(config.data_dir);
    config.db_path = config.data_dir + "/wkr.db";

    return config;
}

}  // namespace

Config Config::load() {
    static const Config cached = resolve();
    return cached;
}
