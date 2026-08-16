#include "http.h"

#include "config.h"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <thread>

namespace http {

namespace {

void ensure_curl_initialized() {
    static bool initialized = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)initialized;
}

const std::string& cached_token() {
    static const std::string token = Config::load().api_token;
    return token;
}

size_t write_body_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
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

bool ci_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct HeaderState {
    std::string etag;
    std::optional<long> rate_limit_remaining;
    std::optional<long> rate_limit_reset;
};

size_t write_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* state = static_cast<HeaderState*>(userdata);
    const size_t len = size * nitems;
    const std::string line(buffer, len);
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return len;
    }
    const std::string name = trim(line.substr(0, colon));
    const std::string value = trim(line.substr(colon + 1));

    if (ci_equal(name, "ETag")) {
        state->etag = value;
    } else if (ci_equal(name, "RateLimit-Remaining")) {
        try {
            state->rate_limit_remaining = std::stol(value);
        } catch (...) {
            // Malformed header value; ignore rather than crash the sync.
        }
    } else if (ci_equal(name, "RateLimit-Reset")) {
        try {
            state->rate_limit_reset = std::stol(value);
        } catch (...) {
        }
    }
    return len;
}

void sleep_until_unix_timestamp(long reset_epoch_seconds) {
    const auto target =
        std::chrono::system_clock::from_time_t(static_cast<std::time_t>(reset_epoch_seconds));
    const auto now = std::chrono::system_clock::now();
    if (target > now) {
        // 1-second cushion so clock skew doesn't send us into another 429.
        std::this_thread::sleep_for(target - now + std::chrono::seconds(1));
    }
}

struct Attempt {
    HttpResponse response;
    HeaderState headers;
};

Attempt perform_once(const std::string& url,
                      const std::vector<std::string>& extra_headers,
                      const std::string& if_none_match) {
    ensure_curl_initialized();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("failed to initialize curl handle");
    }

    Attempt attempt;

    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + cached_token();
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Wanikani-Revision: 20170710");
    std::string if_none_match_header;
    if (!if_none_match.empty()) {
        if_none_match_header = "If-None-Match: " + if_none_match;
        headers = curl_slist_append(headers, if_none_match_header.c_str());
    }
    for (const auto& h : extra_headers) {
        headers = curl_slist_append(headers, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &attempt.response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &attempt.headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        const std::string message = curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP GET failed: " + message);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    attempt.response.status = status;
    attempt.response.etag = attempt.headers.etag;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return attempt;
}

}  // namespace

HttpResponse get(const std::string& url,
                  const std::vector<std::string>& extra_headers,
                  const std::string& if_none_match) {
    constexpr int kMaxAttempts = 3;
    HttpResponse last_response;

    for (int attempt_num = 1; attempt_num <= kMaxAttempts; ++attempt_num) {
        Attempt attempt = perform_once(url, extra_headers, if_none_match);
        last_response = attempt.response;

        if (attempt.response.status == 429) {
            if (attempt.headers.rate_limit_reset.has_value() && attempt_num < kMaxAttempts) {
                sleep_until_unix_timestamp(*attempt.headers.rate_limit_reset);
                continue;
            }
            return attempt.response;
        }

        if (attempt.response.status >= 500 && attempt.response.status < 600) {
            if (attempt_num < kMaxAttempts) {
                std::this_thread::sleep_for(std::chrono::seconds(1 << (attempt_num - 1)));
                continue;
            }
            return attempt.response;
        }

        // Success, 304, or a non-retryable 4xx. Enforce rate limiting
        // before handing control back so the caller's *next* get() call
        // doesn't fire straight into a 429.
        if (attempt.headers.rate_limit_remaining.has_value() &&
            *attempt.headers.rate_limit_remaining == 0 &&
            attempt.headers.rate_limit_reset.has_value()) {
            sleep_until_unix_timestamp(*attempt.headers.rate_limit_reset);
        }

        return attempt.response;
    }

    return last_response;
}

std::string url_encode(const std::string& value) {
    ensure_curl_initialized();
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("failed to initialize curl handle");
    }
    char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string result = escaped != nullptr ? std::string(escaped) : std::string();
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return result;
}

}  // namespace http
