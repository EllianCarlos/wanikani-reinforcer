#pragma once

#include <string>

// Resolves runtime configuration: the WaniKani API token, and the
// filesystem paths wkr reads/writes. Never logs or exposes the token
// beyond returning it via `require_token()`.
//
// Path resolution and token resolution are deliberately independent:
// `wkr report` and `wkr drill` are purely local commands (they only read
// the SQLite database `wkr sync` populated) and must work with no token
// configured at all, so a missing token is NOT an error at load time --
// only asking for the token itself is.
struct Config {
    // Empty when no token is configured. Callers that actually need a
    // token (only http.cpp's request path, plus `wkr sync`'s up-front
    // check) must go through require_token(), never read this directly.
    std::string api_token;
    std::string config_dir;  // directory the token file lives in
    std::string data_dir;    // directory containing wkr.db
    std::string db_path;     // <data_dir>/wkr.db

    // Resolves the token (env var, then config file), the data dir
    // (creating it if missing), and the db path. Caches the result for
    // the process, so the token file is read (and its 0600 mode checked)
    // only once. Never throws because a token is missing -- see
    // require_token().
    static Config load();

    // The same resolution load() performs, without the process-wide
    // cache. Exists so tests can resolve against a temporary
    // XDG_CONFIG_HOME/XDG_DATA_HOME more than once in a single process.
    static Config resolve();

    bool has_token() const { return !api_token.empty(); }

    // Returns the token, or throws std::runtime_error with a clear
    // message naming both ways to configure one if none was found. This
    // is the single place the "no token" failure is raised.
    const std::string& require_token() const;
};
