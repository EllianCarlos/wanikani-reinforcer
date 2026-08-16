#pragma once

#include <string_view>

// Total display-column width of a UTF-8 string in a typical monospace
// terminal, i.e. counting most East Asian ("wide") codepoints as 2
// columns and everything else as 1. report.cpp uses this (not byte or
// codepoint length) to pad columns so CJK characters line up with ASCII
// text in a real terminal.
int display_width(std::string_view utf8);
