#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "model.h"
#include "store.h"
#include "wk_api.h"

namespace {

int run_sync() {
    try {
        Config config = Config::load();
        Store store(config.db_path);

        const std::optional<std::string> cursor = store.get_meta("subjects_cursor");
        const std::string updated_after = cursor.value_or("");

        const std::vector<Subject> subjects = wk_api::fetch_all_subjects(updated_after);

        std::string max_updated_at = updated_after;
        for (const auto& subject : subjects) {
            store.upsert_subject(subject);
            if (subject.data_updated_at > max_updated_at) {
                max_updated_at = subject.data_updated_at;
            }
        }

        if (!max_updated_at.empty()) {
            store.set_meta("subjects_cursor", max_updated_at);
        }

        std::cout << "synced " << subjects.size() << " subjects" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

int run_report() {
    std::cout << "not implemented yet" << std::endl;
    return 0;
}

int run_drill() {
    std::cout << "not implemented yet" << std::endl;
    return 0;
}

void print_usage() {
    std::cerr << "usage: wkr <sync|report|drill>" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string command = argv[1];
    if (command == "sync") {
        return run_sync();
    }
    if (command == "report") {
        return run_report();
    }
    if (command == "drill") {
        return run_drill();
    }

    print_usage();
    return 1;
}
