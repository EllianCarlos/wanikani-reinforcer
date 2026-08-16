#pragma once

#include <string>
#include <vector>

#include "model.h"

// Typed WaniKani API endpoints, built on top of http::get(). This task
// only implements /subjects; later tasks add assignments, study
// materials, and review statistics fetches to this same header.
namespace wk_api {

// Fetches every subject from GET /subjects, following pagination via
// `pages.next_url` until it is null. If `updated_after` is non-empty, it
// is sent as the `updated_after` query parameter (URL-encoded) on the
// first request, so the caller can resume from a stored cursor.
std::vector<Subject> fetch_all_subjects(const std::string& updated_after);

}  // namespace wk_api
