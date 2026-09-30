/* spell.h — spell checking over enchant.
 *
 * Enchant is the layer every Linux desktop app shares: it finds whichever
 * backend and dictionaries the distribution installed (hunspell, nuspell,
 * aspell, ...) and keeps one personal word list per language under
 * ~/.config/enchant, so a word added here is known to other programs too.
 * Nothing in this file knows about the document or a toolkit. */
#ifndef WP_SPELL_H
#define WP_SPELL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct WpSpell WpSpell;

/* A checker for a language tag such as "en_US". NULL picks the first of the
 * user's locale languages that has a dictionary. Returns NULL and sets *err
 * (malloc'd) when no dictionary matches. */
WpSpell    *wp_spell_new(const char *lang, char **err);
void        wp_spell_free(WpSpell *sp);
const char *wp_spell_language(WpSpell *sp);

/* True when the dictionary knows the word. */
bool   wp_spell_check(WpSpell *sp, const char *word, size_t len);
/* Corrections, best first; a NULL-terminated vector for g_strfreev, *n its length. */
char **wp_spell_suggest(WpSpell *sp, const char *word, size_t len, size_t *n);
/* Add to the personal word list (kept across runs) or to this session only. */
void   wp_spell_add(WpSpell *sp, const char *word, size_t len);
void   wp_spell_ignore(WpSpell *sp, const char *word, size_t len);

/* Language tags with a dictionary installed, sorted; NULL-terminated for g_strfreev. */
char **wp_spell_languages(size_t *n);

/* Words to check, in order. A word is a run of letters and combining marks;
 * an apostrophe or one of the dictionary's extra characters (see
 * wp_spell_extra_chars) is part of a word only between letters. Runs with a
 * digit in them (dates, model numbers) are skipped. *pos is where to scan
 * from and is advanced past the word found; false when there is none. */
bool        wp_spell_next_word(const char *text, size_t len, const char *extra, size_t *pos, size_t *start, size_t *end);
const char *wp_spell_extra_chars(WpSpell *sp);

#endif
