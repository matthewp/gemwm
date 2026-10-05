/*
 * GemMail's cache: see cache.h.
 */
#include <glib/gstdio.h>
#include <gmime/gmime.h>
#include <string.h>
#include <sqlite3.h>
#include "bills.h"
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
		exec(c, "CREATE TABLE IF NOT EXISTS bills ("
			" mailbox TEXT, uid INTEGER, is_bill INTEGER, payee TEXT,"
			" amount INTEGER, currency TEXT, due TEXT, period TEXT,"
			" autopay INTEGER, paid INTEGER DEFAULT 0, version TEXT,"
			" PRIMARY KEY (mailbox, uid))");
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
	s = prepare(c, "DELETE FROM bills WHERE mailbox = ?1");
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
	const char *tables[] = { "messages", "categories", "bills" };
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

void filed_free(gpointer p) {
	struct filed *f = p;
	g_free(f->mailbox);
	g_free(f->s.from);
	g_free(f->s.subject);
	g_free(f);
}

GPtrArray *cache_in_category(struct cache *c, const char *name) {
	GPtrArray *out = g_ptr_array_new_with_free_func(filed_free);
	/* names is newline-separated: match it whole, at either end or between. */
	sqlite3_stmt *s = prepare(c, "SELECT m.mailbox, m.uid, m.sender, m.subject,"
		" m.date, m.seen, m.size FROM messages m JOIN categories k"
		" ON k.mailbox = m.mailbox AND k.uid = m.uid"
		" WHERE instr(char(10) || k.names || char(10), char(10) || ?1 || char(10))"
		" > 0 ORDER BY m.date DESC");
	if (s == NULL) {
		return out;
	}
	bind_text(s, 1, name);
	while (sqlite3_step(s) == SQLITE_ROW) {
		struct filed *f = g_new0(struct filed, 1);
		f->mailbox = g_strdup((const char *)sqlite3_column_text(s, 0));
		f->s.uid = sqlite3_column_int64(s, 1);
		f->s.from = g_strdup((const char *)sqlite3_column_text(s, 2));
		f->s.subject = g_strdup((const char *)sqlite3_column_text(s, 3));
		f->s.date = sqlite3_column_int64(s, 4);
		f->s.seen = sqlite3_column_int(s, 5);
		f->s.size = sqlite3_column_int(s, 6);
		g_ptr_array_add(out, f);
	}
	sqlite3_finalize(s);
	return out;
}

void ledger_entry_free(gpointer p) {
	struct ledger_entry *e = p;
	g_free(e->mailbox);
	g_free(e->s.from);
	g_free(e->s.subject);
	g_free(e->payee);
	g_free(e->currency);
	g_free(e->due);
	g_free(e->period);
	g_free(e);
}

static char *column_text(sqlite3_stmt *s, int i) {
	return g_strdup((const char *)sqlite3_column_text(s, i));
}

GPtrArray *cache_ledger(struct cache *c) {
	GPtrArray *out = g_ptr_array_new_with_free_func(ledger_entry_free);
	sqlite3_stmt *s = prepare(c, "SELECT m.mailbox, m.uid, m.sender, m.subject,"
		" m.date, m.seen, b.payee, b.amount, b.currency, b.due, b.period,"
		" b.autopay, b.paid, b.is_bill IS NOT NULL"
		" FROM messages m JOIN categories k"
		" ON k.mailbox = m.mailbox AND k.uid = m.uid"
		" LEFT JOIN bills b ON b.mailbox = m.mailbox AND b.uid = m.uid"
		" WHERE instr(char(10) || k.names || char(10), char(10) || 'Bill' ||"
		" char(10)) > 0 AND (b.is_bill IS NULL OR b.is_bill = 1)"
		" ORDER BY m.date DESC");
	if (s == NULL) {
		return out;
	}
	while (sqlite3_step(s) == SQLITE_ROW) {
		struct ledger_entry *e = g_new0(struct ledger_entry, 1);
		e->mailbox = column_text(s, 0);
		e->s.uid = sqlite3_column_int64(s, 1);
		e->s.from = column_text(s, 2);
		e->s.subject = column_text(s, 3);
		e->s.date = sqlite3_column_int64(s, 4);
		e->s.seen = sqlite3_column_int(s, 5);
		e->payee = column_text(s, 6);
		e->amount = sqlite3_column_type(s, 7) == SQLITE_NULL ? -1 :
			sqlite3_column_int64(s, 7);
		e->currency = column_text(s, 8);
		e->due = column_text(s, 9);
		e->period = column_text(s, 10);
		e->autopay = sqlite3_column_int(s, 11);
		e->paid = sqlite3_column_int(s, 12);
		e->read = sqlite3_column_int(s, 13);
		g_ptr_array_add(out, e);
	}
	sqlite3_finalize(s);
	return out;
}

GPtrArray *cache_bills_unread(struct cache *c, const char *version,
		guint limit) {
	GPtrArray *out = g_ptr_array_new_with_free_func(filed_free);
	sqlite3_stmt *s = prepare(c, "SELECT m.mailbox, m.uid, m.sender, m.subject,"
		" m.date FROM messages m JOIN categories k"
		" ON k.mailbox = m.mailbox AND k.uid = m.uid"
		" LEFT JOIN bills b ON b.mailbox = m.mailbox AND b.uid = m.uid"
		" WHERE instr(char(10) || k.names || char(10), char(10) || 'Bill' ||"
		" char(10)) > 0 AND (b.version IS NULL OR b.version != ?1)"
		" ORDER BY m.date DESC");
	if (s == NULL) {
		return out;
	}
	bind_text(s, 1, version);
	while (sqlite3_step(s) == SQLITE_ROW && out->len < limit) {
		char *mailbox = column_text(s, 0);
		guint32 uid = sqlite3_column_int64(s, 1);
		if (!cache_has_body(c, mailbox, uid)) {
			g_free(mailbox); /* not fetched yet: when it is */
			continue;
		}
		struct filed *f = g_new0(struct filed, 1);
		f->mailbox = mailbox;
		f->s.uid = uid;
		f->s.from = column_text(s, 2);
		f->s.subject = column_text(s, 3);
		f->s.date = sqlite3_column_int64(s, 4);
		g_ptr_array_add(out, f);
	}
	sqlite3_finalize(s);
	return out;
}

void cache_set_bill(struct cache *c, const char *mailbox, guint32 uid,
		const struct bill *b, const char *version) {
	/* What's read again replaces what was; whether it's paid stays. */
	sqlite3_stmt *s = prepare(c, "INSERT INTO bills (mailbox, uid, is_bill,"
		" payee, amount, currency, due, period, autopay, version)"
		" VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"
		" ON CONFLICT (mailbox, uid) DO UPDATE SET is_bill = ?3, payee = ?4,"
		" amount = ?5, currency = ?6, due = ?7, period = ?8, autopay = ?9,"
		" version = ?10");
	if (s == NULL) {
		return;
	}
	bind_text(s, 1, mailbox);
	sqlite3_bind_int64(s, 2, uid);
	sqlite3_bind_int(s, 3, b->is_bill);
	bind_text(s, 4, b->payee);
	if (b->amount >= 0) {
		sqlite3_bind_int64(s, 5, b->amount);
	} else {
		sqlite3_bind_null(s, 5);
	}
	bind_text(s, 6, b->currency);
	bind_text(s, 7, b->due);
	bind_text(s, 8, b->period);
	sqlite3_bind_int(s, 9, b->autopay);
	bind_text(s, 10, version);
	run(s);
}

void cache_set_bill_paid(struct cache *c, const char *mailbox, guint32 uid,
		bool paid) {
	sqlite3_stmt *s = prepare(c, "UPDATE bills SET paid = ?3"
		" WHERE mailbox = ?1 AND uid = ?2");
	if (s == NULL) {
		return;
	}
	bind_text(s, 1, mailbox);
	sqlite3_bind_int64(s, 2, uid);
	sqlite3_bind_int(s, 3, paid);
	run(s);
}

/* The From header of a message file, unfolded; NULL if there's none. */
static char *from_header(const char *text, gsize len) {
	const char *end = text + len;
	for (const char *line = text; line < end;) {
		const char *nl = memchr(line, '\n', end - line);
		const char *stop = nl != NULL ? nl : end;
		if (stop == line || (stop == line + 1 && *line == '\r')) {
			return NULL; /* the headers end */
		}
		if (g_ascii_strncasecmp(line, "From:", 5) == 0) {
			GString *v = g_string_new_len(line + 5, stop - line - 5);
			/* Continued on lines that start with space. */
			while (nl != NULL && nl + 1 < end && (nl[1] == ' ' || nl[1] == '\t')) {
				const char *next = memchr(nl + 1, '\n', end - nl - 1);
				const char *nstop = next != NULL ? next : end;
				g_string_append_len(v, nl + 1, nstop - nl - 1);
				nl = next;
			}
			return g_string_free(v, FALSE);
		}
		line = nl != NULL ? nl + 1 : end;
	}
	return NULL;
}

char *cache_from_address(struct cache *c, const char *mailbox, guint32 uid) {
	char *path = body_path(c, mailbox, uid);
	if (path == NULL) {
		return NULL;
	}
	/* Only the headers are wanted: the start of the file. */
	char buf[16384];
	FILE *f = g_fopen(path, "rb");
	g_free(path);
	if (f == NULL) {
		return NULL;
	}
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	char *from = from_header(buf, n);
	if (from == NULL) {
		return NULL;
	}
	char *address = NULL;
	InternetAddressList *list = internet_address_list_parse(NULL, from);
	for (int i = 0; list != NULL && i < internet_address_list_length(list) &&
			address == NULL; i++) {
		InternetAddress *a = internet_address_list_get_address(list, i);
		if (INTERNET_ADDRESS_IS_MAILBOX(a)) {
			const char *addr = internet_address_mailbox_get_addr(
				INTERNET_ADDRESS_MAILBOX(a));
			address = addr != NULL ? g_ascii_strdown(addr, -1) : NULL;
		}
	}
	if (list != NULL) {
		g_object_unref(list);
	}
	g_free(from);
	return address;
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
