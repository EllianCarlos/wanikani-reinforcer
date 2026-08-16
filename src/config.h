#pragma once

#include <string>

// Resolves runtime configuration: the WaniKani API token, and the
// filesystem paths wkr reads/writes. Never logs or exposes the token
// beyond returning it via `token()`.
struct Config {
    std::string api_token;
    std::string data_dir;  // directory containing wkr.db
    std::string db_path;   // <data_dir>/wkr.db

    // Resolves the token (env var, then config file), the data dir
    // (creating it if missing), and the db path. Throws std::runtime_error
    // with a clear message if no token can be found.
    static Config load();
};
