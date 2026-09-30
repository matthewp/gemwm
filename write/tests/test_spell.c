/* Spell checking: word splitting needs no dictionary; the rest runs only
 * when an English one is installed, so the suite passes on a bare system. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/spell.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The words of text joined by '|', so a whole split compares as one string. */
static char *words(const char *text, const char *extra)
{
  GString *out = g_string_new(NULL);
  size_t pos = 0, s, e, len = strlen(text);
  while (wp_spell_next_word(text, len, extra, &pos, &s, &e)) {
    if (out->len) g_string_append_c(out, '|');
    g_string_append_len(out, text + s, (gssize)(e - s));
  }
  return g_string_free(out, FALSE);
}

static void expect_words(const char *text, const char *extra, const char *want)
{
  char *got = words(text, extra);
  if (strcmp(got, want)) { printf("FAIL: words of \"%s\": want \"%s\", got \"%s\"\n", text, want, got); failures++; }
  free(got);
}

int main(void)
{
  expect_words("Hello, world!", NULL, "Hello|world");
  expect_words("don't won't 'quoted' rock'n'roll", NULL, "don't|won't|quoted|rock'n'roll");
  expect_words("it\xe2\x80\x99s", NULL, "it\xe2\x80\x99s");                 /* typographic apostrophe */
  expect_words("caf\xc3\xa9 na\xc3\xafve", NULL, "caf\xc3\xa9|na\xc3\xafve");   /* accents are letters */
  expect_words("R2D2 2024 3rd x86_64 plain", NULL, "plain");                /* digits skip the run */
  expect_words("snake_case", NULL, "snake|case");
  expect_words("e-mail", NULL, "e|mail");
  expect_words("e-mail", "-", "e-mail");                                    /* dictionary extra char */
  expect_words("-lead trail-", "-", "lead|trail");                           /* joiners only inside */
  expect_words("", NULL, "");
  expect_words("   ...  ", NULL, "");

  /* positions are byte offsets that resume correctly */
  const char *t = "ab cd";
  size_t pos = 0, s = 0, e = 0;
  CHECK(wp_spell_next_word(t, 5, NULL, &pos, &s, &e) && s == 0 && e == 2 && pos == 2);
  CHECK(wp_spell_next_word(t, 5, NULL, &pos, &s, &e) && s == 3 && e == 5 && pos == 5);
  CHECK(!wp_spell_next_word(t, 5, NULL, &pos, &s, &e));

  size_t n = 0;
  char **langs = wp_spell_languages(&n);
  CHECK(langs != NULL && langs[n] == NULL);
  bool have_en = false;
  for (size_t i = 0; i < n; i++) if (g_str_has_prefix(langs[i], "en")) have_en = true;
  g_strfreev(langs);

  char *err = NULL;
  WpSpell *bad = wp_spell_new("xx_NOWHERE", &err);
  CHECK(bad == NULL && err != NULL);
  free(err);

  if (!have_en) {
    printf("spell ok (no English dictionary installed; dictionary checks skipped)\n");
    return failures ? 1 : 0;
  }
  WpSpell *sp = wp_spell_new("en", &err);
  CHECK(sp != NULL);
  if (!sp) { printf("  %s\n", err); return 1; }
  CHECK(g_str_has_prefix(wp_spell_language(sp), "en"));
  CHECK(wp_spell_check(sp, "hello", 5));
  CHECK(!wp_spell_check(sp, "helo", 4));
  CHECK(wp_spell_check(sp, "hello world", 5));   /* len bounds the word */
  size_t ns = 0;
  char **sug = wp_spell_suggest(sp, "helo", 4, &ns);
  bool has_hello = false;
  for (size_t i = 0; i < ns; i++) if (!strcmp(sug[i], "hello")) has_hello = true;
  CHECK(ns > 0 && has_hello && sug[ns] == NULL);
  g_strfreev(sug);
  wp_spell_ignore(sp, "helo", 4);
  CHECK(wp_spell_check(sp, "helo", 4));
  CHECK(wp_spell_extra_chars(sp) != NULL);
  wp_spell_free(sp);

  printf(failures ? "spell: %d failure(s)\n" : "spell ok\n", failures);
  return failures ? 1 : 0;
}
