#include "wk_api.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>

#include "http.h"

namespace wk_api {

namespace {

using nlohmann::json;

std::string get_string_or_empty(const json& obj, const std::string& key) {
    if (!obj.contains(key) || obj[key].is_null()) {
        return "";
    }
    return obj[key].get<std::string>();
}

std::vector<long long> get_ids_or_empty(const json& obj, const std::string& key) {
    if (!obj.contains(key) || obj[key].is_null()) {
        return {};
    }
    return obj[key].get<std::vector<long long>>();
}

std::vector<std::string> get_strings_or_empty(const json& obj, const std::string& key) {
    if (!obj.contains(key) || obj[key].is_null()) {
        return {};
    }
    return obj[key].get<std::vector<std::string>>();
}

// Fetches every page of `endpoint`, applying `parse_item` to each element
// of the top-level "data" array and collecting the results. Follows
// `pages.next_url` exactly like fetch_all_subjects. If `updated_after` is
// non-empty it is sent as the `updated_after` query parameter on the
// first request only (subsequent requests use the next_url WaniKani
// hands back, which already carries the cursor).
template <typename T>
std::vector<T> fetch_all_pages(const std::string& endpoint, const std::string& updated_after,
                                const std::function<T(const json&)>& parse_item) {
    std::vector<T> results;

    std::string url = std::string(http::kBaseUrl) + endpoint;
    if (!updated_after.empty()) {
        url += "?updated_after=" + http::url_encode(updated_after);
    }

    while (!url.empty()) {
        http::HttpResponse response = http::get(url);
        if (response.status != 200) {
            throw std::runtime_error("GET " + endpoint + " failed with status " +
                                      std::to_string(response.status));
        }

        json body = json::parse(response.body);
        if (body.contains("data") && body["data"].is_array()) {
            for (const auto& item : body["data"]) {
                results.push_back(parse_item(item));
            }
        }

        url.clear();
        if (body.contains("pages") && body["pages"].contains("next_url") &&
            !body["pages"]["next_url"].is_null()) {
            url = body["pages"]["next_url"].get<std::string>();
        }
    }

    return results;
}

Subject parse_subject(const json& item) {
    Subject subject;
    subject.id = item.value("id", 0LL);
    subject.type = get_string_or_empty(item, "object");
    subject.data_updated_at = get_string_or_empty(item, "data_updated_at");

    const json empty_object = json::object();
    const json& data = (item.contains("data") && item["data"].is_object()) ? item["data"] : empty_object;

    subject.characters = get_string_or_empty(data, "characters");
    subject.slug = get_string_or_empty(data, "slug");
    subject.level = data.value("level", 0);
    subject.meaning_mnemonic = get_string_or_empty(data, "meaning_mnemonic");
    subject.reading_mnemonic = get_string_or_empty(data, "reading_mnemonic");
    subject.component_subject_ids = get_ids_or_empty(data, "component_subject_ids");
    subject.visually_similar_subject_ids = get_ids_or_empty(data, "visually_similar_subject_ids");

    if (data.contains("meanings") && data["meanings"].is_array()) {
        for (const auto& m : data["meanings"]) {
            subject.meanings.push_back(Meaning{
                get_string_or_empty(m, "meaning"),
                m.value("primary", false),
                m.value("accepted_answer", false),
            });
        }
    }

    if (data.contains("auxiliary_meanings") && data["auxiliary_meanings"].is_array()) {
        for (const auto& m : data["auxiliary_meanings"]) {
            subject.auxiliary_meanings.push_back(AuxiliaryMeaning{
                get_string_or_empty(m, "meaning"),
                get_string_or_empty(m, "type"),
            });
        }
    }

    if (data.contains("readings") && data["readings"].is_array()) {
        for (const auto& r : data["readings"]) {
            subject.readings.push_back(Reading{
                get_string_or_empty(r, "reading"),
                r.value("primary", false),
                r.value("accepted_answer", false),
            });
        }
    }

    return subject;
}

ReviewStat parse_review_stat(const json& item) {
    ReviewStat stat;
    stat.id = item.value("id", 0LL);
    stat.data_updated_at = get_string_or_empty(item, "data_updated_at");

    const json empty_object = json::object();
    const json& data = (item.contains("data") && item["data"].is_object()) ? item["data"] : empty_object;

    stat.subject_id = data.value("subject_id", 0LL);
    stat.subject_type = get_string_or_empty(data, "subject_type");
    stat.hidden = data.value("hidden", false);
    stat.meaning_correct = data.value("meaning_correct", 0);
    stat.meaning_incorrect = data.value("meaning_incorrect", 0);
    stat.meaning_max_streak = data.value("meaning_max_streak", 0);
    stat.meaning_current_streak = data.value("meaning_current_streak", 0);
    stat.reading_correct = data.value("reading_correct", 0);
    stat.reading_incorrect = data.value("reading_incorrect", 0);
    stat.reading_max_streak = data.value("reading_max_streak", 0);
    stat.reading_current_streak = data.value("reading_current_streak", 0);
    stat.percentage_correct = data.value("percentage_correct", 0);

    return stat;
}

Assignment parse_assignment(const json& item) {
    Assignment assignment;
    assignment.id = item.value("id", 0LL);
    assignment.data_updated_at = get_string_or_empty(item, "data_updated_at");

    const json empty_object = json::object();
    const json& data = (item.contains("data") && item["data"].is_object()) ? item["data"] : empty_object;

    assignment.subject_id = data.value("subject_id", 0LL);
    assignment.subject_type = get_string_or_empty(data, "subject_type");
    assignment.srs_stage = data.value("srs_stage", 0);
    assignment.available_at = get_string_or_empty(data, "available_at");
    assignment.passed_at = get_string_or_empty(data, "passed_at");

    return assignment;
}

StudyMaterial parse_study_material(const json& item) {
    StudyMaterial material;
    material.data_updated_at = get_string_or_empty(item, "data_updated_at");

    const json empty_object = json::object();
    const json& data = (item.contains("data") && item["data"].is_object()) ? item["data"] : empty_object;

    material.subject_id = data.value("subject_id", 0LL);
    material.subject_type = get_string_or_empty(data, "subject_type");
    material.meaning_note = get_string_or_empty(data, "meaning_note");
    material.reading_note = get_string_or_empty(data, "reading_note");
    material.meaning_synonyms = get_strings_or_empty(data, "meaning_synonyms");

    return material;
}

}  // namespace

std::vector<Subject> fetch_all_subjects(const std::string& updated_after) {
    std::vector<Subject> subjects;

    std::string url = std::string(http::kBaseUrl) + "/subjects";
    if (!updated_after.empty()) {
        url += "?updated_after=" + http::url_encode(updated_after);
    }

    while (!url.empty()) {
        http::HttpResponse response = http::get(url);
        if (response.status != 200) {
            throw std::runtime_error(
                "GET /subjects failed with status " + std::to_string(response.status));
        }

        json body = json::parse(response.body);
        if (body.contains("data") && body["data"].is_array()) {
            for (const auto& item : body["data"]) {
                subjects.push_back(parse_subject(item));
            }
        }

        url.clear();
        if (body.contains("pages") && body["pages"].contains("next_url") &&
            !body["pages"]["next_url"].is_null()) {
            url = body["pages"]["next_url"].get<std::string>();
        }
    }

    return subjects;
}

std::vector<ReviewStat> fetch_all_review_statistics(const std::string& updated_after) {
    return fetch_all_pages<ReviewStat>("/review_statistics", updated_after, parse_review_stat);
}

std::vector<Assignment> fetch_all_assignments(const std::string& updated_after) {
    return fetch_all_pages<Assignment>("/assignments", updated_after, parse_assignment);
}

std::vector<StudyMaterial> fetch_all_study_materials(const std::string& updated_after) {
    return fetch_all_pages<StudyMaterial>("/study_materials", updated_after, parse_study_material);
}

}  // namespace wk_api
