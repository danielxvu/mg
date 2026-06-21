/*
 * bridge.h -- extern "C" surface of the mg.utf8 codepoint layer (task U1).
 *
 * The legacy C core (display.c column math, basic.c cursor motion -- U2+) will
 * call these to become UTF-8 aware. No C++ types cross the boundary.
 */
#ifndef MG_UTF8_BRIDGE_H
#define MG_UTF8_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Decode one codepoint at s[0..len). Returns bytes consumed (>= 1 for len > 0;
 * 0 if len <= 0). Fills *cp (codepoint) and *width (display columns). An invalid
 * or truncated sequence -> 1 byte, *cp = 0xFFFD, *width = 1. cp/width may be
 * NULL if not wanted. */
int mg_utf8_decode(const char *s, int len, unsigned int *cp, int *width);

/* Display width of a codepoint: 0 (combining), 1, or 2 (wide). */
int mg_utf8_char_width(unsigned int cp);

/* Encode `cp` as UTF-8 into `out` (>= 4 bytes); returns byte count (0 on error). */
int mg_utf8_encode(unsigned int cp, char *out);

/* Is `cp` a word constituent (letter/number/connector)?  1 or 0. */
int mg_utf8_is_word(unsigned int cp);

/* Simple (1:1) Unicode upper/lower case of `cp`; returns `cp` unchanged for
 * non-letters, caseless codepoints, or multi-char mappings (e.g. U+00DF). */
unsigned int mg_utf8_toupper(unsigned int cp);
unsigned int mg_utf8_tolower(unsigned int cp);

/* Is there a grapheme boundary between codepoints `a` and `b`? 1 = boundary
 * (separate clusters), 0 = `b` extends `a` (e.g. a combining mark). */
int mg_utf8_grapheme_break(unsigned int a, unsigned int b);

/* Byte length of the first grapheme cluster in s[0..len) (a base codepoint plus
 * any following combining marks). 0 if len <= 0. */
int mg_utf8_grapheme_len(const char *s, int len);

/* Start byte offset of the grapheme cluster ending at `pos` within s[0..pos)
 * (where the cursor lands one grapheme left of `pos`). 0 if pos <= 0. */
int mg_utf8_grapheme_back(const char *s, int pos);

#ifdef __cplusplus
}
#endif

#endif /* MG_UTF8_BRIDGE_H */
