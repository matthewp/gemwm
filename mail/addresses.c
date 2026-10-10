#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gmime/gmime.h>
#include <sqlite3.h>
#include <string.h>
#include "addresses.h"
#include "cache.h"
#include "imap.h"

/* Each address once (lower case): sent, how many times you've sent to it
 * from GemMail; seeded, how often it turned up in what's downloaded (your
 * Sent mail's To and Cc, senders you've answered); last, when, latest;
 * forgotten, Shift+Delete'd, until you send to it again. */
static sqlite3 *db;
static char *own;

void addresses_open(void) {
	char *dir = g_build_filename(g_get_user_data_dir(), "gemmail", NULL);
	g_mkdir_with_parents(dir, 0700);
	char *path = g_build_filename(dir, "addresses.sqlite", NULL);
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		g_printerr("gemmail: can't open %s: %s\n", path, sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
	} else {
		g_chmod(path, 0600);
		sqlite3_busy_timeout(db, 2000);
		sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS addresses ("
			" address TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',"
			" sent INTEGER NOT NULL DEFAULT 0, seeded INTEGER NOT NULL DEFAULT 0,"
			" last INTEGER NOT NULL DEFAULT 0,"
			" forgotten INTEGER NOT NULL DEFAULT 0)", NULL, NULL, NULL);
	}
	g_free(path);
	g_free(dir);
}

void addresses_close(void) {
	if (db != NULL) {
		sqlite3_close(db);
		db = NULL;
	}
	g_clear_pointer(&own, g_free);
}

/* Each mailbox in an address list (groups' members too), as name and
 * lower-case address, to fn. */
static void each_address(InternetAddressList *list,
		void (*fn)(const char *name, const char *address, void *data),
		void *data) {
	int n = list != NULL ? internet_address_list_length(list) : 0;
	for (int i = 0; i < n; i++) {
		InternetAddress *a = internet_address_list_get_address(list, i);
		if (INTERNET_ADDRESS_IS_GROUP(a)) {
			each_address(internet_address_group_get_members(
				INTERNET_ADDRESS_GROUP(a)), fn, data);
			continue;
		}
		const char *addr = internet_address_mailbox_get_addr(
			INTERNET_ADDRESS_MAILBOX(a));
		if (addr == NULL || strchr(addr, '@') == NULL) {
			continue;
		}
		char *lower = g_ascii_strdown(addr, -1);
		const char *name = internet_address_get_name(a);
		fn(name != NULL ? name : "", lower, data);
		g_free(lower);
	}
}

static void learn_one(const char *name, const char *address, void *data) {
	sqlite3_stmt *s;
	if (sqlite3_prepare_v2(db,
			"INSERT INTO addresses (address, name, sent, last)"
			" VALUES (?1, ?2, 1, ?3)"
			" ON CONFLICT (address) DO UPDATE SET sent = sent + 1, last = ?3,"
			" forgotten = 0,"
			" name = CASE WHEN ?2 <> '' THEN ?2 ELSE name END",
			-1, &s, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(s, 1, address, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(s, 3, g_get_real_time() / G_USEC_PER_SEC);
	sqlite3_step(s);
	sqlite3_finalize(s);
}

void addresses_learn(const char *header) {
	if (db == NULL || header == NULL || header[0] == '\0') {
		return;
	}
	InternetAddressList *list = internet_address_list_parse(NULL, header);
	if (list != NULL) {
		each_address(list, learn_one, NULL);
		g_object_unref(list);
	}
}

/* ---- Seeding, from what's downloaded ---------------------------------- */

struct seen {
	char *name;
	int count;
	gint64 last;
};

struct seed {
	GHashTable *found; /* address → struct seen */
	gint64 date;       /* the message's */
};

static void seen_free(gpointer p) {
	struct seen *s = p;
	g_free(s->name);
	g_free(s);
}

static void found_one(const char *name, const char *address, void *data) {
	struct seed *sd = data;
	struct seen *s = g_hash_table_lookup(sd->found, address);
	if (s == NULL) {
		s = g_new0(struct seen, 1);
		s->name = g_strdup("");
		g_hash_table_insert(sd->found, g_strdup(address), s);
	}
	s->count++;
	if (sd->date >= s->last) {
		s->last = sd->date;
		if (name[0] != '\0') {
			g_free(s->name);
			s->name = g_strdup(name);
		}
	}
}

static GMimeMessage *parse(GBytes *raw) {
	gsize len;
	const char *data = g_bytes_get_data(raw, &len);
	GMimeStream *stream = g_mime_stream_mem_new_with_buffer(data, len);
	GMimeParser *parser = g_mime_parser_new_with_stream(stream);
	GMimeMessage *msg = g_mime_parser_construct_message(parser, NULL);
	g_object_unref(parser);
	g_object_unref(stream);
	return msg;
}

/* In a thread, with a cache of its own: every downloaded message in Sent
 * (its To and Cc), and every one answered elsewhere (its sender). */
static void seed_thread(GTask *task, gpointer source, gpointer data,
		GCancellable *cancel) {
	struct seed sd = { g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		seen_free), 0 };
	struct cache *c = cache_open();
	GPtrArray *folders = c != NULL ? cache_folders(c) : NULL;
	for (guint i = 0; folders != NULL && i < folders->len; i++) {
		struct folder *f = folders->pdata[i];
		bool sent = f->role == ROLE_SENT;
		if (f->role == ROLE_TRASH || f->role == ROLE_JUNK ||
				f->role == ROLE_DRAFTS) {
			continue;
		}
		GPtrArray *messages = cache_messages(c, f->mailbox, 1000);
		for (guint j = 0; messages != NULL && j < messages->len; j++) {
			struct summary *s = messages->pdata[j];
			if (!sent && !s->answered) {
				continue;
			}
			GBytes *raw = cache_body(c, f->mailbox, s->uid);
			if (raw == NULL) {
				continue;
			}
			GMimeMessage *msg = parse(raw);
			g_bytes_unref(raw);
			if (msg == NULL) {
				continue;
			}
			sd.date = s->date;
			if (sent) {
				each_address(g_mime_message_get_to(msg), found_one, &sd);
				each_address(g_mime_message_get_cc(msg), found_one, &sd);
			} else {
				each_address(g_mime_message_get_from(msg), found_one, &sd);
			}
			g_object_unref(msg);
		}
		if (messages != NULL) {
			summaries_free(messages);
		}
	}
	if (folders != NULL) {
		folders_free(folders);
	}
	if (c != NULL) {
		cache_close(c);
	}
	g_task_return_pointer(task, sd.found, (GDestroyNotify)g_hash_table_unref);
}

static void seeded(GObject *source, GAsyncResult *res, gpointer data) {
	GHashTable *found = g_task_propagate_pointer(G_TASK(res), NULL);
	if (found == NULL || db == NULL) {
		if (found != NULL) {
			g_hash_table_unref(found);
		}
		return;
	}
	sqlite3_stmt *s;
	if (sqlite3_prepare_v2(db,
			"INSERT INTO addresses (address, name, seeded, last)"
			" VALUES (?1, ?2, ?3, ?4)"
			" ON CONFLICT (address) DO UPDATE SET seeded = ?3,"
			" last = MAX(last, ?4),"
			" name = CASE WHEN name = '' THEN ?2 ELSE name END",
			-1, &s, NULL) == SQLITE_OK) {
		sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
		GHashTableIter it;
		gpointer key, value;
		g_hash_table_iter_init(&it, found);
		while (g_hash_table_iter_next(&it, &key, &value)) {
			struct seen *v = value;
			if (own != NULL && strcmp(key, own) == 0) {
				continue;
			}
			sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(s, 2, v->name, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(s, 3, v->count);
			sqlite3_bind_int64(s, 4, v->last);
			sqlite3_step(s);
			sqlite3_reset(s);
		}
		sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
		sqlite3_finalize(s);
	}
	g_hash_table_unref(found);
}

void addresses_seed(const char *own_address) {
	g_free(own);
	own = own_address != NULL ? g_ascii_strdown(own_address, -1) : NULL;
	if (db == NULL) {
		return;
	}
	GTask *task = g_task_new(NULL, NULL, seeded, NULL);
	g_task_run_in_thread(task, seed_thread);
	g_object_unref(task);
}

/* ---- Suggesting ----------------------------------------------------------- */

/* text for a LIKE, its % _ and \ taken literally. */
static char *like_escape(const char *text) {
	GString *s = g_string_new(NULL);
	for (const char *p = text; *p != '\0'; p++) {
		if (*p == '%' || *p == '_' || *p == '\\') {
			g_string_append_c(s, '\\');
		}
		g_string_append_c(s, *p);
	}
	return g_string_free(s, FALSE);
}

static void known_address_free(gpointer p) {
	struct known_address *a = p;
	g_free(a->name);
	g_free(a->address);
	g_free(a);
}

GPtrArray *addresses_suggest(const char *text, const char *skip, int max) {
	GPtrArray *out = g_ptr_array_new_with_free_func(known_address_free);
	if (db == NULL || text == NULL || text[0] == '\0') {
		return out;
	}
	sqlite3_stmt *s;
	if (sqlite3_prepare_v2(db,
			"SELECT name, address FROM addresses WHERE forgotten = 0 AND"
			" (address LIKE ?1 ESCAPE '\\' OR name LIKE ?1 ESCAPE '\\'"
			"  OR name LIKE ?2 ESCAPE '\\')"
			" ORDER BY sent + seeded DESC, last DESC LIMIT ?3",
			-1, &s, NULL) != SQLITE_OK) {
		return out;
	}
	char *esc = like_escape(text);
	char *start = g_strconcat(esc, "%", NULL);
	char *word = g_strconcat("% ", esc, "%", NULL);
	sqlite3_bind_text(s, 1, start, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(s, 2, word, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(s, 3, max * 3);
	g_free(esc);
	g_free(start);
	g_free(word);
	/* Not you, and not anyone already in the field. */
	char *skipping = g_strconcat(",", skip != NULL ? skip : "", ",", NULL);
	while (sqlite3_step(s) == SQLITE_ROW && (int)out->len < max) {
		const char *address = (const char *)sqlite3_column_text(s, 1);
		char *key = g_strconcat(",", address, ",", NULL);
		bool skip_it = (own != NULL && strcmp(address, own) == 0) ||
			strstr(skipping, key) != NULL;
		g_free(key);
		if (skip_it) {
			continue;
		}
		struct known_address *a = g_new0(struct known_address, 1);
		a->name = g_strdup((const char *)sqlite3_column_text(s, 0));
		a->address = g_strdup(address);
		g_ptr_array_add(out, a);
	}
	g_free(skipping);
	sqlite3_finalize(s);
	return out;
}

void addresses_forget(const char *address) {
	sqlite3_stmt *s;
	if (db == NULL || sqlite3_prepare_v2(db,
			"UPDATE addresses SET forgotten = 1 WHERE address = ?1", -1, &s,
			NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(s, 1, address, -1, SQLITE_TRANSIENT);
	sqlite3_step(s);
	sqlite3_finalize(s);
}

char *known_address_format(const struct known_address *a) {
	if (a->name == NULL || a->name[0] == '\0') {
		return g_strdup(a->address);
	}
	/* Quoted if the name has what an address list would read otherwise. */
	if (strpbrk(a->name, ",;<>@\"()[]:") != NULL) {
		GString *q = g_string_new("\"");
		for (const char *p = a->name; *p != '\0'; p++) {
			if (*p == '"' || *p == '\\') {
				g_string_append_c(q, '\\');
			}
			g_string_append_c(q, *p);
		}
		g_string_append_printf(q, "\" <%s>", a->address);
		return g_string_free(q, FALSE);
	}
	return g_strdup_printf("%s <%s>", a->name, a->address);
}
