/*
 * bridge.h -- extern "C" surface of the mg.text classification module (C1.5).
 *
 * The legacy C core's chrdef.h ISWORD/ISCTRL/... macros route through these
 * under ENABLE_CPP_UPGRADES, retiring the runtime-mutable global cinfo[256].
 * Predicates return 1/0. The lone mutable bit of the legacy table -- the ctags
 * '_'-as-word toggle -- is exposed as a single setter.
 */
#ifndef MG_TEXT_BRIDGE_H
#define MG_TEXT_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

int mg_text_is_word(int c);   /* word constituent (honours the '_' toggle) */
int mg_text_is_ctrl(int c);
int mg_text_is_upper(int c);
int mg_text_is_lower(int c);
int mg_text_is_eosp(int c);   /* end-of-sentence punctuation */
int mg_text_is_digit(int c);

/* Treat '_' as a word constituent while `on` is non-zero (ctags tag matching). */
void mg_text_set_underscore_word(int on);

#ifdef __cplusplus
}
#endif

#endif /* MG_TEXT_BRIDGE_H */
