#include "similarity.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>

namespace {

// Decodes a UTF-8 byte string into its Unicode codepoints. Malformed
// trailing bytes (a truncated multi-byte sequence at the end of the
// string) are simply dropped rather than throwing — subject.characters
// always comes from WaniKani's own well-formed JSON, so this is a safety
// net, not a real code path.
std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if ((c & 0x80) == 0x00) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            // Invalid leading byte: skip it and keep going rather than
            // aborting the whole decode.
            ++i;
            continue;
        }
        if (i + len > s.size()) {
            break;  // truncated multi-byte sequence at end of string
        }
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cont = static_cast<unsigned char>(s[i + k]);
            cp = (cp << 6) | (cont & 0x3F);
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Standard two-row Levenshtein distance over codepoint sequences (not
// bytes), so multi-byte Japanese characters each count as one edit unit.
size_t levenshtein(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    std::vector<size_t> prev(m + 1);
    std::vector<size_t> curr(m + 1);
    for (size_t j = 0; j <= m; ++j) {
        prev[j] = j;
    }
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

// Naive ASCII lowercasing. WaniKani meanings are English words/phrases
// ("fat", "big"), so locale-aware casing is unnecessary here.
std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

using IdPair = std::pair<long long, long long>;

// Normalizes (id_a, id_b) so the smaller id is first, matching
// SimilarityEdge's a_id < b_id invariant.
IdPair normalize_pair(long long id_a, long long id_b) {
    return id_a < id_b ? IdPair{id_a, id_b} : IdPair{id_b, id_a};
}

// wk_visual: a direct id-list walk, no bucketing needed — it's already
// O(n) over the (small) visually_similar_subject_ids lists. A std::set
// keyed on the normalized pair is what prevents a reverse pairing
// (WaniKani sometimes lists the relation on both subjects) from producing
// two edges.
std::set<IdPair> build_wk_visual_edges(const std::vector<Subject>& subjects) {
    std::set<long long> valid_ids;
    for (const auto& s : subjects) {
        valid_ids.insert(s.id);
    }

    std::set<IdPair> edges;
    for (const auto& s : subjects) {
        for (long long other_id : s.visually_similar_subject_ids) {
            if (other_id == s.id || valid_ids.count(other_id) == 0) {
                continue;
            }
            edges.insert(normalize_pair(s.id, other_id));
        }
    }
    return edges;
}

// component: bucket subjects by each component id they contain (an
// inverted index: component_id -> subject indices), then only compare
// subjects within the same bucket. Jaccard is computed over each pair's
// full component_subject_ids sets and is identical no matter which
// bucket the pair is found through, so dedup is just "first result
// wins" via the map.
std::map<IdPair, double> build_component_edges(const std::vector<Subject>& subjects) {
    std::vector<std::set<long long>> component_sets(subjects.size());
    for (size_t i = 0; i < subjects.size(); ++i) {
        for (long long comp_id : subjects[i].component_subject_ids) {
            component_sets[i].insert(comp_id);
        }
    }

    std::unordered_map<long long, std::vector<size_t>> buckets;
    for (size_t i = 0; i < subjects.size(); ++i) {
        for (long long comp_id : component_sets[i]) {
            buckets[comp_id].push_back(i);
        }
    }

    std::map<IdPair, double> edges;
    for (const auto& [comp_id, indices] : buckets) {
        for (size_t x = 0; x < indices.size(); ++x) {
            for (size_t y = x + 1; y < indices.size(); ++y) {
                const size_t i = indices[x];
                const size_t j = indices[y];
                if (subjects[i].id == subjects[j].id) {
                    continue;
                }
                const double index = jaccard(component_sets[i], component_sets[j]);
                if (index >= 0.5) {
                    edges.try_emplace(normalize_pair(subjects[i].id, subjects[j].id), index);
                }
            }
        }
    }
    return edges;
}

// reading: bucket by each reading string (inverted index: reading string
// -> [(subject index, was this reading primary for that subject)]).
// Radicals have no readings and are naturally excluded since they never
// contribute any bucket entries. Unlike component's Jaccard, a pair's
// weight here can differ depending on which shared reading string
// produced the match (primary-primary vs not), so ties are broken by
// keeping the strongest (highest-weight) signal found across every
// reading string the pair shares, rather than whichever bucket the
// unordered_map happens to visit first.
std::map<IdPair, double> build_reading_edges(const std::vector<Subject>& subjects) {
    struct Entry {
        size_t index;
        bool primary;
    };
    std::unordered_map<std::string, std::vector<Entry>> buckets;
    for (size_t i = 0; i < subjects.size(); ++i) {
        for (const auto& r : subjects[i].readings) {
            buckets[r.reading].push_back({i, r.primary});
        }
    }

    std::map<IdPair, double> edges;
    for (const auto& [reading_str, entries] : buckets) {
        for (size_t x = 0; x < entries.size(); ++x) {
            for (size_t y = x + 1; y < entries.size(); ++y) {
                const size_t i = entries[x].index;
                const size_t j = entries[y].index;
                if (subjects[i].id == subjects[j].id) {
                    continue;
                }
                const double weight = (entries[x].primary && entries[y].primary) ? 0.8 : 0.5;
                const IdPair key = normalize_pair(subjects[i].id, subjects[j].id);
                auto it = edges.find(key);
                if (it == edges.end() || weight > it->second) {
                    edges[key] = weight;
                }
            }
        }
    }
    return edges;
}

// meaning: bucket by each lowercased meaning string, from both meanings
// (all entries, not just primary) and auxiliary_meanings. Weight is
// fixed at 0.7 for every pair that shares any meaning string, so this
// only needs to track presence, not a per-bucket weight.
std::set<IdPair> build_meaning_edges(const std::vector<Subject>& subjects) {
    std::unordered_map<std::string, std::vector<size_t>> buckets;
    for (size_t i = 0; i < subjects.size(); ++i) {
        // Dedup per subject first (a meaning could conceivably appear in
        // both meanings and auxiliary_meanings, or be repeated) so a
        // subject never appears twice in the same bucket.
        std::set<std::string> lowered;
        for (const auto& m : subjects[i].meanings) {
            lowered.insert(to_lower(m.meaning));
        }
        for (const auto& m : subjects[i].auxiliary_meanings) {
            lowered.insert(to_lower(m.meaning));
        }
        for (const auto& meaning_str : lowered) {
            buckets[meaning_str].push_back(i);
        }
    }

    std::set<IdPair> edges;
    for (const auto& [meaning_str, indices] : buckets) {
        for (size_t x = 0; x < indices.size(); ++x) {
            for (size_t y = x + 1; y < indices.size(); ++y) {
                const size_t i = indices[x];
                const size_t j = indices[y];
                if (subjects[i].id == subjects[j].id) {
                    continue;
                }
                edges.insert(normalize_pair(subjects[i].id, subjects[j].id));
            }
        }
    }
    return edges;
}

// char_shape: bucket by codepoint length (a byte-length bucket would
// wrongly group e.g. two single-byte ASCII chars with one two-byte
// character of matching total bytes, so the bucket key itself must
// already be codepoint-based). Within a length bucket every pair
// automatically has the same length, so normalized similarity is
// 1 - (levenshtein distance / that shared length). The 0.6 keep
// threshold isn't given numerically by the parent design doc beyond
// "catches 大人 against 大学" — 0.6 is chosen here as "differs by at
// most one character out of a two-plus character string" (a 1-of-2 diff
// scores exactly 0.5 and is excluded; a 1-of-3 diff scores 0.667 and is
// kept).
constexpr double kCharShapeThreshold = 0.6;

std::set<IdPair> build_char_shape_edges(const std::vector<Subject>& subjects) {
    std::vector<std::vector<uint32_t>> codepoints(subjects.size());
    std::unordered_map<size_t, std::vector<size_t>> buckets;
    for (size_t i = 0; i < subjects.size(); ++i) {
        if (subjects[i].characters.empty()) {
            continue;
        }
        codepoints[i] = utf8_decode(subjects[i].characters);
        buckets[codepoints[i].size()].push_back(i);
    }

    std::set<IdPair> edges;
    for (const auto& [len, indices] : buckets) {
        for (size_t x = 0; x < indices.size(); ++x) {
            for (size_t y = x + 1; y < indices.size(); ++y) {
                const size_t i = indices[x];
                const size_t j = indices[y];
                if (subjects[i].id == subjects[j].id || len == 0) {
                    continue;
                }
                const size_t distance = levenshtein(codepoints[i], codepoints[j]);
                const double similarity = 1.0 - static_cast<double>(distance) / static_cast<double>(len);
                if (similarity >= kCharShapeThreshold) {
                    edges.insert(normalize_pair(subjects[i].id, subjects[j].id));
                }
            }
        }
    }
    return edges;
}

}  // namespace

double jaccard(const std::set<long long>& a, const std::set<long long>& b) {
    if (a.empty() && b.empty()) {
        return 0.0;
    }
    std::vector<long long> intersection;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(intersection));
    const size_t union_size = a.size() + b.size() - intersection.size();
    if (union_size == 0) {
        return 0.0;
    }
    return static_cast<double>(intersection.size()) / static_cast<double>(union_size);
}

size_t utf8_codepoint_count(const std::string& s) { return utf8_decode(s).size(); }

std::vector<SimilarityEdge> build_similarity_graph(const std::vector<Subject>& subjects) {
    std::vector<SimilarityEdge> result;

    for (const auto& [a, b] : build_wk_visual_edges(subjects)) {
        result.push_back(SimilarityEdge{a, b, EdgeKind::WkVisual, 1.0});
    }
    for (const auto& [key, weight] : build_component_edges(subjects)) {
        result.push_back(SimilarityEdge{key.first, key.second, EdgeKind::Component, weight});
    }
    for (const auto& [key, weight] : build_reading_edges(subjects)) {
        result.push_back(SimilarityEdge{key.first, key.second, EdgeKind::Reading, weight});
    }
    for (const auto& [a, b] : build_meaning_edges(subjects)) {
        result.push_back(SimilarityEdge{a, b, EdgeKind::Meaning, 0.7});
    }
    for (const auto& [a, b] : build_char_shape_edges(subjects)) {
        result.push_back(SimilarityEdge{a, b, EdgeKind::CharShape, 0.4});
    }

    return result;
}
