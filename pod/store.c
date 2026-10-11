/*
 * GemPod's database: see store.h.
 */
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include "store.h"

static sqlite3 *db;

static const char schema[] =
	"CREATE TABLE IF NOT EXISTS podcasts ("
	"  url TEXT PRIMARY KEY, title TEXT, author TEXT, description TEXT,"
	"  image TEXT, link TEXT, etag TEXT, modified TEXT,"
	"  fetched INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL DEFAULT 0);"
	"CREATE TABLE IF NOT EXISTS episodes ("
	"  url TEXT PRIMARY KEY, podcast TEXT NOT NULL, guid TEXT, title TEXT,"
	"  notes TEXT, mime TEXT, image TEXT, published INTEGER NOT NULL DEFAULT 0,"
	"  duration INTEGER NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0,"
	"  position INTEGER NOT NULL DEFAULT 0, total INTEGER NOT NULL DEFAULT 0,"
	"  played INTEGER NOT NULL DEFAULT 0, fresh INTEGER NOT NULL DEFAULT 0,"
	"  action_ts INTEGER NOT NULL DEFAULT 0, file TEXT);"
	"CREATE INDEX IF NOT EXISTS episodes_by_podcast ON episodes (podcast, published);"
	"CREATE TABLE IF NOT EXISTS actions ("
	"  id INTEGER PRIMARY KEY, podcast TEXT, episode TEXT, guid TEXT, action TEXT,"
	"  ts INTEGER, started INTEGER, position INTEGER, total INTEGER);"
	"CREATE TABLE IF NOT EXISTS subscription_changes ("
	"  url TEXT PRIMARY KEY, subscribed INTEGER NOT NULL);"
	"CREATE TABLE IF NOT EXISTS remote ("
	"  episode TEXT PRIMARY KEY, action TEXT, ts INTEGER, position INTEGER,"
	"  total INTEGER);"
	"CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT);";

bool store_open(char **error) {
	char *dir = g_build_filename(g_get_user_data_dir(), "gempod", NULL);
	g_mkdir_with_parents(dir, 0700);
	char *path = g_build_filename(dir, "gempod.sqlite", NULL);
	int r = sqlite3_open(path, &db);
	g_free(path);
	g_free(dir);
	char *msg = NULL;
	if (r == SQLITE_OK) {
		sqlite3_busy_timeout(db, 2000);
		r = sqlite3_exec(db, schema, NULL, NULL, &msg);
	}
	if (r != SQLITE_OK) {
		*error = g_strdup(msg != NULL ? msg : sqlite3_errmsg(db));
		sqlite3_free(msg);
		sqlite3_close(db);
		db = NULL;
		return false;
	}
	return true;
}

void store_close(void) {
	if (db != NULL) {
		sqlite3_close(db);
		db = NULL;
	}
}

/* ---- Small things ---- */

static sqlite3_stmt *prepare(const char *sql) {
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		g_warning("gempod: %s: %s", sql, sqlite3_errmsg(db));
	}
	return st;
}

static void bind_text(sqlite3_stmt *st, int i, const char *s) {
	if (s != NULL) {
		sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
	} else {
		sqlite3_bind_null(st, i);
	}
}

static char *column_text(sqlite3_stmt *st, int i) {
	const unsigned char *s = sqlite3_column_text(st, i);
	return s != NULL ? g_strdup((const char *)s) : NULL;
}

static void exec1(const char *sql, const char *a, const char *b) {
	sqlite3_stmt *st = prepare(sql);
	if (st == NULL) {
		return;
	}
	bind_text(st, 1, a);
	if (b != NULL) {
		bind_text(st, 2, b);
	}
	sqlite3_step(st);
	sqlite3_finalize(st);
}

static gint64 now(void) {
	return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---- Podcasts ---- */

void podcast_free(struct podcast *p) {
	if (p == NULL) {
		return;
	}
	g_free(p->url);
	g_free(p->title);
	g_free(p->author);
	g_free(p->description);
	g_free(p->image);
	g_free(p->link);
	g_free(p->etag);
	g_free(p->modified);
	g_free(p);
}

static struct podcast *podcast_row(sqlite3_stmt *st) {
	struct podcast *p = g_new0(struct podcast, 1);
	p->url = column_text(st, 0);
	p->title = column_text(st, 1);
	p->author = column_text(st, 2);
	p->description = column_text(st, 3);
	p->image = column_text(st, 4);
	p->link = column_text(st, 5);
	p->etag = column_text(st, 6);
	p->modified = column_text(st, 7);
	p->fetched = sqlite3_column_int64(st, 8);
	p->fresh = sqlite3_column_int(st, 9);
	return p;
}

#define PODCAST_COLUMNS "p.url, p.title, p.author, p.description, p.image, " \
	"p.link, p.etag, p.modified, p.fetched, " \
	"(SELECT COUNT(*) FROM episodes e WHERE e.podcast = p.url AND e.fresh)"

GPtrArray *store_podcasts(void) {
	GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)podcast_free);
	sqlite3_stmt *st = prepare("SELECT " PODCAST_COLUMNS " FROM podcasts p "
		"ORDER BY LOWER(COALESCE(NULLIF(p.title, ''), p.url))");
	while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		g_ptr_array_add(a, podcast_row(st));
	}
	sqlite3_finalize(st);
	return a;
}

struct podcast *store_podcast(const char *url) {
	sqlite3_stmt *st = prepare("SELECT " PODCAST_COLUMNS
		" FROM podcasts p WHERE p.url = ?");
	struct podcast *p = NULL;
	bind_text(st, 1, url);
	if (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		p = podcast_row(st);
	}
	sqlite3_finalize(st);
	return p;
}

bool store_has_podcast(const char *url) {
	struct podcast *p = store_podcast(url);
	podcast_free(p);
	return p != NULL;
}

void store_add_podcast(const char *url) {
	sqlite3_stmt *st = prepare("INSERT OR IGNORE INTO podcasts (url, added) "
		"VALUES (?, ?)");
	bind_text(st, 1, url);
	sqlite3_bind_int64(st, 2, now());
	sqlite3_step(st);
	sqlite3_finalize(st);
}

void store_update_podcast(const struct podcast *p) {
	sqlite3_stmt *st = prepare("UPDATE podcasts SET title = ?, author = ?, "
		"description = ?, image = ?, link = ?, etag = ?, modified = ?, "
		"fetched = ? WHERE url = ?");
	bind_text(st, 1, p->title);
	bind_text(st, 2, p->author);
	bind_text(st, 3, p->description);
	bind_text(st, 4, p->image);
	bind_text(st, 5, p->link);
	bind_text(st, 6, p->etag);
	bind_text(st, 7, p->modified);
	sqlite3_bind_int64(st, 8, p->fetched);
	bind_text(st, 9, p->url);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

void store_remove_podcast(const char *url) {
	/* Its downloads go too. */
	sqlite3_stmt *st = prepare("SELECT file FROM episodes WHERE podcast = ? "
		"AND file IS NOT NULL");
	bind_text(st, 1, url);
	while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		g_unlink((const char *)sqlite3_column_text(st, 0));
	}
	sqlite3_finalize(st);
	exec1("DELETE FROM episodes WHERE podcast = ?", url, NULL);
	exec1("DELETE FROM podcasts WHERE url = ?", url, NULL);
}

void store_rename_podcast(const char *from, const char *to) {
	if (to[0] == '\0' || strcmp(from, to) == 0) {
		return;
	}
	exec1("UPDATE OR IGNORE podcasts SET url = ?2 WHERE url = ?1", from, to);
	exec1("UPDATE episodes SET podcast = ?2 WHERE podcast = ?1", from, to);
}

/* ---- Episodes ---- */

void episode_free(struct episode *e) {
	if (e == NULL) {
		return;
	}
	g_free(e->url);
	g_free(e->podcast);
	g_free(e->guid);
	g_free(e->title);
	g_free(e->notes);
	g_free(e->mime);
	g_free(e->image);
	g_free(e->file);
	g_free(e->podcast_title);
	g_free(e);
}

#define EPISODE_COLUMNS "e.url, e.podcast, e.guid, e.title, e.notes, e.mime, " \
	"e.image, e.published, e.duration, e.size, e.position, e.total, e.played, " \
	"e.fresh, e.action_ts, e.file, p.title"

static struct episode *episode_row(sqlite3_stmt *st) {
	struct episode *e = g_new0(struct episode, 1);
	e->url = column_text(st, 0);
	e->podcast = column_text(st, 1);
	e->guid = column_text(st, 2);
	e->title = column_text(st, 3);
	e->notes = column_text(st, 4);
	e->mime = column_text(st, 5);
	e->image = column_text(st, 6);
	e->published = sqlite3_column_int64(st, 7);
	e->duration = sqlite3_column_int(st, 8);
	e->size = sqlite3_column_int64(st, 9);
	e->position = sqlite3_column_int(st, 10);
	e->total = sqlite3_column_int(st, 11);
	e->played = sqlite3_column_int(st, 12);
	e->fresh = sqlite3_column_int(st, 13);
	e->action_ts = sqlite3_column_int64(st, 14);
	e->file = column_text(st, 15);
	e->podcast_title = column_text(st, 16);
	return e;
}

GPtrArray *store_episodes(const char *podcast) {
	GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)episode_free);
	sqlite3_stmt *st = prepare(podcast != NULL ?
		"SELECT " EPISODE_COLUMNS " FROM episodes e JOIN podcasts p "
		"ON p.url = e.podcast WHERE e.podcast = ? ORDER BY e.published DESC" :
		"SELECT " EPISODE_COLUMNS " FROM episodes e JOIN podcasts p "
		"ON p.url = e.podcast WHERE e.fresh ORDER BY e.published DESC");
	if (podcast != NULL) {
		bind_text(st, 1, podcast);
	}
	while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		g_ptr_array_add(a, episode_row(st));
	}
	sqlite3_finalize(st);
	return a;
}

struct episode *store_episode(const char *url) {
	sqlite3_stmt *st = prepare("SELECT " EPISODE_COLUMNS " FROM episodes e "
		"LEFT JOIN podcasts p ON p.url = e.podcast WHERE e.url = ?");
	struct episode *e = NULL;
	bind_text(st, 1, url);
	if (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		e = episode_row(st);
	}
	sqlite3_finalize(st);
	return e;
}

/* What another device said about it before it was read here. */
static void apply_kept_remote(const char *url) {
	sqlite3_stmt *st = prepare("SELECT action, ts, position, total FROM remote "
		"WHERE episode = ?");
	bind_text(st, 1, url);
	if (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		char *action = column_text(st, 0);
		gint64 ts = sqlite3_column_int64(st, 1);
		int position = sqlite3_column_int(st, 2), total = sqlite3_column_int(st, 3);
		sqlite3_finalize(st);
		exec1("DELETE FROM remote WHERE episode = ?", url, NULL);
		store_apply_remote(url, NULL, action, ts, position, total);
		g_free(action);
		return;
	}
	sqlite3_finalize(st);
}

bool store_merge_episode(const struct episode *e, bool first_read) {
	struct episode *had = store_episode(e->url);
	if (had == NULL) {
		sqlite3_stmt *st = prepare("INSERT INTO episodes (url, podcast, guid, "
			"title, notes, mime, image, published, duration, size, fresh) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
		bind_text(st, 1, e->url);
		bind_text(st, 2, e->podcast);
		bind_text(st, 3, e->guid);
		bind_text(st, 4, e->title);
		bind_text(st, 5, e->notes);
		bind_text(st, 6, e->mime);
		bind_text(st, 7, e->image);
		sqlite3_bind_int64(st, 8, e->published);
		sqlite3_bind_int(st, 9, e->duration);
		sqlite3_bind_int64(st, 10, e->size);
		sqlite3_bind_int(st, 11, !first_read || e->fresh);
		sqlite3_step(st);
		sqlite3_finalize(st);
		apply_kept_remote(e->url);
		return true;
	}
	sqlite3_stmt *st = prepare("UPDATE episodes SET guid = ?, title = ?, "
		"notes = ?, mime = ?, image = ?, published = ?, duration = ?, size = ? "
		"WHERE url = ?");
	bind_text(st, 1, e->guid);
	bind_text(st, 2, e->title);
	bind_text(st, 3, e->notes);
	bind_text(st, 4, e->mime);
	bind_text(st, 5, e->image);
	sqlite3_bind_int64(st, 6, e->published);
	sqlite3_bind_int(st, 7, e->duration);
	sqlite3_bind_int64(st, 8, e->size);
	bind_text(st, 9, e->url);
	sqlite3_step(st);
	sqlite3_finalize(st);
	episode_free(had);
	return false;
}

void store_set_position(const char *url, int position, int total) {
	sqlite3_stmt *st = prepare("UPDATE episodes SET position = ?, "
		"total = CASE WHEN ? > 0 THEN ? ELSE total END WHERE url = ?");
	sqlite3_bind_int(st, 1, position);
	sqlite3_bind_int(st, 2, total);
	sqlite3_bind_int(st, 3, total);
	bind_text(st, 4, url);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

void store_set_played(const char *url, bool played) {
	sqlite3_stmt *st = prepare(played ?
		"UPDATE episodes SET played = 1, fresh = 0, position = 0 WHERE url = ?" :
		"UPDATE episodes SET played = 0, position = 0 WHERE url = ?");
	bind_text(st, 1, url);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

void store_set_fresh(const char *url, bool fresh) {
	exec1(fresh ? "UPDATE episodes SET fresh = 1 WHERE url = ?" :
		"UPDATE episodes SET fresh = 0 WHERE url = ?", url, NULL);
}

void store_set_file(const char *url, const char *file) {
	exec1("UPDATE episodes SET file = ?2 WHERE url = ?1", url, file);
}

void store_rename_episode(const char *from, const char *to) {
	if (to[0] == '\0' || strcmp(from, to) == 0) {
		return;
	}
	exec1("UPDATE OR IGNORE episodes SET url = ?2 WHERE url = ?1", from, to);
	exec1("UPDATE actions SET episode = ?2 WHERE episode = ?1", from, to);
}

/* ---- Actions ---- */

void action_free(struct action *a) {
	if (a == NULL) {
		return;
	}
	g_free(a->podcast);
	g_free(a->episode);
	g_free(a->guid);
	g_free(a->action);
	g_free(a);
}

void store_queue_action(const struct episode *e, const char *action,
		int started, int position, int total) {
	gint64 ts = now();
	if (strcmp(action, "play") == 0) {
		exec1("DELETE FROM actions WHERE episode = ? AND action = 'play'",
			e->url, NULL);
	}
	sqlite3_stmt *st = prepare("INSERT INTO actions (podcast, episode, guid, "
		"action, ts, started, position, total) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
	bind_text(st, 1, e->podcast);
	bind_text(st, 2, e->url);
	bind_text(st, 3, e->guid);
	bind_text(st, 4, action);
	sqlite3_bind_int64(st, 5, ts);
	sqlite3_bind_int(st, 6, started);
	sqlite3_bind_int(st, 7, position);
	sqlite3_bind_int(st, 8, total);
	sqlite3_step(st);
	sqlite3_finalize(st);
	st = prepare("UPDATE episodes SET action_ts = ? WHERE url = ?");
	sqlite3_bind_int64(st, 1, ts);
	bind_text(st, 2, e->url);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

GPtrArray *store_actions(void) {
	GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)action_free);
	sqlite3_stmt *st = prepare("SELECT id, podcast, episode, guid, action, ts, "
		"started, position, total FROM actions ORDER BY id");
	while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		struct action *x = g_new0(struct action, 1);
		x->id = sqlite3_column_int64(st, 0);
		x->podcast = column_text(st, 1);
		x->episode = column_text(st, 2);
		x->guid = column_text(st, 3);
		x->action = column_text(st, 4);
		x->timestamp = sqlite3_column_int64(st, 5);
		x->started = sqlite3_column_int(st, 6);
		x->position = sqlite3_column_int(st, 7);
		x->total = sqlite3_column_int(st, 8);
		g_ptr_array_add(a, x);
	}
	sqlite3_finalize(st);
	return a;
}

void store_actions_sent(gint64 up_to_id) {
	sqlite3_stmt *st = prepare("DELETE FROM actions WHERE id <= ?");
	sqlite3_bind_int64(st, 1, up_to_id);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

/* A play is finished when it's within a few seconds of the end. */
static bool finished(int position, int total) {
	return total > 0 && position >= total - 5;
}

void store_apply_remote(const char *url, const char *podcast,
		const char *action, gint64 ts, int position, int total) {
	struct episode *e = store_episode(url);
	if (e == NULL) {
		/* Not read here yet: kept, the newest of them. */
		sqlite3_stmt *st = prepare("INSERT INTO remote (episode, action, ts, "
			"position, total) VALUES (?, ?, ?, ?, ?) ON CONFLICT (episode) DO "
			"UPDATE SET action = excluded.action, ts = excluded.ts, "
			"position = excluded.position, total = excluded.total "
			"WHERE excluded.ts > remote.ts");
		bind_text(st, 1, url);
		bind_text(st, 2, action);
		sqlite3_bind_int64(st, 3, ts);
		sqlite3_bind_int(st, 4, position);
		sqlite3_bind_int(st, 5, total);
		sqlite3_step(st);
		sqlite3_finalize(st);
		return;
	}
	if (ts <= e->action_ts) {
		episode_free(e);
		return; /* what's here is as new, or newer (or it's our own, back) */
	}
	sqlite3_stmt *st = NULL;
	if (strcmp(action, "play") == 0) {
		bool done = finished(position, total);
		st = prepare("UPDATE episodes SET position = ?, total = CASE WHEN ? > 0 "
			"THEN ? ELSE total END, played = ?, fresh = CASE WHEN ? THEN 0 "
			"ELSE fresh END, action_ts = ? WHERE url = ?");
		sqlite3_bind_int(st, 1, done ? 0 : position);
		sqlite3_bind_int(st, 2, total);
		sqlite3_bind_int(st, 3, total);
		sqlite3_bind_int(st, 4, done);
		sqlite3_bind_int(st, 5, done || position > 0);
		sqlite3_bind_int64(st, 6, ts);
		bind_text(st, 7, url);
	} else if (strcmp(action, "new") == 0) {
		st = prepare("UPDATE episodes SET played = 0, position = 0, fresh = 1, "
			"action_ts = ? WHERE url = ?");
		sqlite3_bind_int64(st, 1, ts);
		bind_text(st, 2, url);
	}
	if (st != NULL) {
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
	episode_free(e);
}

/* ---- Subscriptions to send ---- */

void store_queue_subscription(const char *url, bool subscribed) {
	sqlite3_stmt *st = prepare("INSERT OR REPLACE INTO subscription_changes "
		"(url, subscribed) VALUES (?, ?)");
	bind_text(st, 1, url);
	sqlite3_bind_int(st, 2, subscribed);
	sqlite3_step(st);
	sqlite3_finalize(st);
}

GHashTable *store_subscription_changes(void) {
	GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sqlite3_stmt *st = prepare("SELECT url, subscribed FROM subscription_changes");
	while (st != NULL && sqlite3_step(st) == SQLITE_ROW) {
		g_hash_table_insert(h, column_text(st, 0),
			g_strdup(sqlite3_column_int(st, 1) ? "1" : "0"));
	}
	sqlite3_finalize(st);
	return h;
}

void store_subscriptions_sent(void) {
	sqlite3_exec(db, "DELETE FROM subscription_changes", NULL, NULL, NULL);
}

bool store_subscription_pending(const char *url) {
	sqlite3_stmt *st = prepare("SELECT 1 FROM subscription_changes WHERE url = ?");
	bind_text(st, 1, url);
	bool yes = st != NULL && sqlite3_step(st) == SQLITE_ROW;
	sqlite3_finalize(st);
	return yes;
}

/* ---- kv ---- */

char *store_get(const char *key) {
	sqlite3_stmt *st = prepare("SELECT value FROM kv WHERE key = ?");
	bind_text(st, 1, key);
	char *v = st != NULL && sqlite3_step(st) == SQLITE_ROW ? column_text(st, 0) :
		NULL;
	sqlite3_finalize(st);
	return v;
}

gint64 store_get_int(const char *key) {
	char *v = store_get(key);
	gint64 n = v != NULL ? g_ascii_strtoll(v, NULL, 10) : 0;
	g_free(v);
	return n;
}

void store_set(const char *key, const char *value) {
	exec1("INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?)", key, value);
}

void store_set_int(const char *key, gint64 value) {
	char *v = g_strdup_printf("%" G_GINT64_FORMAT, value);
	store_set(key, v);
	g_free(v);
}
