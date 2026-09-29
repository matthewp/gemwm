/*
 * GemWeb's history: see history.h. One table, places, a row per address.
 */
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include "history.h"

#define KEEP 5000 /* pages kept; the lowest ranked go first */

/* Frequency weighted by recency ("frecency"): a visit counts for less as
 * the weeks pass, and a typed one for three. */
#define SCORE "((visits + 2.0 * typed) / " \
	"(1.0 + (strftime('%s', 'now') - last_visit) / 604800.0))"

static sqlite3 *db;

const char *history_bare(const char *uri) {
	const char *s = strstr(uri, "://");
	s = s != NULL ? s + 3 : uri;
	if (g_ascii_strncasecmp(s, "www.", 4) == 0) {
		s += 4;
	}
	return s;
}

static bool contains(const char *haystack, const char *folded_needle) {
	if (haystack == NULL) {
		return false;
	}
	char *h = g_utf8_casefold(haystack, -1);
	bool found = strstr(h, folded_needle) != NULL;
	g_free(h);
	return found;
}

/* gemweb_match(bare, title, text): 2 if bare starts with text, 1 if every
 * word of text is in bare or title, else 0. text is already bare. */
static void match(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
	const char *bare = (const char *)sqlite3_value_text(argv[0]);
	const char *title = (const char *)sqlite3_value_text(argv[1]);
	const char *text = (const char *)sqlite3_value_text(argv[2]);
	if (bare == NULL || text == NULL || text[0] == '\0') {
		sqlite3_result_int(ctx, 0);
		return;
	}
	if (g_ascii_strncasecmp(bare, text, strlen(text)) == 0) {
		sqlite3_result_int(ctx, 2);
		return;
	}
	char *folded = g_utf8_casefold(text, -1);
	char **words = g_strsplit_set(folded, " \t", -1);
	int result = 0;
	for (int i = 0; words[i] != NULL; i++) {
		if (words[i][0] == '\0') {
			continue;
		}
		if (!contains(bare, words[i]) && !contains(title, words[i])) {
			result = 0;
			break;
		}
		result = 1;
	}
	g_strfreev(words);
	g_free(folded);
	sqlite3_result_int(ctx, result);
}

static void exec(const char *sql) {
	char *error = NULL;
	if (db != NULL && sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		g_printerr("gemweb: history: %s\n", error);
		sqlite3_free(error);
	}
}

void history_open(const char *path) {
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		g_printerr("gemweb: can't open history %s: %s\n", path,
			sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return;
	}
	/* Where you've been is yours alone. */
	g_chmod(path, 0600);
	/* Other GemWeb windows share this process, but not a second GemWeb
	 * (under another user's bus, say): wait for it rather than fail. */
	sqlite3_busy_timeout(db, 1000);
	sqlite3_create_function(db, "gemweb_match", 3,
		SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, match, NULL, NULL);
	exec("CREATE TABLE IF NOT EXISTS places ("
		" url TEXT PRIMARY KEY,"
		" bare TEXT NOT NULL,"
		" title TEXT,"
		" visits INTEGER NOT NULL DEFAULT 0,"
		" typed INTEGER NOT NULL DEFAULT 0,"
		" last_visit INTEGER NOT NULL)");
	exec("DELETE FROM places WHERE url NOT IN"
		" (SELECT url FROM places ORDER BY " SCORE " DESC"
		" LIMIT " G_STRINGIFY(KEEP) ")");
}

static sqlite3_stmt *prepare(const char *sql) {
	sqlite3_stmt *stmt = NULL;
	if (db != NULL && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		g_printerr("gemweb: history: %s\n", sqlite3_errmsg(db));
		stmt = NULL;
	}
	return stmt;
}

static void run(sqlite3_stmt *stmt) {
	if (stmt != NULL) {
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
}

bool history_visit(const char *uri, bool typed) {
	bool new = true;
	sqlite3_stmt *stmt = prepare("SELECT 1 FROM places WHERE url = ?1");
	if (stmt != NULL) {
		sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
		new = sqlite3_step(stmt) != SQLITE_ROW;
		sqlite3_finalize(stmt);
	}
	stmt = prepare(
		"INSERT INTO places (url, bare, visits, typed, last_visit)"
		" VALUES (?1, ?2, 1, ?3, strftime('%s', 'now'))"
		" ON CONFLICT (url) DO UPDATE SET visits = visits + 1,"
		" typed = typed + excluded.typed, last_visit = excluded.last_visit");
	if (stmt != NULL) {
		sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, history_bare(uri), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 3, typed);
	}
	run(stmt);
	return new;
}

void history_set_title(const char *uri, const char *title) {
	sqlite3_stmt *stmt = prepare("UPDATE places SET title = ?2 WHERE url = ?1");
	if (stmt != NULL) {
		sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
	}
	run(stmt);
}

GPtrArray *history_suggest(const char *text, int limit) {
	GPtrArray *entries = g_ptr_array_new();
	sqlite3_stmt *stmt = prepare(
		"SELECT url, title, gemweb_match(bare, title, ?1) AS m FROM places"
		" WHERE m > 0 OR ?1 = '' ORDER BY m DESC, " SCORE " DESC LIMIT ?2");
	if (stmt == NULL) {
		return entries;
	}
	sqlite3_bind_text(stmt, 1, history_bare(text), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, limit);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		struct history_entry *e = g_new(struct history_entry, 1);
		e->uri = g_strdup((const char *)sqlite3_column_text(stmt, 0));
		e->title = g_strdup((const char *)sqlite3_column_text(stmt, 1));
		g_ptr_array_add(entries, e);
	}
	sqlite3_finalize(stmt);
	return entries;
}

void history_entries_free(GPtrArray *entries) {
	for (guint i = 0; i < entries->len; i++) {
		struct history_entry *e = g_ptr_array_index(entries, i);
		g_free(e->uri);
		g_free(e->title);
		g_free(e);
	}
	g_ptr_array_free(entries, TRUE);
}

bool history_complete(const char *text, char **completed, char **uri) {
	const char *typed = history_bare(text);
	/* Only what could be the start of an address. */
	if (typed[0] == '\0' || strpbrk(typed, " \t") != NULL) {
		return false;
	}
	sqlite3_stmt *stmt = prepare(
		"SELECT url, bare FROM places WHERE gemweb_match(bare, NULL, ?1) = 2"
		" ORDER BY " SCORE " DESC LIMIT 1");
	if (stmt == NULL) {
		return false;
	}
	sqlite3_bind_text(stmt, 1, typed, -1, SQLITE_TRANSIENT);
	bool found = sqlite3_step(stmt) == SQLITE_ROW;
	if (found) {
		const char *url = (const char *)sqlite3_column_text(stmt, 0);
		const char *bare = (const char *)sqlite3_column_text(stmt, 1);
		const char *slash = strchr(bare, '/');
		if (strchr(typed, '/') == NULL && slash != NULL) {
			/* The site: youtube.com/, going to the scheme and host the
			 * page was visited with. */
			const char *host = strstr(url, "://");
			host = host != NULL ? host + 3 : url;
			const char *end = strchr(host, '/');
			*uri = end != NULL ? g_strndup(url, end - url + 1) : g_strdup(url);
			*completed = g_strdup_printf("%s%.*s", text,
				(int)(slash + 1 - bare - strlen(typed)), bare + strlen(typed));
		} else {
			*uri = g_strdup(url);
			*completed = g_strconcat(text, bare + strlen(typed), NULL);
		}
	}
	sqlite3_finalize(stmt);
	return found;
}

void history_forget(const char *uri) {
	sqlite3_stmt *stmt = prepare("DELETE FROM places WHERE url = ?1");
	if (stmt != NULL) {
		sqlite3_bind_text(stmt, 1, uri, -1, SQLITE_TRANSIENT);
	}
	run(stmt);
}

void history_clear(void) {
	exec("DELETE FROM places");
	exec("VACUUM");
}
