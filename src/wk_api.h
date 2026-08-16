#pragma once

#include <string>
#include <vector>

#include "model.h"

// Typed WaniKani API endpoints, built on top of http::get().
namespace wk_api {

// Fetches every subject from GET /subjects, following pagination via
// `pages.next_url` until it is null. If `updated_after` is non-empty, it
// is sent as the `updated_after` query parameter (URL-encoded) on the
// first request, so the caller can resume from a stored cursor.
std::vector<Subject> fetch_all_subjects(const std::string& updated_after);

// Fetches every review statistic from GET /review_statistics, following
// pagination the same way as fetch_all_subjects.
std::vector<ReviewStat> fetch_all_review_statistics(const std::string& updated_after);

// Fetches every assignment from GET /assignments, following pagination
// the same way as fetch_all_subjects.
std::vector<Assignment> fetch_all_assignments(const std::string& updated_after);

struct StudyMaterial {
    long long subject_id = 0;
    std::string subject_type;
    std::string meaning_note, reading_note;
    std::vector<std::string> meaning_synonyms;
    std::string data_updated_at;
};

// Fetches every study material from GET /study_materials, following
// pagination the same way as fetch_all_subjects.
std::vector<StudyMaterial> fetch_all_study_materials(const std::string& updated_after);

}  // namespace wk_api
