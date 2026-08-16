#include "wk_api.h"

#include <nlohmann/json.hpp>

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

}  // namespace wk_api
