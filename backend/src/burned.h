#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "model.h"
#include "store.h"
#include "wk_api.h"

constexpr int kWkBurnedStage = 9;
constexpr int kMaxBurnedStage = 4;
constexpr int kDefaultBurnedQuestionCount = 5;

BurnedSrsState advance_burned_srs(const std::optional<BurnedSrsState>& current, long long subject_id,
                                  bool correct, const std::string& now_iso8601);

bool matches_reading(const std::string& input, const Subject& subject);

bool is_unaccepted_reading(const std::string& input, const Subject& subject);

std::set<std::string> resolve_burned_types(const std::string& only, const std::string& exclude);

std::vector<long long> select_due_burned(const std::vector<Assignment>& assignments,
                                         const std::vector<BurnedSrsState>& states,
                                         const std::string& now_iso8601,
                                         const std::set<std::string>& included_types = {});

std::string burned_nothing_due_message(const std::vector<Assignment>& assignments,
                                       const std::vector<BurnedSrsState>& states,
                                       const std::string& now_iso8601,
                                       const std::set<std::string>& included_types = {});

struct BurnedQuestion {
    long long subject_id = 0;
    std::string characters;
    std::string type_label;
    bool is_reading = false;  // false: meaning question
    std::string prompt;       // full text incl. "[Type] " label
};

// `retry` means the reading was valid but not the one WaniKani wants; the
// same question should be asked again (nothing is recorded or counted).
struct BurnedFeedback {
    bool correct = false;
    bool retry = false;
    std::string detail;
};

// Step-wise burned-review engine: meaning, then reading (if any), per
// subject. The burned SRS state is written once, after a subject's last
// question. `all_subjects` and `study_materials` must outlive the session.
class BurnedSession {
public:
    BurnedSession(std::vector<long long> subject_ids, const std::vector<Subject>& all_subjects,
                  const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                  int question_count, std::string now_iso8601 = "");

    // Current question (repeats until answered), or nullopt when finished.
    std::optional<BurnedQuestion> next();
    BurnedFeedback answer(const std::string& line);

    int asked() const { return asked_; }
    int correct() const { return correct_; }

private:
    enum class Phase { Meaning, Reading };
    void finish_subject(bool is_correct);

    std::vector<long long> subject_ids_;
    Store& store_;
    int question_count_;
    std::string now_;
    std::map<long long, const Subject*> subject_by_id_;
    std::map<long long, const wk_api::StudyMaterial*> material_by_id_;
    std::map<long long, BurnedSrsState> state_by_id_;
    size_t cursor_ = 0;
    const Subject* current_ = nullptr;
    Phase phase_ = Phase::Meaning;
    bool meaning_correct_ = false;
    int asked_ = 0;
    int correct_ = 0;
};
