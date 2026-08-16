#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>

#include "model.h"
#include "similarity.h"

namespace {

Subject make_subject(long long id, const std::string& type, const std::string& characters) {
    Subject s;
    s.id = id;
    s.type = type;
    s.characters = characters;
    s.slug = characters;
    s.level = 1;
    return s;
}

// Finds the single edge of `kind` between subjects `a` and `b` (in either
// id order), or nullopt if none exists. Asserts there's at most one —
// build_similarity_graph must never double-emit the same (a,b,kind).
std::optional<SimilarityEdge> find_edge(const std::vector<SimilarityEdge>& edges, long long a,
                                         long long b, EdgeKind kind) {
    std::optional<SimilarityEdge> found;
    int count = 0;
    for (const auto& e : edges) {
        const bool matches_pair = (e.a_id == std::min(a, b) && e.b_id == std::max(a, b));
        if (matches_pair && e.kind == kind) {
            found = e;
            ++count;
        }
    }
    REQUIRE(count <= 1);
    return found;
}

}  // namespace

// --- jaccard boundary -------------------------------------------------

TEST_CASE("jaccard is correct at and around the 0.5 keep boundary", "[similarity]") {
    // {1,2,3} vs {2,3,4}: intersection {2,3} (size 2), union {1,2,3,4}
    // (size 4) -> 2/4 = 0.5 exactly, right at the keep threshold.
    CHECK(jaccard({1, 2, 3}, {2, 3, 4}) == Catch::Approx(0.5));

    // A pair scoring exactly 0.49, to pin the boundary direction: 49
    // shared elements, 21 unique to a, 30 unique to b -> |a|=70, |b|=79,
    // intersection 49, union = 49 + 21 + 30 = 100 -> 49/100 = 0.49.
    std::set<long long> a;
    std::set<long long> b;
    for (long long i = 0; i < 49; ++i) {
        a.insert(i);
        b.insert(i);
    }
    for (long long i = 100; i < 121; ++i) {
        a.insert(i);
    }
    for (long long i = 200; i < 230; ++i) {
        b.insert(i);
    }
    REQUIRE(a.size() == 70);
    REQUIRE(b.size() == 79);
    CHECK(jaccard(a, b) == Catch::Approx(0.49));
}

TEST_CASE("jaccard of two empty sets is 0.0, not a division by zero", "[similarity]") {
    CHECK(jaccard({}, {}) == Catch::Approx(0.0));
}

TEST_CASE("component edge kept at jaccard 0.5 boundary, weight equals the index", "[similarity]") {
    Subject a = make_subject(1, "kanji", "上");
    a.component_subject_ids = {1, 2, 3};
    Subject b = make_subject(2, "kanji", "下");
    b.component_subject_ids = {2, 3, 4};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    const auto edge = find_edge(edges, 1, 2, EdgeKind::Component);
    REQUIRE(edge.has_value());
    CHECK(edge->weight == Catch::Approx(0.5));
}

TEST_CASE("component edge excluded when jaccard scores 0.49, just under the boundary", "[similarity]") {
    Subject a = make_subject(1, "kanji", "上");
    Subject b = make_subject(2, "kanji", "下");
    for (long long i = 0; i < 49; ++i) {
        a.component_subject_ids.push_back(i);
        b.component_subject_ids.push_back(i);
    }
    for (long long i = 100; i < 121; ++i) {
        a.component_subject_ids.push_back(i);  // 21 unique to a -> |a| = 70
    }
    for (long long i = 200; i < 230; ++i) {
        b.component_subject_ids.push_back(i);  // 30 unique to b -> |b| = 79
    }

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    CHECK_FALSE(find_edge(edges, 1, 2, EdgeKind::Component).has_value());
}

// --- wk_visual ----------------------------------------------------------

TEST_CASE("wk_visual edges are normalized (a_id < b_id) and not double-emitted", "[similarity]") {
    Subject a = make_subject(20, "kanji", "土");
    Subject b = make_subject(10, "kanji", "士");
    // WaniKani sometimes lists the relation on both subjects.
    a.visually_similar_subject_ids = {10};
    b.visually_similar_subject_ids = {20};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    int visual_count = 0;
    for (const auto& e : edges) {
        if (e.kind == EdgeKind::WkVisual) {
            ++visual_count;
            CHECK(e.a_id == 10);
            CHECK(e.b_id == 20);
            CHECK(e.weight == Catch::Approx(1.0));
        }
    }
    CHECK(visual_count == 1);
}

TEST_CASE("wk_visual skips ids that aren't in the subject set", "[similarity]") {
    Subject a = make_subject(1, "kanji", "土");
    a.visually_similar_subject_ids = {999};  // not present in `subjects`

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a});

    CHECK(edges.empty());
}

// --- char_shape -----------------------------------------------------

TEST_CASE("char_shape: 2-codepoint strings differing by one codepoint score 0.5 and are excluded",
          "[similarity]") {
    Subject a = make_subject(1, "vocabulary", "大人");
    Subject b = make_subject(2, "vocabulary", "大学");

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    CHECK_FALSE(find_edge(edges, 1, 2, EdgeKind::CharShape).has_value());
}

TEST_CASE("char_shape: 3-codepoint strings differing by one codepoint score 0.667 and are kept",
          "[similarity]") {
    Subject a = make_subject(1, "vocabulary", "大学生");
    Subject b = make_subject(2, "vocabulary", "大学院");  // differs only in the last codepoint

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    const auto edge = find_edge(edges, 1, 2, EdgeKind::CharShape);
    REQUIRE(edge.has_value());
    CHECK(edge->weight == Catch::Approx(0.4));
}

TEST_CASE("char_shape buckets by codepoint count, not byte length", "[similarity]") {
    // "ABC" is 3 bytes / 3 codepoints. "水" is 3 bytes / 1 codepoint.
    // A byte-length bucket would wrongly put these in the same bucket;
    // bucketing by codepoint count must keep them apart.
    Subject a = make_subject(1, "vocabulary", "ABC");
    Subject b = make_subject(2, "kanji", "水");

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    CHECK_FALSE(find_edge(edges, 1, 2, EdgeKind::CharShape).has_value());
}

TEST_CASE("utf8_codepoint_count counts codepoints, not bytes", "[similarity]") {
    CHECK(utf8_codepoint_count("") == 0);
    CHECK(utf8_codepoint_count("AB") == 2);
    CHECK(utf8_codepoint_count("水") == 1);       // 3 UTF-8 bytes, 1 codepoint
    CHECK(utf8_codepoint_count("水星") == 2);      // 6 UTF-8 bytes, 2 codepoints
}

// --- meaning --------------------------------------------------------

TEST_CASE("meaning bucketing is case-insensitive", "[similarity]") {
    Subject a = make_subject(1, "kanji", "太");
    a.meanings = {Meaning{"Fat", true, true}};
    Subject b = make_subject(2, "kanji", "大");
    b.meanings = {Meaning{"fat", true, true}};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    const auto edge = find_edge(edges, 1, 2, EdgeKind::Meaning);
    REQUIRE(edge.has_value());
    CHECK(edge->weight == Catch::Approx(0.7));
}

TEST_CASE("meaning edges also match on auxiliary_meanings", "[similarity]") {
    Subject a = make_subject(1, "kanji", "太");
    a.auxiliary_meanings = {AuxiliaryMeaning{"Big", "whitelist"}};
    Subject b = make_subject(2, "kanji", "大");
    b.meanings = {Meaning{"big", true, true}};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    CHECK(find_edge(edges, 1, 2, EdgeKind::Meaning).has_value());
}

TEST_CASE("subjects with no shared meaning get no meaning edge", "[similarity]") {
    Subject a = make_subject(1, "kanji", "水");
    a.meanings = {Meaning{"Water", true, true}};
    Subject b = make_subject(2, "kanji", "火");
    b.meanings = {Meaning{"Fire", true, true}};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    CHECK_FALSE(find_edge(edges, 1, 2, EdgeKind::Meaning).has_value());
}

// --- reading ----------------------------------------------------------

TEST_CASE("reading edge weight is 0.8 when both sides share a primary reading", "[similarity]") {
    Subject a = make_subject(1, "kanji", "行");
    a.readings = {Reading{"こう", true, true}};
    Subject b = make_subject(2, "kanji", "校");
    b.readings = {Reading{"こう", true, true}};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    const auto edge = find_edge(edges, 1, 2, EdgeKind::Reading);
    REQUIRE(edge.has_value());
    CHECK(edge->weight == Catch::Approx(0.8));
}

TEST_CASE("reading edge weight is 0.5 when at least one side's shared reading is non-primary",
          "[similarity]") {
    Subject a = make_subject(1, "kanji", "行");
    a.readings = {Reading{"こう", true, true}, Reading{"ぎょう", false, true}};
    Subject b = make_subject(2, "kanji", "行事");
    b.readings = {Reading{"ぎょう", true, true}};

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    const auto edge = find_edge(edges, 1, 2, EdgeKind::Reading);
    REQUIRE(edge.has_value());
    CHECK(edge->weight == Catch::Approx(0.5));
}

TEST_CASE("radicals (empty readings) never produce reading edges", "[similarity]") {
    Subject a = make_subject(1, "radical", "");
    Subject b = make_subject(2, "radical", "");

    const std::vector<SimilarityEdge> edges = build_similarity_graph({a, b});

    CHECK(edges.empty());
}

// --- bucketing performance: not an O(n^2) full scan ----------------------

// Regression guard against an accidental full N x N comparison: 9000
// subjects (WaniKani's actual scale) each with a *distinct* component id,
// reading, and character string, so no two subjects ever land in the same
// bucket for any edge kind. A correctly bucketed implementation does
// effectively no pairwise work here and finishes in well under a second;
// a full O(n^2) comparison (~40M jaccard/levenshtein calls) would not.
TEST_CASE("build_similarity_graph stays fast at WaniKani scale with no shared buckets",
          "[similarity]") {
    std::vector<Subject> subjects;
    subjects.reserve(9000);
    for (long long i = 0; i < 9000; ++i) {
        // Characters left empty on purpose: distinct short numeric
        // strings (e.g. "1234" vs "1235") can be Levenshtein-close
        // enough to legitimately produce char_shape edges, which would
        // make this test about digit-string similarity instead of about
        // bucketing performance. component/reading/meaning are the
        // kinds under test here, so those buckets are what get distinct
        // keys per subject.
        Subject s = make_subject(i, "kanji", "");
        s.component_subject_ids = {i + 1000000};                      // distinct component id
        s.readings = {Reading{"r" + std::to_string(i), true, true}};  // distinct reading
        s.meanings = {Meaning{"m" + std::to_string(i), true, true}};  // distinct meaning
        subjects.push_back(std::move(s));
    }

    const auto start = std::chrono::steady_clock::now();
    const std::vector<SimilarityEdge> edges = build_similarity_graph(subjects);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Every comparison key (component id, reading string, meaning
    // string) is unique per subject, so every bucket has exactly one
    // member and no pairwise comparison ever happens for any kind. A
    // correctly bucketed implementation therefore produces zero edges
    // here; a full O(n^2) comparison would likely still produce zero
    // edges too (nothing actually matches), but would take much longer
    // to get there — hence the timing assertion below is the real guard.
    CHECK(edges.empty());
    CHECK(elapsed < std::chrono::seconds(3));
}
