/* spell.c — see spell.h. */
#include "edit/spell.h"

#include <enchant.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

struct WpSpell {
  EnchantBroker *broker;
  EnchantDict   *dict;
  char          *lang;
};

/* Enchant warns on stderr about every provider plugin whose backend library
 * is not installed (aspell, voikko, ...), which is the normal state of a
 * system with one backend. Those are not errors for us: what matters is
 * whether a dictionary turns up, and that is reported through return values. */
static void quiet(const char *domain, GLogLevelFlags level, const char *message, gpointer user) {}

static EnchantBroker *broker_new(void)
{
  guint h = g_log_set_handler("libenchant", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE, quiet, NULL);
  EnchantBroker *b = enchant_broker_init();
  g_log_remove_handler("libenchant", h);
  return b;
}

static void collect_tag(const char *const tag, const char *const name, const char *const desc,
                        const char *const file, void *user)
{
  GPtrArray *tags = user;
  for (guint i = 0; i < tags->len; i++)
    if (!strcmp(tags->pdata[i], tag)) return;   /* two providers may serve one language */
  g_ptr_array_add(tags, g_strdup(tag));
}

static int cmp_str(gconstpointer a, gconstpointer b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static GPtrArray *list_tags(EnchantBroker *b)
{
  GPtrArray *tags = g_ptr_array_new();
  enchant_broker_list_dicts(b, collect_tag, tags);
  g_ptr_array_sort(tags, cmp_str);
  return tags;
}

char **wp_spell_languages(size_t *n)
{
  EnchantBroker *b = broker_new();
  GPtrArray *tags = list_tags(b);
  enchant_broker_free(b);
  if (n) *n = tags->len;
  g_ptr_array_add(tags, NULL);
  return (char **)g_ptr_array_free(tags, FALSE);
}

/* The installed tag that serves `want`: an exact match, else the first
 * dictionary for the same language ("en" or "en_AU" find "en_US"). */
static char *match_tag(GPtrArray *tags, const char *want)
{
  for (guint i = 0; i < tags->len; i++)
    if (!strcmp(tags->pdata[i], want)) return g_strdup(want);
  g_autofree char *lang = g_strdup(want);
  char *us = strchr(lang, '_');
  if (us) *us = 0;
  size_t n = strlen(lang);
  for (guint i = 0; i < tags->len; i++) {
    const char *t = tags->pdata[i];
    if (!strncmp(t, lang, n) && (t[n] == 0 || t[n] == '_')) return g_strdup(t);
  }
  return NULL;
}

/* Does the user's locale ask for a language we have? g_get_language_names
 * yields e.g. "en_US.UTF-8", "en_US", "en", "C". */
static char *find_locale_tag(GPtrArray *tags)
{
  const char *const *names = g_get_language_names();
  for (size_t i = 0; names[i]; i++) {
    if (!strcmp(names[i], "C") || !strcmp(names[i], "POSIX")) continue;
    g_autofree char *want = g_strdup(names[i]);
    char *dot = strpbrk(want, ".@");
    if (dot) *dot = 0;
    char *tag = match_tag(tags, want);
    if (tag) return tag;
  }
  return NULL;
}

WpSpell *wp_spell_new(const char *lang, char **err)
{
  EnchantBroker *b = broker_new();
  GPtrArray *tags = list_tags(b);
  char *tag = lang && *lang ? match_tag(tags, lang) : find_locale_tag(tags);
  if (!tag) {
    if (err) {
      if (tags->len == 0)
        *err = strdup("No spelling dictionary is installed; GemWrite needs a hunspell dictionary for your language");
      else {
        g_ptr_array_add(tags, NULL);
        g_autofree char *have = g_strjoinv(", ", (char **)tags->pdata);
        *err = lang && *lang
          ? g_strdup_printf("No spelling dictionary for '%s'; installed: %s", lang, have)
          : g_strdup_printf("No spelling dictionary matches the locale; installed: %s", have);
      }
    }
    for (guint i = 0; i < tags->len; i++) g_free(tags->pdata[i]);
    g_ptr_array_free(tags, TRUE);
    enchant_broker_free(b);
    return NULL;
  }
  for (guint i = 0; i < tags->len; i++) g_free(tags->pdata[i]);
  g_ptr_array_free(tags, TRUE);
  EnchantDict *d = enchant_broker_request_dict(b, tag);
  if (!d) {
    if (err) *err = g_strdup_printf("Could not load the '%s' dictionary: %s", tag, enchant_broker_get_error(b));
    g_free(tag);
    enchant_broker_free(b);
    return NULL;
  }
  WpSpell *sp = calloc(1, sizeof *sp);
  sp->broker = b;
  sp->dict = d;
  sp->lang = tag;
  return sp;
}

void wp_spell_free(WpSpell *sp)
{
  if (!sp) return;
  enchant_broker_free_dict(sp->broker, sp->dict);
  enchant_broker_free(sp->broker);
  g_free(sp->lang);
  free(sp);
}

const char *wp_spell_language(WpSpell *sp) { return sp->lang; }

bool wp_spell_check(WpSpell *sp, const char *word, size_t len)
{
  return enchant_dict_check(sp->dict, word, (ssize_t)len) == 0;   /* < 0 is an error: treat as unknown */
}

char **wp_spell_suggest(WpSpell *sp, const char *word, size_t len, size_t *n)
{
  size_t count = 0;
  char **raw = enchant_dict_suggest(sp->dict, word, (ssize_t)len, &count);
  char **out = g_new(char *, count + 1);
  for (size_t i = 0; i < count; i++) out[i] = g_strdup(raw[i]);
  out[count] = NULL;
  if (raw) enchant_dict_free_string_list(sp->dict, raw);
  if (n) *n = count;
  return out;
}

void wp_spell_add(WpSpell *sp, const char *word, size_t len)    { enchant_dict_add(sp->dict, word, (ssize_t)len); }
void wp_spell_ignore(WpSpell *sp, const char *word, size_t len) { enchant_dict_add_to_session(sp->dict, word, (ssize_t)len); }

const char *wp_spell_extra_chars(WpSpell *sp)
{
  const char *s = enchant_dict_get_extra_word_characters(sp->dict);
  return s ? s : "";
}

/* ---- words --------------------------------------------------------------- */

static bool is_letter(gunichar c) { return g_unichar_isalpha(c) || g_unichar_ismark(c); }

static bool is_joiner(gunichar c, const char *extra)
{
  if (c == '\'' || c == 0x2019) return true;   /* ASCII and typographic apostrophes */
  if (!extra) return false;
  for (const char *p = extra; *p; p = g_utf8_next_char(p))
    if (g_utf8_get_char(p) == c) return true;
  return false;
}

static gunichar char_at(const char *text, size_t len, size_t off)
{
  if (off >= len) return 0;
  gunichar c = g_utf8_get_char_validated(text + off, (gssize)(len - off));
  return c == (gunichar)-1 || c == (gunichar)-2 ? 0xFFFD : c;
}

static size_t next_off(const char *text, size_t len, size_t off)
{
  gunichar c = g_utf8_get_char_validated(text + off, (gssize)(len - off));
  if (c == (gunichar)-1 || c == (gunichar)-2) return off + 1;
  return (size_t)(g_utf8_next_char(text + off) - text);
}

bool wp_spell_next_word(const char *text, size_t len, const char *extra, size_t *pos, size_t *start, size_t *end)
{
  size_t i = *pos;
  while (i < len) {
    gunichar c = char_at(text, len, i);
    if (!is_letter(c) && !g_unichar_isdigit(c)) { i = next_off(text, len, i); continue; }
    /* a run of letters and digits, with joiners allowed strictly inside */
    size_t s = i;
    bool digit = false;
    for (;;) {
      c = char_at(text, len, i);
      if (is_letter(c)) { i = next_off(text, len, i); continue; }
      if (g_unichar_isdigit(c)) { digit = true; i = next_off(text, len, i); continue; }
      if (is_joiner(c, extra)) {
        size_t after = next_off(text, len, i);
        gunichar d = char_at(text, len, after);
        if (is_letter(d) || g_unichar_isdigit(d)) { i = after; continue; }
      }
      break;
    }
    if (!digit) { *start = s; *end = i; *pos = i; return true; }
  }
  *pos = i;
  return false;
}
