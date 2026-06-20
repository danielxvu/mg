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

extern "C" int mg_utf8_is_word(unsigned int cp)
{
    return mg::utf8::is_word(static_cast<char32_t>(cp)) ? 1 : 0;
}
