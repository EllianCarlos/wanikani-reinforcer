#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "model.h"
#include "store.h"
#include "wk_api.h"

// The sync pipeline's ordering logic, extracted out of main.cpp so it is
// part of wkr_lib and can be exercised by tests directly. Before this
// existed, tests/test_integration.cpp had to hand-reimplement the same
// ordering, which meant the "end-to-end" test verified a copy of the
// pipeline rather than the pipeline itself — a bug in main.cpp's real
// ordering could not be caught. Both `wkr sync` and the integration test
// now call the exact same run_sync() below.
//
// Everything here is at global scope rather than in a `sync` namespace:
// <unistd.h> declares a POSIX ::sync(), and main.cpp includes both.

// The four network fetches, injected so run_sync() can be driven from a
// test with fixture data and no network at all. Each takes the stored
// cursor (`updated_after`) and returns everything newer than it, exactly
// like the wk_api::fetch_all_* functions it wraps.
struct SyncFetchers {
    std::function<std::vector<Subject>(const std::string&)> subjects;
    std::function<std::vector<ReviewStat>(const std::string&)> review_statistics;
    std::function<std::vector<Assignment>(const std::string&)> assignments;
    std::function<std::vector<wk_api::StudyMaterial>(const std::string&)> study_materials;
};

// Fetchers bound to the real WaniKani endpoints (wk_api::fetch_all_*),
// i.e. the ones `wkr sync` uses. Calling any of them performs HTTP GETs
// and therefore requires a configured API token.
SyncFetchers live_sync_fetchers();

// What one sync run did, for the caller to report to the user.
struct SyncSummary {
    std::size_t subjects = 0;
    std::size_t stats = 0;
    std::size_t assignments = 0;
    std::size_t study_materials = 0;
    int failures = 0;
    // Number of edges written by the similarity-graph rebuild, or -1 when
    // the rebuild was skipped because nothing changed and a graph was
    // already present.
    int similarity_edge_count = -1;
};

// Runs one full sync against `store`, in order: subjects fetch + upsert,
// review-statistics fetch + snapshot/delta/failure detection, assignments,
// study materials, session grouping, similarity-graph rebuild. Cursors in
// `sync_meta` are advanced per resource as each fetch's data lands.
// Idempotent: re-running with the same input produces no duplicate rows.
SyncSummary run_sync(Store& store, const SyncFetchers& fetchers, int session_gap_minutes);
