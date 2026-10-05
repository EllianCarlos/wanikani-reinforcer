#include "rpc.h"

#include <algorithm>
#include <stdexcept>

#include "json_codec.h"
#include "kana.h"
#include "srs.h"

namespace {

using nlohmann::json;

struct RpcError : std::runtime_error {
    RpcError(int code_, const std::string& message) : std::runtime_error(message), code(code_) {}
    int code;
};

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kAppError = -32000;  // e.g. no active session

std::string string_param(const json& params, const char* key, const std::string& fallback = "") {
    if (!params.is_object() || !params.contains(key)) {
        return fallback;
    }
    if (!params.at(key).is_string()) {
        throw RpcError(kInvalidParams, std::string("param '") + key + "' must be a string");
    }
    return params.at(key).get<std::string>();
}

int int_param(const json& params, const char* key, int fallback) {
    if (!params.is_object() || !params.contains(key)) {
        return fallback;
    }
    if (!params.at(key).is_number_integer()) {
        throw RpcError(kInvalidParams, std::string("param '") + key + "' must be an integer");
    }
    return params.at(key).get<int>();
}

json error_response(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

struct RpcServer::ActiveSession {
    std::string kind;
    AppData data;  // owns the vectors the session engines reference
    std::unique_ptr<DrillSession> drill;
    std::unique_ptr<BurnedSession> burned;
    int total = 0;
    std::optional<json> awaiting;  // question handed out, not yet answered
};

RpcServer::RpcServer(Store& store, std::function<SyncSummary(int)> sync_fn, std::function<std::string()> now_fn)
    : store_(store), sync_fn_(std::move(sync_fn)), now_fn_(std::move(now_fn)) {}

RpcServer::~RpcServer() = default;

void RpcServer::serve(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        const std::string response = handle_line(line);
        if (!response.empty()) {
            out << response << "\n" << std::flush;
        }
    }
}

std::string RpcServer::handle_line(const std::string& line) {
    json request;
    try {
        request = json::parse(line);
    } catch (const json::parse_error& e) {
        return error_response(nullptr, kParseError, std::string("parse error: ") + e.what()).dump();
    }
    if (!request.is_object() || !request.contains("method") || !request.at("method").is_string()) {
        const json id = request.is_object() && request.contains("id") ? request.at("id") : json(nullptr);
        return error_response(id, kInvalidRequest, "invalid request").dump();
    }

    const bool is_notification = !request.contains("id");
    const json id = is_notification ? json(nullptr) : request.at("id");
    try {
        const json params = request.contains("params") ? request.at("params") : json::object();
        json result = dispatch(request.at("method").get<std::string>(), params);
        if (is_notification) {
            return "";
        }
        return json({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}).dump();
    } catch (const RpcError& e) {
        return is_notification ? "" : error_response(id, e.code, e.what()).dump();
    } catch (const std::exception& e) {
        return is_notification ? "" : error_response(id, kInternalError, e.what()).dump();
    }
}

json RpcServer::dispatch(const std::string& method, const json& params) {
    if (method == "dashboard.get") {
        return build_dashboard(store_, now_fn_());
    }
    if (method == "sync.run") {
        return sync_fn_(int_param(params, "session_gap_minutes", 45));
    }
    if (method == "session.start") {
        return start_session(params);
    }
    if (method == "session.next") {
        return next_question();
    }
    if (method == "session.answer") {
        return answer_question(params);
    }
    if (method == "session.abort") {
        return abort_session();
    }
    if (method == "kana.convert") {
        return {{"hiragana", to_hiragana(string_param(params, "text"))}};
    }
    throw RpcError(kMethodNotFound, "method not found: " + method);
}

json RpcServer::start_session(const json& params) {
    const std::string kind = string_param(params, "kind");
    if (kind != "drill" && kind != "burned") {
        throw RpcError(kInvalidParams, "param 'kind' must be \"drill\" or \"burned\"");
    }
    const int count = int_param(params, "count", kind == "drill" ? kDefaultDrillQuestionCount : kDefaultBurnedQuestionCount);
    if (count <= 0) {
        throw RpcError(kInvalidParams, "param 'count' must be positive");
    }
    const std::string now = now_fn_();

    auto session = std::make_unique<ActiveSession>();
    session->kind = kind;
    session->data = load_app_data(store_, now);

    if (kind == "drill") {
        if (session->data.pairs.empty()) {
            return {{"nothing_due", "Nothing to drill yet -- run a sync first to build up confusion pairs."}};
        }
        const std::vector<PairSrsState> srs = store_.all_pair_srs();
        std::vector<ConfusionPair> due = select_due_pairs(session->data.pairs, srs, now);
        if (due.empty()) {
            return {{"nothing_due", nothing_due_message(session->data.pairs, srs, now)}};
        }
        session->drill = std::make_unique<DrillSession>(std::move(due), session->data.subjects,
                                                         session->data.study_materials, store_, count, now);
        session->total = session->drill->total();
    } else {
        std::set<std::string> types;
        try {
            types = resolve_burned_types(string_param(params, "only"), string_param(params, "exclude"));
        } catch (const std::exception& e) {
            throw RpcError(kInvalidParams, e.what());
        }
        const std::vector<BurnedSrsState> states = store_.all_burned_srs();
        std::vector<long long> due = select_due_burned(session->data.assignments, states, now, types);
        if (due.empty()) {
            return {{"nothing_due", burned_nothing_due_message(session->data.assignments, states, now, types)}};
        }
        session->total = static_cast<int>(std::min(due.size(), static_cast<size_t>(count)));
        session->burned = std::make_unique<BurnedSession>(std::move(due), session->data.subjects,
                                                           session->data.study_materials, store_, count, now);
    }

    const int total = session->total;
    session_ = std::move(session);
    return {{"total", total}};
}

json RpcServer::next_question() {
    if (!session_) {
        throw RpcError(kAppError, "no active session");
    }
    if (session_->awaiting) {
        return {{"done", false}, {"question", *session_->awaiting}};
    }

    json question;
    if (session_->drill) {
        const std::optional<DrillQuestion> q = session_->drill->next();
        if (q) {
            question = *q;
            question["index"] = session_->drill->asked();
            question["total"] = session_->total;
        }
    } else {
        const std::optional<BurnedQuestion> q = session_->burned->next();
        if (q) {
            question = *q;
            question["index"] = session_->burned->asked();
            question["total"] = session_->total;
        }
    }

    if (question.is_null()) {
        const int asked = session_->drill ? session_->drill->asked() : session_->burned->asked();
        const int correct = session_->drill ? session_->drill->correct() : session_->burned->correct();
        session_.reset();
        return {{"done", true}, {"asked", asked}, {"correct", correct}};
    }
    session_->awaiting = question;
    return {{"done", false}, {"question", question}};
}

json RpcServer::answer_question(const json& params) {
    if (!session_) {
        throw RpcError(kAppError, "no active session");
    }
    if (!session_->awaiting) {
        throw RpcError(kAppError, "no pending question; call session.next first");
    }
    if (!params.is_object() || !params.contains("answer")) {
        throw RpcError(kInvalidParams, "missing param 'answer'");
    }
    const std::string answer = string_param(params, "answer");

    json feedback;
    bool retry = false;
    if (session_->drill) {
        feedback = session_->drill->answer(answer);
    } else {
        const BurnedFeedback fb = session_->burned->answer(answer);
        feedback = fb;
        retry = fb.retry;
    }
    // A retry keeps the same question pending; anything else consumes it.
    if (!retry) {
        session_->awaiting.reset();
    }
    return feedback;
}

json RpcServer::abort_session() {
    if (!session_) {
        throw RpcError(kAppError, "no active session");
    }
    const int asked = session_->drill ? session_->drill->asked() : session_->burned->asked();
    const int correct = session_->drill ? session_->drill->correct() : session_->burned->correct();
    session_.reset();
    return {{"asked", asked}, {"correct", correct}};
}
