#ifndef NEOMG_SYNTAX_H
#define NEOMG_SYNTAX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NEOMG_NORMAL=0, NEOMG_KEYWORD, NEOMG_STRING, NEOMG_COMMENT,
       NEOMG_NUMBER, NEOMG_TYPE, NEOMG_FUNCTION, NEOMG_PUNCT };
typedef struct { uint16_t start; uint16_t len; uint8_t kind; } NeomgSpan;
uint8_t neomg_lang_from_path(const char *path);
size_t  neomg_highlight_line(uint8_t lang, const char *text, size_t len,
                             NeomgSpan *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
