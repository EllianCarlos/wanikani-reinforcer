#pragma once

#include <set>
#include <string>
#include <vector>

#include "model.h"

// Builds a similarity graph over WaniKani subjects: which pairs of
// subjects look alike to a learner, and along which axis (shared
// components/radicals, WaniKani's own visually-similar list, shared
// readings, overlapping meanings, similar character shape). This is a
// pure function of subject data — no DB or HTTP access happens in this
// file, which is what keeps it testable with small hand-built fixture
// vectors. Task 4 combines the returned edges with failure events to rank
// confusion pairs; this file only produces the graph.
//
// Performance note: WaniKani has ~9000 subjects, so an O(n^2) all-pairs
// comparison (81M pairs) is not acceptable. Every edge kind below is
// computed via an inverted index ("bucket": comparison-key -> subject
// indices that share it) so only subjects that land in the same bucket
// are ever compared against each other.
std::vector<SimilarityEdge> build_similarity_graph(const std::vector<Subject>& subjects);

// Jaccard index |a ∩ b| / |a ∪ b| over two sets of subject ids. Returns
// 0.0 when both sets are empty, to avoid a 0/0 division (this never
// actually arises from build_similarity_graph's component pass, since a
// subject with no components never lands in any bucket, but the helper
// itself must not divide by zero if called directly, e.g. from tests).
double jaccard(const std::set<long long>& a, const std::set<long long>& b);

// Number of Unicode codepoints in a UTF-8-encoded string — NOT the byte
// length and NOT a display-width calculation (a fuller terminal
// display-width helper belongs to Task 4's width.cpp; this is just a
// codepoint counter, used only to bucket and compare characters strings
// for the char_shape edge kind).
size_t utf8_codepoint_count(const std::string& s);
