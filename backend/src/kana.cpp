#include "kana.h"

#include <cctype>
#include <map>
#include <string>

namespace {

const std::map<std::string, std::string>& romaji_table() {
    static const std::map<std::string, std::string> table = {
        {"a", "あ"},   {"i", "い"},   {"u", "う"},   {"e", "え"},   {"o", "お"},
        {"ka", "か"},  {"ki", "き"},  {"ku", "く"},  {"ke", "け"},  {"ko", "こ"},
        {"sa", "さ"},  {"shi", "し"}, {"si", "し"},  {"su", "す"},  {"se", "せ"},  {"so", "そ"},
        {"ta", "た"},  {"chi", "ち"}, {"ti", "ち"},  {"tsu", "つ"}, {"tu", "つ"},  {"te", "て"},  {"to", "と"},
        {"na", "な"},  {"ni", "に"},  {"nu", "ぬ"},  {"ne", "ね"},  {"no", "の"},
        {"ha", "は"},  {"hi", "ひ"},  {"fu", "ふ"},  {"hu", "ふ"},  {"he", "へ"},  {"ho", "ほ"},
        {"ma", "ま"},  {"mi", "み"},  {"mu", "む"},  {"me", "め"},  {"mo", "も"},
        {"ya", "や"},  {"yu", "ゆ"},  {"yo", "よ"},
        {"ra", "ら"},  {"ri", "り"},  {"ru", "る"},  {"re", "れ"},  {"ro", "ろ"},
        {"wa", "わ"},  {"wi", "ゐ"},  {"we", "ゑ"},  {"wo", "を"},
        {"ga", "が"},  {"gi", "ぎ"},  {"gu", "ぐ"},  {"ge", "げ"},  {"go", "ご"},
        {"za", "ざ"},  {"ji", "じ"},  {"zi", "じ"},  {"zu", "ず"},  {"ze", "ぜ"},  {"zo", "ぞ"},
        {"da", "だ"},  {"di", "ぢ"},  {"du", "づ"},  {"de", "で"},  {"do", "ど"},
        {"ba", "ば"},  {"bi", "び"},  {"bu", "ぶ"},  {"be", "べ"},  {"bo", "ぼ"},
        {"pa", "ぱ"},  {"pi", "ぴ"},  {"pu", "ぷ"},  {"pe", "ぺ"},  {"po", "ぽ"},
        {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
        {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"}, {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
        {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"}, {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
        {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
        {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
        {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
        {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
        {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
        {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
        {"ja", "じゃ"},  {"ju", "じゅ"},  {"jo", "じょ"},  {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
        {"zya", "じゃ"}, {"zyu", "じゅ"}, {"zyo", "じょ"},
        {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"},
        {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
        {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
        {"fa", "ふぁ"},  {"fi", "ふぃ"},  {"fe", "ふぇ"},  {"fo", "ふぉ"},
        {"xa", "ぁ"},    {"xi", "ぃ"},    {"xu", "ぅ"},    {"xe", "ぇ"},    {"xo", "ぉ"},
        {"xya", "ゃ"},   {"xyu", "ゅ"},   {"xyo", "ょ"},   {"xtsu", "っ"},  {"xtu", "っ"},
        {"-", "ー"},
    };
    return table;
}

bool is_vowel(char c) {
    return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

bool is_romaji_consonant(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 && !is_vowel(c) && c != 'n';
}

std::string trim_ascii(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string fold_katakana(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 == 0xE3 && i + 2 < s.size()) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            const unsigned code = ((c0 & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
            if (code >= 0x30A1 && code <= 0x30F6) {
                const unsigned folded = code - 0x60;
                out += static_cast<char>(0xE0 | (folded >> 12));
                out += static_cast<char>(0x80 | ((folded >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (folded & 0x3F));
                i += 3;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

}  // namespace

std::string to_hiragana(const std::string& input) {
    std::string lowered = trim_ascii(input);
    for (char& c : lowered) {
        if (static_cast<unsigned char>(c) < 0x80) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    const auto& table = romaji_table();
    std::string out;
    size_t i = 0;
    while (i < lowered.size()) {
        const char c = lowered[i];
        const char next = i + 1 < lowered.size() ? lowered[i + 1] : '\0';
        const char after = i + 2 < lowered.size() ? lowered[i + 2] : '\0';

        if (c == 'n') {
            if (next == '\'') {
                out += "ん";
                i += 2;
                continue;
            }
            if (next == 'n') {
                out += "ん";
                i += is_vowel(after) || after == 'y' ? 1 : 2;
                continue;
            }
            if (next == '\0' || (!is_vowel(next) && next != 'y')) {
                out += "ん";
                i += 1;
                continue;
            }
        }

        if (is_romaji_consonant(c) && (next == c || (c == 't' && next == 'c' && after == 'h'))) {
            out += "っ";
            i += 1;
            continue;
        }

        bool matched = false;
        for (size_t len = 4; len >= 1; --len) {
            if (i + len > lowered.size()) {
                continue;
            }
            const auto it = table.find(lowered.substr(i, len));
            if (it != table.end()) {
                out += it->second;
                i += len;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out += lowered[i++];
        }
    }
    return fold_katakana(out);
}
