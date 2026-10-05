/*
 * GemMail's cache: see cache.h.
 */
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "cache.h"

struct cache {
	sqlite3 *db;
	char *dir;
};

static void exec(struct cache *c, const char *sql) {
	char *error = NULL;
	if (c->db != NULL && sqlite3_exec(c->db, sql, NULL, NULL, &error) !=
			SQLITE_OK) {
		g_printerr("gemmail: cache: %s\n", error);
		sqlite3_free(error);
	}
}

static sqlite3_stmt *prepare(struct cache *c, const char *sql) {
	sqlite3_stmt *stmt = NULL;
	if (c->db != NULL && sqlite3_prepare_v2(c->db, sql, -1, &stmt, NULL) !=
			SQLITE_OK) {
		g_printerr("gemmail: cache: %s\n", sqlite3_errmsg(c->db));
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

static void bind_text(sqlite3_stmt *s, int i, const char *text) {
	if (s != NULL) {
		sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
	}
}

struct cache *cache_open(void) {
	struct cache *c = g_new0(struct cache, 1);
	c->dir = g_build_filename(g_get_user_cache_dir(), "gemmail", NULL);
	g_mkdir_with_parents(c->dir, 0700);
	char *path = g_build_filename(c->dir, "cache.sqlite", NULL);
	if (sqlite3_open(path, &c->db) != SQLITE_OK) {
		g_printerr("gemmail: can't open the cache %s\n", path);
		sqlite3_close(c->db);
		c->db = NULL;
	} else {
		g_chmod(path, 0600);
		sqlite3_busy_timeout(c->db, 5000);
		exec(c, "PRAGMA journal_mode = WAL");
		exec(c, "CREATE TABLE IF NOT EXISTS folders ("
			" mailbox TEXT PRIMARY KEY, name TEXT, depth INTEGER,"
			" role INTEGER, selectable INTEGER, messages INTEGER,"
			" unseen INTEGER, position INTEGER)");
		exec(c, "CREATE TABLE IF NOT EXISTS state ("
			" mailbox TEXT PRIMARY KEY, uidvalidity INTEGER,"
			" uidnext INTEGER, modseq INTEGER, exists_ INTEGER)");
		exec(c, "CREATE TABLE IF NOT EXISTS messages ("
			" mailbox TEXT, uid INTEGER, sender TEXT, subject TEXT,"
			" date INTEGER, seen INTEGER, answered INTEGER, flagged INTEGER,"
			" size INTEGER, PRIMARY KEY (mailbox, uid))");
		exec(c, "CREATE TABLE IF NOT EXISTS categories ("
			" mailbox TEXT, uid INTEGER, names TEXT, version TEXT,"
			" manual INTEGER, changed INTEGER, PRIMARY KEY (mailbox, uid))");
	}
	g_free(path);
	return c;
}

void cache_close(struct cache *c) {
	if (c != NULL) {
		sqlite3_close(c->db);
		g_free(c->dir);
		g_free(c);
	}
}

/* ---- Folders ------------------------------------------------------------ */

GPtrArray *cache_folders(struct cache *c) {
	sqlite3_stmt *s = prepare(c, "SELECT mailbox, name, depth, role,"
		" selectable, messages, unseen FROM folders ORDER BY position");
	if (s == NULL) {
		return NULL;
	}
	GPtrArray *folders = g_ptr_array_new();
	while (sqlite3_step(s) == SQLITE_ROW) {
		struct folder *f = g_new0(struct folder, 1);
		f->mailbox = g_strdup((const char *)sqlite3_column_text(s, 0));
		f->name = g_strdup((const char *)sqlite3_column_text(s, 1));
		f->depth = sqlite3_column_int(s, 2);
		f->role = sqlite3_column_int(s, 3);
		f->selectable = sqlite3_column_int(s, 4);
		f->messages = sqlite3_column_int(s, 5);
		f->unseen = sqlite3_column_int(s, 6);
		g_ptr_array_add(folders, f);
	}
	sqlite3_finalize(s);
	if (folders->len == 0) {
		g_ptr_array_free(folders, TRUE);
		return NULL;
	}
	return folders;
}

void cache_set_folders(struct cache *c, GPtrArray *folders) {
	cache_begin(c);
	exec(c, "DELETE FROM folders");
	for (guint i = 0; i < folders->len; i++) {
		struct folder *f = folders->pdata[i];
		sqlite3_stmt *s = prepare(c, "INSERT INTO folders VALUES"
			" (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
		if (s == NULL) {
			break;
		}
		bind_text(s, 1, f->mailbox);
		bind_text(s, 2, f->name);
		sqlite3_bind_int(s, 3, f->depth);
		sqlite3_bind_int(s, 4, f->role);
		sqlite3_bind_int(s, 5, f->selectable);
		sqlite3_bind_int(s, 6, f->messages);
		sqlite3_bind_int(s, 7, f->unseen);
		sqlite3_bind_int(s, 8, i);
		run(s);
	}
	/* Folders gone from the server go from the cache. */
	exec(c, "DELETE FROM messages WHERE mailbox NOT IN"
		" (SELECT mailbox FROM folders)");
	exec(c, "DELETE FROM state WHERE mailbox NOT IN"
		" (SELECT mailbox FROM folders)");
	cache_commit(c);
}

/* ---- Where a folder was left -------------------------------------------- */

bool cache_state(struct cache *c, const char *mailbox, struct folder_state *st) {
	sqlite3_stmt *s = prepare(c, "SELECT uidvalidity, uidnext, modseq,"
		" exists_ FROM state WHERE mailbox = ?1");
	bool found = false;
	if (s != NULL) {
		bind_text(s, 1, mailbox);
		if (sqlite3_step(s) == SQLITE_ROW) {
			st->uidvalidity = (guint32)sqlite3_column_int64(s, 0);
			st->uidnext = (guint32)sqlite3_column_int64(s, 1);
			st->modseq = (guint64)sqlite3_column_int64(s, 2);
			st->exists = (guint32)sqlite3_column_int64(s, 3);
			found = true;
		}
		sqlite3_finalize(s);
	}
	return found;
}

void cache_set_state(struct cache *c, const char *mailbox,
		const struct folder_state *st) {
	sqlite3_stmt *s = prepare(c, "INSERT OR REPLACE INTO state VALUES"
		" (?1, ?2, ?3, ?4, ?5)");
	if (s != NULL) {
		bind_text(s, 1, mailbox);
		sqlite3_bind_int64(s, 2, st->uidvalidity);
		sqlite3_bind_int64(s, 3, st->uidnext);
		sqlite3_bind_int64(s, 4, (sqlite3_int64)st->modseq);
		sqlite3_bind_int64(s, 5, st->exists);
	}
	run(s);
}

static char *folder_dir(struct cache *c, const char *mailbox) {
	char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, mailbox, -1);
	char *dir = g_build_filename(c->dir, "bodies", hash, NULL);
	g_free(hash);
	return dir;
}

void cache_forget_folder(struct cache *c, const char *mailbox) {
	sqlite3_stmt *s = prepare(c, "DELETE FROM messages WHERE mailbox = ?1");
	bind_text(s, 1, mailbox);
	run(s);
	s = prepare(c, "DELETE FROM categories WHERE mailbox = ?1");
	bind_text(s, 1, mailbox);
	run(s);
	s = prepare(c, "DELETE FROM state WHERE mailbox = ?1");
	bind_text(s, 1, mailbox);
	run(s);
	/* Its bodies, all of them. */
	char *dir = folder_dir(c, mailbox);
	GDir *d = g_dir_open(dir, 0, NULL);
	const char *name;
	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		char *path = g_build_filename(dir, name, NULL);
		g_unlink(path);
		g_free(path);
	}
	if (d != NULL) {
		g_dir_close(d);
	}
	g_rmdir(dir);
	g_free(dir);
}

/* ---- Messages ----------------------------------------------------------- */

GPtrArray *cache_messages(struct cache *c, const char *mailbox, guint limit) {
	GPtrArray *list = g_ptr_array_new();
	sqlite3_stmt *s = prepare(c, "SELECT uid, sender, subject, date, seen,"
		" answered, flagged, size FROM messages WHERE mailbox = ?1"
		" ORDER BY date DESC, uid DESC LIMIT ?2");
	if (s == NULL) {
		return list;
	}
	bind_text(s, 1, mailbox);
	sqlite3_bind_int(s, 2, limit);
	while (sqlite3_step(s) == SQLITE_ROW) {
		struct summary *m = g_new0(struct summary, 1);
		m->uid = (guint32)sqlite3_column_int64(s, 0);
		m->from = g_strdup((const char *)sqlite3_column_text(s, 1));
		m->subject = g_strdup((const char *)sqlite3_column_text(s, 2));
		m->date = sqlite3_column_int64(s, 3);
		m->seen = sqlite3_column_int(s, 4);
		m->answered = sqlite3_column_int(s, 5);
		m->flagged = sqlite3_column_int(s, 6);
		m->size = (guint32)sqlite3_column_int64(s, 7);
		if (m->from == NULL) {
			m->from = g_strdup("");
		}
		if (m->subject == NULL) {
			m->subject = g_strdup("");
		}
		g_ptr_array_add(list, m);
	}
	sqlite3_finalize(s);
	return list;
}

GArray *cache_uids(struct cache *c, const char *mailbox) {
	GArray *uids = g_array_new(FALSE, FALSE, sizeof(guint32));
	sqlite3_stmt *s = prepare(c, "SELECT uid FROM messages WHERE mailbox = ?1"
		" ORDER BY uid");
	if (s == NULL) {
		return uids;
	}
	bind_text(s, 1, mailbox);
	while (sqlite3_step(s) == SQLITE_ROW) {
		guint32 uid = (guint32)sqlite3_column_int64(s, 0);
		g_array_append_val(uids, uid);
	}
	sqlite3_finalize(s);
	return uids;
}

void cache_begin(struct cache *c) {
	exec(c, "BEGIN");
}

void cache_commit(struct cache *c) {
	exec(c, "COMMIT");
}

void cache_put(struct cache *c, const char *mailbox, const struct summary *m) {
	sqlite3_stmt *s = prepare(c, "INSERT OR REPLACE INTO messages VALUES"
		" (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
	if (s != NULL) {
		bind_text(s, 1, mailbox);
		sqlite3_bind_int64(s, 2, m->uid);
		bind_text(s, 3, m->from);
		bind_text(s, 4, m->subject);
		sqlite3_bind_int64(s, 5, m->date);
		sqlite3_bind_int(s, 6, m->seen);
		sqlite3_bind_int(s, 7, m->answered);
		sqlite3_bind_int(s, 8, m->flagged);
		sqlite3_bind_int64(s, 9, m->size);
	}
	run(s);
}

void cache_set_flags(struct cache *c, const char *mailbox, guint32 uid,
		bool seen, bool answered, bool flagged) {
	sqlite3_stmt *s = prepare(c, "UPDATE messages SET seen = ?3,"
		" answered = ?4, flagged = ?5 WHERE mailbox = ?1 AND uid = ?2");
	if (s != NULL) {
		bind_text(s, 1, mailbox);
		sqlite3_bind_int64(s, 2, uid);
		sqlite3_bind_int(s, 3, seen);
		sqlite3_bind_int(s, 4, answered);
		sqlite3_bind_int(s, 5, flagged);
	}
	run(s);
}

void cache_set_seen(struct cache *c, const char *mailbox, guint32 uid,
		bool seen) {
	sqlite3_stmt *s = prepare(c, "UPDATE messages SET seen = ?3"
		" WHERE mailbox = ?1 AND uid = ?2");
	if (s != NULL) {
		bind_text(s, 1, mailbox);
		sqlite3_bind_int64(s, 2, uid);
		sqlite3_bind_int(s, 3, seen);
	}
	run(s);
}

static char *body_path(struct cache *c, const char *mailbox, guint32 uid) {
	struct folder_state st;
	if (!cache_state(c, mailbox, &st)) {
		return NULL;
	}
	char *dir = folder_dir(c, mailbox);
	char *name = g_strdup_printf("%u-%u.eml", st.uidvalidity, uid);
	char *path = g_build_filename(dir, name, NULL);
	g_free(name);
	g_free(dir);
	return path;
}

void cache_remove(struct cache *c, const char *mailbox, guint32 uid) {
	char *path = body_path(c, mailbox, uid);
	if (path != NULL) {
		g_unlink(path);
		g_free(path);
	}
	const char *tables[] = { "messages", "categories" };
	for (size_t i = 0; i < G_N_ELEMENTS(tables); i++) {
		char *sql = g_strdup_printf("DELETE FROM %s"
			" WHERE mailbox = ?1 AND uid = ?2", tables[i]);
		sqlite3_stmt *s = prepare(c, sql);
		g_free(sql);
		if (s != NULL) {
			bind_text(s, 1, mailbox);
			sqlite3_bind_int64(s, 2, uid);
		}
		run(s);
	}
}

void cache_trim(struct cache *c, const char *mailbox, guint keep) {
	GArray *uids = cache_uids(c, mailbox);
	for (guint i = 0; i + keep < uids->len; i++) {
		cache_remove(c, mailbox, g_array_index(uids, guint32, i));
	}
	g_array_free(uids, TRUE);
}

/* ---- Categories ----------------------------------------------------------- */

static void categorised_free(gpointer p) {
	struct categorised *k = p;
	g_free(k->names);
	g_free(k->version);
	g_free(k);
}

GHashTable *cache_categorised(struct cache *c, const char *mailbox) {
	GHashTable *t = g_hash_table_new_full(NULL, NULL, NULL, categorised_free);
	sqlite3_stmt *s = prepare(c, "SELECT uid, names, version, manual"
		" FROM categories WHERE mailbox = ?1");
	if (s == NULL) {
		return t;
	}
	bind_text(s, 1, mailbox);
	while (sqlite3_step(s) == SQLITE_ROW) {
		struct categorised *k = g_new0(struct categorised, 1);
		k->names = g_strdup((const char *)sqlite3_column_text(s, 1));
		k->version = g_strdup((const char *)sqlite3_column_text(s, 2));
		k->manual = sqlite3_column_int(s, 3);
		g_hash_table_insert(t, GUINT_TO_POINTER(sqlite3_column_int64(s, 0)), k);
	}
	sqlite3_finalize(s);
	return t;
}

void cache_set_categorised(struct cache *c, const char *mailbox, guint32 uid,
		const char *names, const char *version, bool manual) {
	sqlite3_stmt *s = prepare(c, "INSERT OR REPLACE INTO categories"
		" VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
	if (s == NULL) {
		return;
	}
	bind_text(s, 1, mailbox);
	sqlite3_bind_int64(s, 2, uid);
	bind_text(s, 3, names);
	bind_text(s, 4, version);
	sqlite3_bind_int(s, 5, manual);
	sqlite3_bind_int64(s, 6, g_get_real_time() / G_USEC_PER_SEC);
	run(s);
}

GPtrArray *cache_category_examples(struct cache *c, guint limit) {
	GPtrArray *examples = g_ptr_array_new();
	sqlite3_stmt *s = prepare(c, "SELECT m.sender, m.subject, k.names"
		" FROM categories k JOIN messages m"
		" ON m.mailbox = k.mailbox AND m.uid = k.uid"
		" WHERE k.manual = 1 ORDER BY k.changed DESC LIMIT ?1");
	if (s == NULL) {
		return examples;
	}
	sqlite3_bind_int(s, 1, limit);
	while (sqlite3_step(s) == SQLITE_ROW) {
		struct example *e = g_new0(struct example, 1);
		e->from = g_strdup((const char *)sqlite3_column_text(s, 0));
		e->subject = g_strdup((const char *)sqlite3_column_text(s, 1));
		e->names = g_strdup((const char *)sqlite3_column_text(s, 2));
		g_ptr_array_add(examples, e);
	}
	sqlite3_finalize(s);
	return examples;
}

/* ---- Bodies ------------------------------------------------------------- */

GBytes *cache_body(struct cache *c, const char *mailbox, guint32 uid) {
	char *path = body_path(c, mailbox, uid);
	char *data = NULL;
	gsize len = 0;
	GBytes *bytes = NULL;
	if (path != NULL && g_file_get_contents(path, &data, &len, NULL)) {
		bytes = g_bytes_new_take(data, len);
	}
	g_free(path);
	return bytes;
}

bool cache_has_body(struct cache *c, const char *mailbox, guint32 uid) {
	char *path = body_path(c, mailbox, uid);
	bool has = path != NULL && g_file_test(path, G_FILE_TEST_EXISTS);
	g_free(path);
	return has;
}

void cache_set_body(struct cache *c, const char *mailbox, guint32 uid,
		GBytes *message) {
	char *path = body_path(c, mailbox, uid);
	if (path == NULL) {
		return;
	}
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	gsize len;
	const char *data = g_bytes_get_data(message, &len);
	/* Written whole, then put in place: never half a message. */
	g_file_set_contents_full(path, data, len,
		G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
	g_free(dir);
	g_free(path);
}
