#pragma once

#include <string>
#include <vector>

// The GET-only HTTP client. This is the single place in wkr that talks to
// the network. It must never grow a way to perform a write-verb request
// (POST, PUT, PATCH, DELETE, or an upload) — that is a structural
// guarantee enforced by a repo-wide grep in CI/review, not a comment
// promise.
namespace http {

inline constexpr const char* kBaseUrl = "https://api.wanikani.com/v2";

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string etag;
};

// Performs an HTTP GET against `url`.
//
// Always sends `Authorization: Bearer <token>` (token resolved via
// Config::load(), cached for the process) and `Wanikani-Revision:
// 20170710`. `extra_headers` are additional raw header lines (e.g.
// "X-Foo: bar").
//
// `if_none_match`, if non-empty, sends `If-None-Match: <if_none_match>`
// for conditional requests; expect a 304 response when the resource is
// unchanged. The response ETag header is always captured into
// HttpResponse::etag, on 200 or 304.
//
// Rate limiting: after any response, if `RateLimit-Remaining` reached 0,
// sleeps until `RateLimit-Reset` (a Unix timestamp) before returning, so
// the caller's next call won't fire into a 429. On a 429 response itself,
// sleeps until `RateLimit-Reset` and retries (capped at 3 total
// attempts). On a 5xx response, retries with exponential backoff (1s,
// 2s, 4s; capped at 3 total attempts), then returns the error response
// to the caller.
HttpResponse get(const std::string& url,
                  const std::vector<std::string>& extra_headers = {},
                  const std::string& if_none_match = "");

// Percent-encodes `value` for safe use as a URL query parameter value.
std::string url_encode(const std::string& value);

}  // namespace http
