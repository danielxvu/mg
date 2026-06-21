// bridge.cpp -- C++ implementation of the mg.utf8 extern "C" bridge (task U1).
//
// A regular TU (imports mg.utf8) exposing the plain-C primitives in bridge.h to
// the legacy core. No C++ types cross the boundary.

#include "bridge.h"

#include <string_view>

import mg.utf8;

extern "C" int mg_utf8_decode(const char *s, int len, unsigned int *cp,
                              int *width)
{
    if (s == nullptr || len <= 0)
        return 0;
    auto d = mg::utf8::decode_first(
        std::string_view(s, static_cast<std::size_t>(len)));
    if (cp != nullptr)
        *cp = static_cast<unsigned int>(d.cp);
    if (width != nullptr)
        *width = d.width;
    return d.bytes;
}

extern "C" int mg_utf8_char_width(unsigned int cp)
{
    return mg::utf8::char_width(static_cast<char32_t>(cp));
}

extern "C" int mg_utf8_encode(unsigned int cp, char *out)
{
    if (out == nullptr)
        return 0;
    return mg::utf8::encode(static_cast<char32_t>(cp), out);
}

extern "C" int mg_utf8_is_word(unsigned int cp)
{
    return mg::utf8::is_word(static_cast<char32_t>(cp)) ? 1 : 0;
}

extern "C" unsigned int mg_utf8_toupper(unsigned int cp)
{
    return static_cast<unsigned int>(mg::utf8::to_upper(static_cast<char32_t>(cp)));
}

extern "C" unsigned int mg_utf8_tolower(unsigned int cp)
{
    return static_cast<unsigned int>(mg::utf8::to_lower(static_cast<char32_t>(cp)));
}

extern "C" int mg_utf8_grapheme_break(unsigned int a, unsigned int b)
{
    return mg::utf8::grapheme_break(static_cast<char32_t>(a),
                                    static_cast<char32_t>(b))
               ? 1
               : 0;
}

extern "C" int mg_utf8_grapheme_len(const char *s, int len)
{
    if (s == nullptr || len <= 0)
        return 0;
    return static_cast<int>(
        mg::utf8::grapheme_len(std::string_view(s, static_cast<size_t>(len))));
}

extern "C" int mg_utf8_grapheme_back(const char *s, int pos)
{
    if (s == nullptr || pos <= 0)
        return 0;
    return static_cast<int>(mg::utf8::grapheme_back(
        std::string_view(s, static_cast<size_t>(pos)),
        static_cast<size_t>(pos)));
}
