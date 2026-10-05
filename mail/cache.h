/*
 * GemMail's cache of the server, in ~/.cache/gemmail (yours only): the
 * folders, the newest messages of each (their summaries and flags, in
 * SQLite), where each folder was left (UIDVALIDITY, UIDNEXT and
 * HIGHESTMODSEQ, to ask the server only what's changed), and messages
 * already fetched, each a file. The server says what's true; the cache
 * shows it at once, and works offline. Deleting it only costs a slower
 * start.
 *
 * Each thread opens its own (SQLite in WAL mode: the window reads while
 * the mail thread writes).
 */
#ifndef GEMWM_MAIL_CACHE_H
#define GEMWM_MAIL_CACHE_H

#include <glib.h>
#include <stdbool.h>
#include "imap.h"

struct cache;

struct cache *cache_open(void);
void cache_close(struct cache *c);

/* The folder list, as last seen (struct folder), or NULL; and saving it. */
GPtrArray *cache_folders(struct cache *c);
void cache_set_folders(struct cache *c, GPtrArray *folders);

/* Where a folder was left; false if it's never been seen. */
struct folder_state {
	guint32 uidvalidity, uidnext, exists;
	guint64 modseq; /* 0: the server doesn't do CONDSTORE */
};
bool cache_state(struct cache *c, const char *mailbox, struct folder_state *s);
void cache_set_state(struct cache *c, const char *mailbox,
	const struct folder_state *s);

/* Forgets a folder's messages (its UIDVALIDITY changed). */
void cache_forget_folder(struct cache *c, const char *mailbox);

/* A folder's messages, newest first (struct summary), at most limit. */
GPtrArray *cache_messages(struct cache *c, const char *mailbox, guint limit);
/* Its UIDs, lowest first. */
GArray *cache_uids(struct cache *c, const char *mailbox);

void cache_begin(struct cache *c);
void cache_commit(struct cache *c);
void cache_put(struct cache *c, const char *mailbox, const struct summary *s);
void cache_set_flags(struct cache *c, const char *mailbox, guint32 uid,
	bool seen, bool answered, bool flagged);
void cache_set_seen(struct cache *c, const char *mailbox, guint32 uid,
	bool seen);
void cache_remove(struct cache *c, const char *mailbox, guint32 uid);
/* Keeps only the newest keep (by UID). */
void cache_trim(struct cache *c, const char *mailbox, guint keep);

/* ---- Categories (see categories.h) ---- */

/* What a message was put in: names separated by newlines (empty: none),
 * the version of the categories and model that did it, and whether you did
 * (manual: never redone). */
struct categorised {
	char *names;
	char *version;
	bool manual;
};
/* A folder's, by UID (struct categorised). */
GHashTable *cache_categorised(struct cache *c, const char *mailbox);
void cache_set_categorised(struct cache *c, const char *mailbox, guint32 uid,
	const char *names, const char *version, bool manual);
/* The latest you put in categories yourself, newest first, as examples for
 * the model: each a struct example. */
struct example {
	char *from, *subject, *names;
};
GPtrArray *cache_category_examples(struct cache *c, guint limit);

/* A message's text, if it's been fetched before (NULL if not); keeping it. */
GBytes *cache_body(struct cache *c, const char *mailbox, guint32 uid);
void cache_set_body(struct cache *c, const char *mailbox, guint32 uid,
	GBytes *message);
bool cache_has_body(struct cache *c, const char *mailbox, guint32 uid);

#endif
