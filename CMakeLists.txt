cmake_minimum_required(VERSION 3.20)
project(wkr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(nlohmann_json REQUIRED)

add_library(wkr_lib STATIC
    src/config.cpp
    src/http.cpp
    src/wk_api.cpp
    src/store.cpp
    src/similarity.cpp
    src/confusion.cpp
    src/width.cpp
    src/advice.cpp
    src/report.cpp
    src/drill.cpp
)
target_include_directories(wkr_lib PUBLIC src)
target_link_libraries(wkr_lib PUBLIC
    PkgConfig::CURL
    PkgConfig::SQLITE3
    nlohmann_json::nlohmann_json
)

add_executable(wkr src/main.cpp)
target_link_libraries(wkr PRIVATE wkr_lib)

enable_testing()
find_package(Catch2 3 REQUIRED)

add_executable(wkr_tests
    tests/test_model.cpp
    tests/test_store.cpp
    tests/test_failure_detection.cpp
    tests/test_similarity.cpp
    tests/test_width.cpp
    tests/test_confusion.cpp
    tests/test_advice.cpp
    tests/test_drill.cpp
)
target_link_libraries(wkr_tests PRIVATE wkr_lib Catch2::Catch2WithMain)

add_test(NAME wkr_tests COMMAND wkr_tests)
