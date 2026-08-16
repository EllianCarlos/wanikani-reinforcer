#pragma once

#include <string>
#include <vector>

// Domain model shared across wkr. This header grows incrementally: each
// task adds only the structs it needs. Do not add structs here for
// features not yet implemented.

struct Meaning {
    std::string meaning;
    bool primary = false;
    bool accepted_answer = false;
};

struct AuxiliaryMeaning {
    std::string meaning;
    std::string type;
};

struct Reading {
    std::string reading;
    bool primary = false;
    bool accepted_answer = false;
};

struct Subject {
    long long id = 0;
    std::string type;              // "radical" | "kanji" | "vocabulary" | "kana_vocabulary"
    std::string characters;        // may be empty for some radicals (image-only)
    std::string slug;
    int level = 0;
    std::vector<Meaning> meanings;
    std::vector<AuxiliaryMeaning> auxiliary_meanings;
    std::vector<Reading> readings; // empty for radicals
    std::vector<long long> component_subject_ids;
    std::vector<long long> visually_similar_subject_ids; // optional; kanji-only per WK docs, but read if present on other types
    std::string meaning_mnemonic;
    std::string reading_mnemonic;
    std::string data_updated_at;   // ISO 8601 string, store and compare as text
};

struct Assignment {
    long long id = 0;
    long long subject_id = 0;
    std::string subject_type;
    int srs_stage = 0;
    std::string available_at;
    std::string passed_at;
    std::string data_updated_at;
};

// Returns the first Meaning with primary == true, or the first meaning if
// none is marked primary. Returns nullptr if meanings is empty.
inline const Meaning* primary_meaning(const Subject& subject) {
    if (subject.meanings.empty()) {
        return nullptr;
    }
    for (const auto& m : subject.meanings) {
        if (m.primary) {
            return &m;
        }
    }
    return &subject.meanings.front();
}
