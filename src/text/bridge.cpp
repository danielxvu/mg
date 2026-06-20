// bridge.cpp -- C++ implementation of the mg.text extern "C" bridge (C1.5).
//
// A regular TU (imports mg.text) exposing the plain-C predicates in bridge.h to
// the legacy core's chrdef.h macros. The classification table is immutable; the
// only runtime state is the ctags '_'-as-word toggle (one bool), replacing the
// legacy mutation of cinfo['_'].

#include "bridge.h"

import mg.text;

namespace {
bool g_underscore_word = false;
}

extern "C" int mg_text_is_word(int c)
{
    if (c == '_')
        return g_underscore_word ? 1 : 0;
    return mg::text::is_word(c) ? 1 : 0;
}

extern "C" int mg_text_is_ctrl(int c)  { return mg::text::is_ctrl(c) ? 1 : 0; }
extern "C" int mg_text_is_upper(int c) { return mg::text::is_upper(c) ? 1 : 0; }
extern "C" int mg_text_is_lower(int c) { return mg::text::is_lower(c) ? 1 : 0; }
extern "C" int mg_text_is_eosp(int c)  { return mg::text::is_eosp(c) ? 1 : 0; }
extern "C" int mg_text_is_digit(int c) { return mg::text::is_digit(c) ? 1 : 0; }

extern "C" void mg_text_set_underscore_word(int on)
{
    g_underscore_word = on != 0;
}
