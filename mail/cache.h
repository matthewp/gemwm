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

/* Every message, in any folder, put in a category (by AI or you), newest
 * first: for views, which span folders. */
struct filed {
	char *mailbox;
	struct summary s;          /* its from and subject owned here */
};
GPtrArray *cache_in_category(struct cache *c, const char *name);
void filed_free(gpointer p);
/* The address a cached message is from (lower case), read from its
 * headers; NULL if it isn't here. */
char *cache_from_address(struct cache *c, const char *mailbox, guint32 uid);

/* ---- Bills (see bills.h) ---- */

/* A row of the Bills ledger: a message in Bill, from any folder, and what
 * was read from it (read false: not yet, and the rest's empty). Messages
 * read and found not to be bills aren't there. */
struct ledger_entry {
	char *mailbox;
	struct summary s;          /* its from and subject owned here */
	bool read;
	char *payee, *currency, *due, *period;
	gint64 amount;             /* cents, or -1 */
	bool autopay, paid;
};
GPtrArray *cache_ledger(struct cache *c);
void ledger_entry_free(gpointer p);
/* Bills not yet read (or read by another version), already downloaded,
 * newest first, at most limit (struct filed). */
GPtrArray *cache_bills_unread(struct cache *c, const char *version,
	guint limit);
struct bill;
void cache_set_bill(struct cache *c, const char *mailbox, guint32 uid,
	const struct bill *b, const char *version);
void cache_set_bill_paid(struct cache *c, const char *mailbox, guint32 uid,
	bool paid);

/* A message's text, if it's been fetched before (NULL if not); keeping it. */
GBytes *cache_body(struct cache *c, const char *mailbox, guint32 uid);
void cache_set_body(struct cache *c, const char *mailbox, guint32 uid,
	GBytes *message);
bool cache_has_body(struct cache *c, const char *mailbox, guint32 uid);

#endif
