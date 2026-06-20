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

#ifdef __cplusplus
}
#endif

#endif /* MG_UTF8_BRIDGE_H */
