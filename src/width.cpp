#include "width.h"

#include <cstdint>

namespace {

// The "wide" codepoint ranges from the plan: East Asian scripts and a
// handful of fullwidth/compatibility blocks. Anything not in one of
// these ranges is a single display column.
bool is_wide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

}  // namespace

int display_width(std::string_view utf8) {
    int width = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if ((c & 0x80) == 0x00) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            // Invalid leading byte: don't crash, count it as one column
            // and advance a single byte so a corrupt string still
            // terminates.
            width += 1;
            ++i;
            continue;
        }

        if (i + len > utf8.size()) {
            // Truncated multi-byte sequence at the end of the string:
            // count the salvageable lead byte as one column and stop.
            width += 1;
            break;
        }

        bool valid_continuation = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cont = static_cast<unsigned char>(utf8[i + k]);
            if ((cont & 0xC0) != 0x80) {
                valid_continuation = false;
                break;
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if (!valid_continuation) {
            // Malformed continuation byte: treat the lead byte as width
            // 1 and advance past just that byte, so a bad byte in the
            // middle of a string doesn't cascade-corrupt the rest of the
            // decode.
            width += 1;
            ++i;
            continue;
        }

        width += is_wide(cp) ? 2 : 1;
        i += len;
    }
    return width;
}
