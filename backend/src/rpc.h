#pragma once

#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <string>

#include <nlohmann/json.hpp>

#include "app.h"
#include "burned.h"
#include "drill.h"
#include "store.h"
#include "sync.h"

// Line-delimited JSON-RPC 2.0 server driving the dashboard, sync and quiz
// sessions for a frontend. One request per input line, one response per
// output line; one active quiz session at a time. See docs/protocol.md.
class RpcServer {
public:
    // `sync_fn` runs one real sync (given session_gap_minutes) and returns
    // its summary; injected so tests need no network. `now_fn` supplies the
    // clock (fixed in tests).
    RpcServer(Store& store, std::function<SyncSummary(int)> sync_fn,
              std::function<std::string()> now_fn = utc_now_iso8601);
    ~RpcServer();

    // Handles one request line; returns the response line ("" for a
    // notification, which gets no response).
    std::string handle_line(const std::string& line);

    // Reads requests from `in` until EOF, writing responses to `out`.
    void serve(std::istream& in, std::ostream& out);

private:
    struct ActiveSession;
    nlohmann::json dispatch(const std::string& method, const nlohmann::json& params);
    nlohmann::json start_session(const nlohmann::json& params);
    nlohmann::json next_question();
    nlohmann::json answer_question(const nlohmann::json& params);
    nlohmann::json abort_session();

    Store& store_;
    std::function<SyncSummary(int)> sync_fn_;
    std::function<std::string()> now_fn_;
    std::unique_ptr<ActiveSession> session_;
};
