#include "advice.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace {

// Radicals can be image-only (empty `characters`), so fall back to the
// slug wherever a subject needs to be named in generated text.
std::string display_name(const Subject& subject) {
    return subject.characters.empty() ? subject.slug : subject.characters;
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Rule 1: always the first line. Names which axis (or both) the user
// actually got wrong on `failed_subject`.
void append_failure_type_line(std::ostringstream& out, const Subject& failed_subject,
                               FailureKind failure_kind) {
    const std::string name = display_name(failed_subject);
    switch (failure_kind) {
        case FailureKind::Meaning:
            out << "You fail the MEANING of " << name << ", not the reading.\n";
            break;
        case FailureKind::Reading:
            out << "You fail the READING of " << name << ", not the meaning.\n";
            break;
        case FailureKind::Both:
            out << "You fail BOTH the meaning and the reading of " << name << ".\n";
            break;
    }
}

// Resolves a component subject id to a printable slug, falling back to
// the numeric id (as text) if the id isn't found in `all_subjects` —
// this can legitimately happen for a component sync hasn't fetched yet,
// and printing something is better than silently dropping the entry.
std::string component_label(long long id, const std::map<long long, const Subject*>& by_id) {
    const auto it = by_id.find(id);
    if (it != by_id.end()) {
        return it->second->slug.empty() ? std::to_string(id) : it->second->slug;
    }
    return std::to_string(id);
}

std::string join(const std::vector<std::string>& items) {
    std::ostringstream out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << items[i];
    }
    return out.str();
}

// Rule 2: the set difference between the two subjects' components, both
// ways. Skipped when either subject has no components (radicals) or the
// two component sets are identical (nothing to diff).
void append_component_diff(std::ostringstream& out, const Subject& failed_subject,
                            const Subject& neighbor_subject, const std::vector<Subject>& all_subjects) {
    if (failed_subject.component_subject_ids.empty() || neighbor_subject.component_subject_ids.empty()) {
        return;
    }

    const std::set<long long> failed_set(failed_subject.component_subject_ids.begin(),
                                          failed_subject.component_subject_ids.end());
    const std::set<long long> neighbor_set(neighbor_subject.component_subject_ids.begin(),
                                            neighbor_subject.component_subject_ids.end());
    if (failed_set == neighbor_set) {
        return;
    }

    std::vector<long long> failed_only;
    std::vector<long long> neighbor_only;
    std::set_difference(failed_set.begin(), failed_set.end(), neighbor_set.begin(), neighbor_set.end(),
                         std::back_inserter(failed_only));
    std::set_difference(neighbor_set.begin(), neighbor_set.end(), failed_set.begin(), failed_set.end(),
                         std::back_inserter(neighbor_only));
    if (failed_only.empty() && neighbor_only.empty()) {
        return;  // shouldn't happen given the != check above, but guard anyway
    }

    std::map<long long, const Subject*> by_id;
    for (const auto& s : all_subjects) {
        by_id[s.id] = &s;
    }

    auto labels = [&](const std::vector<long long>& ids) {
        std::vector<std::string> out_labels;
        out_labels.reserve(ids.size());
        for (long long id : ids) {
            out_labels.push_back(component_label(id, by_id));
        }
        return out_labels;
    };

    out << display_name(failed_subject) << " has: [" << join(labels(failed_only)) << "]. "
        << display_name(neighbor_subject) << " has: [" << join(labels(neighbor_only)) << "].\n";
}

// Rule 3: only fires for a Reading edge. Prints the shared reading(s)
// plus each subject's readings that aren't shared, so the user can see
// exactly which sound the two subjects have in common and which ones
// tell them apart. Skipped (prints nothing) if edge_kind != Reading, or
// if there turns out to be no actual shared reading (defensive — the
// similarity graph is what claims a Reading edge exists).
void append_shared_reading(std::ostringstream& out, const Subject& failed_subject,
                            const Subject& neighbor_subject, EdgeKind edge_kind) {
    if (edge_kind != EdgeKind::Reading) {
        return;
    }

    std::set<std::string> failed_readings;
    for (const auto& r : failed_subject.readings) {
        failed_readings.insert(r.reading);
    }
    std::set<std::string> neighbor_readings;
    for (const auto& r : neighbor_subject.readings) {
        neighbor_readings.insert(r.reading);
    }

    std::vector<std::string> shared;
    std::set_intersection(failed_readings.begin(), failed_readings.end(), neighbor_readings.begin(),
                           neighbor_readings.end(), std::back_inserter(shared));
    if (shared.empty()) {
        return;
    }

    std::vector<std::string> failed_only;
    std::vector<std::string> neighbor_only;
    std::set_difference(failed_readings.begin(), failed_readings.end(), neighbor_readings.begin(),
                         neighbor_readings.end(), std::back_inserter(failed_only));
    std::set_difference(neighbor_readings.begin(), neighbor_readings.end(), failed_readings.begin(),
                         failed_readings.end(), std::back_inserter(neighbor_only));

    out << "Shared reading: " << join(shared) << ". " << display_name(failed_subject)
        << " also reads: [" << join(failed_only) << "]. " << display_name(neighbor_subject)
        << " also reads: [" << join(neighbor_only) << "].\n";
}

constexpr size_t kMnemonicTruncateLen = 300;

// Cuts `text` to roughly kMnemonicTruncateLen characters at a sentence
// boundary (a '.', '!', or '?') if one exists within range, else at a
// word boundary, so a long mnemonic paragraph never gets chopped
// mid-word. Returns `text` unchanged if it's already short enough.
std::string truncate_mnemonic(const std::string& text) {
    if (text.size() <= kMnemonicTruncateLen) {
        return text;
    }

    size_t cut = std::string::npos;
    for (size_t i = kMnemonicTruncateLen; i > 0; --i) {
        const char c = text[i - 1];
        if (c == '.' || c == '!' || c == '?') {
            cut = i;
            break;
        }
    }
    if (cut == std::string::npos) {
        const size_t space = text.rfind(' ', kMnemonicTruncateLen);
        cut = (space == std::string::npos) ? kMnemonicTruncateLen : space;
    }

    std::string result = text.substr(0, cut);
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
        result.pop_back();
    }
    return result + "...";
}

// Rule 4: the stored mnemonic for whichever axis `failure_kind` is
// about (Both counts as meaning, matching the meaning_mnemonic/
// reading_mnemonic choice made for the same axis elsewhere in this
// file), truncated per truncate_mnemonic. If the user has their own
// study_material note on the same axis, it's appended, clearly labeled
// as the user's own note rather than WaniKani's.
void append_mnemonic(std::ostringstream& out, const Subject& failed_subject, FailureKind failure_kind,
                      const std::optional<wk_api::StudyMaterial>& study_material) {
    const bool is_meaning_axis = (failure_kind != FailureKind::Reading);
    const std::string& mnemonic =
        is_meaning_axis ? failed_subject.meaning_mnemonic : failed_subject.reading_mnemonic;

    if (!mnemonic.empty()) {
        out << "Mnemonic: " << truncate_mnemonic(mnemonic) << "\n";
    }

    if (study_material.has_value()) {
        const std::string& note =
            is_meaning_axis ? study_material->meaning_note : study_material->reading_note;
        if (!note.empty()) {
            out << "Your note: " << note << "\n";
        }
    }
}

// Rule 5: only fires for a Meaning edge. Prints the meaning string(s)
// the two subjects share, then each subject's accepted_answer meanings
// that aren't shared — i.e. what the user should have typed for
// `failed_subject` vs. what they likely typed instead (a meaning they
// know from `neighbor_subject`).
void append_meaning_collision(std::ostringstream& out, const Subject& failed_subject,
                               const Subject& neighbor_subject, EdgeKind edge_kind) {
    if (edge_kind != EdgeKind::Meaning) {
        return;
    }

    std::set<std::string> failed_all;
    for (const auto& m : failed_subject.meanings) {
        failed_all.insert(lower(m.meaning));
    }
    for (const auto& m : failed_subject.auxiliary_meanings) {
        failed_all.insert(lower(m.meaning));
    }
    std::set<std::string> neighbor_all;
    for (const auto& m : neighbor_subject.meanings) {
        neighbor_all.insert(lower(m.meaning));
    }
    for (const auto& m : neighbor_subject.auxiliary_meanings) {
        neighbor_all.insert(lower(m.meaning));
    }

    std::vector<std::string> overlap;
    std::set_intersection(failed_all.begin(), failed_all.end(), neighbor_all.begin(), neighbor_all.end(),
                           std::back_inserter(overlap));
    if (overlap.empty()) {
        return;
    }

    std::vector<std::string> failed_accepted;
    for (const auto& m : failed_subject.meanings) {
        if (m.accepted_answer && neighbor_all.count(lower(m.meaning)) == 0) {
            failed_accepted.push_back(m.meaning);
        }
    }
    std::vector<std::string> neighbor_accepted;
    for (const auto& m : neighbor_subject.meanings) {
        if (m.accepted_answer && failed_all.count(lower(m.meaning)) == 0) {
            neighbor_accepted.push_back(m.meaning);
        }
    }

    out << "Overlapping meaning: " << join(overlap) << ". You should type: [" << join(failed_accepted)
        << "]. You likely confused it for: [" << join(neighbor_accepted) << "].\n";
}

}  // namespace

std::string generate_advice(const Subject& failed_subject, const Subject& neighbor_subject,
                             EdgeKind edge_kind, FailureKind failure_kind,
                             const std::optional<wk_api::StudyMaterial>& study_material,
                             const std::vector<Subject>& all_subjects) {
    std::ostringstream out;
    append_failure_type_line(out, failed_subject, failure_kind);
    append_component_diff(out, failed_subject, neighbor_subject, all_subjects);
    append_shared_reading(out, failed_subject, neighbor_subject, edge_kind);
    append_mnemonic(out, failed_subject, failure_kind, study_material);
    append_meaning_collision(out, failed_subject, neighbor_subject, edge_kind);
    return out.str();
}
