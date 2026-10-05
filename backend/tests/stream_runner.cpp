#include "stream_runner.h"

namespace {

constexpr const char* kColorReset = "\033[0m";
constexpr const char* kColorGreen = "\033[32m";
constexpr const char* kColorRed = "\033[31m";

std::string colorize(const std::string& text, const char* code, bool use_color) {
    if (!use_color || text.empty()) {
        return text;
    }
    return std::string(code) + text + kColorReset;
}

}  // namespace

void run_drill(const std::vector<ConfusionPair>& pairs, const std::vector<Subject>& all_subjects,
               const std::vector<wk_api::StudyMaterial>& study_materials, Store& store, int question_count,
               std::istream& in, std::ostream& out, bool use_color, const std::string& now_override) {
    if (pairs.empty() || question_count <= 0) {
        out << "Nothing to drill yet -- run 'wkr report' first to build up confusion pairs.\n";
        return;
    }

    DrillSession session(pairs, all_subjects, study_materials, store, question_count, now_override);
    while (const std::optional<DrillQuestion> q = session.next()) {
        out << q->prompt << "\n> ";
        std::string line;
        if (!std::getline(in, line)) {
            break;  // EOF: stop early and report the partial score
        }
        const DrillFeedback fb = session.answer(line);
        if (fb.correct) {
            out << colorize("Correct!", kColorGreen, use_color) << "\n";
        } else {
            out << colorize("Incorrect.", kColorRed, use_color) << " " << fb.detail << "\n";
            out << fb.advice << "\n";
        }
        out << fb.reason << "\n";
    }

    out << session.correct() << "/" << session.asked() << " correct\n";
}

void run_burned_review(const std::vector<long long>& subject_ids, const std::vector<Subject>& all_subjects,
                       const std::vector<wk_api::StudyMaterial>& study_materials, Store& store,
                       int question_count, std::istream& in, std::ostream& out, bool use_color,
                       const std::string& now_iso8601) {
    if (subject_ids.empty() || question_count <= 0) {
        out << "Nothing to review yet.\n";
        return;
    }

    BurnedSession session(subject_ids, all_subjects, study_materials, store, question_count, now_iso8601);
    while (const std::optional<BurnedQuestion> q = session.next()) {
        out << q->prompt << "\n> ";
        std::string line;
        if (!std::getline(in, line)) {
            break;
        }
        const BurnedFeedback fb = session.answer(line);
        if (fb.retry) {
            out << fb.detail << "\n";
        } else if (fb.correct) {
            out << colorize("Correct!", kColorGreen, use_color) << "\n";
        } else {
            out << colorize("Incorrect.", kColorRed, use_color) << " " << fb.detail << "\n";
        }
    }

    out << session.correct() << "/" << session.asked() << " correct\n";
}
