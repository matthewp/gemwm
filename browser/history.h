/*
 * GemWeb's history, for completing addresses: every page visited, in an
 * SQLite database, ranked by how often and how lately it was visited
 * (typing an address counts for more than following a link to it).
 */
#ifndef GEMWEB_HISTORY_H
#define GEMWEB_HISTORY_H

#include <glib.h>
#include <stdbool.h>

struct history_entry {
	char *uri;
	char *title;
};

/* Opens (or creates) the database; without one, history is just empty. */
void history_open(const char *path);

/* A page was visited; typed if its address was typed in. True if it's
 * new to history. */
bool history_visit(const char *uri, bool typed);
void history_set_title(const char *uri, const char *title);

/* Pages for what's being typed: those whose address starts with it first
 * (ignoring https:// and www.), then those whose address or title has
 * every word of it; for no text, the top pages. An array of struct
 * history_entry, free with history_entries_free. */
GPtrArray *history_suggest(const char *text, int limit);
void history_entries_free(GPtrArray *entries);

/* Completes text inline: what it would read completed, as typed (so text
 * is a prefix of it, ignoring case), and the address that means. Without
 * a '/', text completes to a site, else to a page. False if nothing
 * starts with text. */
bool history_complete(const char *text, char **completed, char **uri);

void history_forget(const char *uri);
void history_clear(void);

/* An address without its scheme and "www.", as addresses are matched and
 * shown. */
const char *history_bare(const char *uri);

#endif
