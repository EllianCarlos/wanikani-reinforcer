#pragma once

#include <chrono>
#include <optional>
#include <sstream>
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

// A single /review_statistics row as WaniKani reports it at a point in
// time. wkr stores one of these per (subject_id, data_updated_at) pair
// (see stat_snapshot in store.cpp) so consecutive snapshots for the same
// subject can be diffed to detect a failure.
struct ReviewStat {
    long long id = 0;
    long long subject_id = 0;
    std::string subject_type;
    bool hidden = false;
    int meaning_correct = 0, meaning_incorrect = 0;
    int meaning_max_streak = 0, meaning_current_streak = 0;
    int reading_correct = 0, reading_incorrect = 0;
    int reading_max_streak = 0, reading_current_streak = 0;
    int percentage_correct = 0;
    std::string data_updated_at;
};

enum class FailureKind { Meaning, Reading, Both };

// One detected failure: the user got `subject_id` wrong on `kind` at
// `occurred_at`. `session_id` is unset (-1) until the session-grouping
// pass assigns it. `cold_start` marks a failure inferred from a zero
// streak on the first snapshot ever seen for a subject, rather than from
// a counter rising between two snapshots.
//
// `id` is the failure_event table's row id. It is unset (-1) for an
// event not yet round-tripped through the database — `classify_failure`
// below never fills it in, since the row doesn't exist until
// Store::insert_failure_event runs; Store::failure_events_without_session
// fills it in from the SELECT so later code can target the right row via
// Store::assign_failure_event_session.
struct FailureEvent {
    long long id = -1;
    long long subject_id = 0;
    std::string occurred_at;  // = the stat_snapshot's data_updated_at that triggered it
    FailureKind kind = FailureKind::Meaning;
    long long session_id = -1;  // filled in by the session-grouping pass; -1 until then
    bool cold_start = false;
};

struct Session {
    long long id = 0;
    std::string started_at;
    std::string ended_at;
};

// One edge in the similarity graph built by similarity.{h,cpp}: subjects
// `a_id` and `b_id` look alike to a learner along axis `kind`, with
// `weight` capturing how strong that particular signal is. `a_id` is
// always < `b_id` so a pair is never stored/emitted twice under the same
// kind with the endpoints swapped.
enum class EdgeKind { WkVisual, Component, Reading, Meaning, CharShape };

struct SimilarityEdge {
    long long a_id;   // always a_id < b_id
    long long b_id;
    EdgeKind kind;
    double weight;
};

// Parses a WaniKani RFC 3339 timestamp, e.g.
// "2026-08-16T03:14:07.000000Z", into a time_point. WaniKani always
// reports UTC with a literal trailing "Z", so this deliberately does not
// handle other timezone offsets. Returns std::nullopt if `s` doesn't
// match the expected shape rather than throwing, so callers can decide
// how to handle malformed/empty timestamps.
inline std::optional<std::chrono::system_clock::time_point> parse_wk_timestamp(
    const std::string& s) {
    if (s.size() < 20 || s.back() != 'Z') {
        return std::nullopt;
    }
    // Strip the trailing 'Z' and parse the rest, which is either
    // "YYYY-MM-DDTHH:MM:SS" or "YYYY-MM-DDTHH:MM:SS.ffffff".
    std::istringstream in(s.substr(0, s.size() - 1));
    std::chrono::sys_time<std::chrono::microseconds> tp;
    in >> std::chrono::parse("%Y-%m-%dT%H:%M:%S", tp);
    if (in.fail()) {
        return std::nullopt;
    }
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(tp);
}

// Classifies a review-statistics update into zero or one failure events.
//
// If `prev` (the immediately preceding snapshot for the same subject) is
// present, a failure is a *rise* in an incorrect counter: it means the
// user answered again since the last snapshot and got it wrong (a flat
// or falling counter, which WaniKani never produces, is not a failure).
//
// If `prev` is absent (this is the first snapshot wkr has ever seen for
// this subject — "cold start"), there is no counter history to diff
// against, so a current streak of 0 is used as a proxy: it means the
// most recent review on that axis was a miss. Reading streaks are only
// meaningful for kanji/vocabulary (radicals and kana_vocabulary have no
// reading quiz, so their reading streak is always 0 and must not be
// treated as a failure).
//
// A subject can fail on both axes at once, which is reported as
// FailureKind::Both rather than two separate events.
inline std::vector<FailureEvent> classify_failure(const std::optional<ReviewStat>& prev,
                                                    const ReviewStat& curr) {
    bool meaning_failed = false;
    bool reading_failed = false;
    const bool cold_start = !prev.has_value();

    if (prev.has_value()) {
        meaning_failed = curr.meaning_incorrect > prev->meaning_incorrect;
        reading_failed = curr.reading_incorrect > prev->reading_incorrect;
    } else {
        const bool has_reading = curr.subject_type == "kanji" || curr.subject_type == "vocabulary";
        meaning_failed = curr.meaning_current_streak == 0;
        reading_failed = has_reading && curr.reading_current_streak == 0;
    }

    if (!meaning_failed && !reading_failed) {
        return {};
    }

    FailureEvent event;
    event.subject_id = curr.subject_id;
    event.occurred_at = curr.data_updated_at;
    event.session_id = -1;
    event.cold_start = cold_start;
    event.kind = (meaning_failed && reading_failed)
                     ? FailureKind::Both
                     : (meaning_failed ? FailureKind::Meaning : FailureKind::Reading);
    return {event};
}

// Groups a chronologically-sorted list of failure events into sessions:
// a new session starts on the first event, or whenever the gap to the
// previous event's `occurred_at` exceeds `gap_minutes` (a gap of exactly
// `gap_minutes` does NOT start a new session — only a gap strictly
// greater than the threshold does).
//
// Design choice: this function assigns `session_id` on each event in
// `sorted_events` in place, but since no database session id exists yet,
// it uses the 0-based index into the *returned* vector as a placeholder
// (event.session_id == i means "belongs to sessions[i]"). The caller
// (which owns the Store) inserts each returned Session via
// Store::insert_session, gets back a real row id, and remaps: for every
// event whose session_id == i, call
// Store::assign_failure_event_session(event.id, real_id_for_session_i).
// This keeps this function pure/DB-free while still only assigning each
// session_id once.
//
// Events with an unparseable `occurred_at` are treated as if they were
// infinitely far from the previous event (i.e. they always start a new
// session), so malformed data can't silently merge into an unrelated
// session.
inline std::vector<Session> group_into_sessions(std::vector<FailureEvent>& sorted_events,
                                                  int gap_minutes) {
    std::vector<Session> sessions;
    std::optional<std::chrono::system_clock::time_point> prev_time;

    for (auto& event : sorted_events) {
        const auto this_time = parse_wk_timestamp(event.occurred_at);

        bool start_new = sessions.empty();
        if (!start_new && prev_time.has_value() && this_time.has_value()) {
            // Compare the full-precision duration against the threshold
            // directly, rather than truncating the gap down to whole
            // minutes first — a gap of 45 minutes and 1 second must
            // count as ">45 minutes", but duration_cast<minutes> would
            // truncate it to exactly 45 and miss the split.
            start_new = (*this_time - *prev_time) > std::chrono::minutes(gap_minutes);
        } else if (!start_new) {
            // Unparseable timestamp on either side: don't guess, split.
            start_new = true;
        }

        if (start_new) {
            sessions.push_back(Session{static_cast<long long>(sessions.size()),
                                        event.occurred_at, event.occurred_at});
        } else {
            sessions.back().ended_at = event.occurred_at;
        }

        event.session_id = static_cast<long long>(sessions.size()) - 1;
        prev_time = this_time;
    }

    return sessions;
}
