/*
 * The mail thread: one IMAP connection (libetpan), used a job at a time
 * off the main thread, connecting when needed and again after an error.
 * Each job's callback runs back on the main thread. SMTP goes through it
 * too, on a connection of its own.
 */
#ifndef GEMWM_MAIL_IMAP_H
#define GEMWM_MAIL_IMAP_H

#include <glib.h>
#include <stdbool.h>
#include "account.h"

enum folder_role {
	ROLE_NONE, ROLE_INBOX, ROLE_DRAFTS, ROLE_SENT, ROLE_ARCHIVE, ROLE_JUNK,
	ROLE_TRASH,
	ROLE_ALL, /* every message (Gmail's All Mail): archive here if no Archive */
};

struct folder {
	char *mailbox; /* the server's name (modified UTF-7) */
	char *name;    /* for people: decoded, the last part of the path */
	int depth;     /* how far down the tree */
	enum folder_role role;
	bool selectable;
	guint32 messages, unseen;
};

struct summary {
	guint32 uid;
	char *from, *subject;
	gint64 date; /* unix time; 0 if none */
	bool seen, answered, flagged;
	guint32 size;
};

struct mail;

/* The password, when the thread needs it: asked on the main thread, why
 * saying why (NULL: nothing's gone wrong, look it up), answered with
 * mail_set_password (or mail_cancel_password). */
typedef void (*mail_password_fn)(const char *why, void *data);

struct mail *mail_new(struct account *account, mail_password_fn password,
	void *data);
void mail_set_password(struct mail *m, const char *password);
void mail_cancel_password(struct mail *m);

/* Folders (struct folder, by the server's order but Inbox first), with
 * their counts. error is NULL on success; the arrays are freed after. */
typedef void (*mail_folders_fn)(GPtrArray *folders, const char *error,
	void *data);
void mail_list_folders(struct mail *m, mail_folders_fn done, void *data);

/* The newest messages in a folder (struct summary, newest first). */
typedef void (*mail_messages_fn)(GPtrArray *messages, const char *error,
	void *data);
void mail_list_messages(struct mail *m, const char *mailbox, guint limit,
	mail_messages_fn done, void *data);

/* A whole message, as sent; marked read on the server if mark_seen. */
typedef void (*mail_message_fn)(GBytes *message, const char *error,
	void *data);
void mail_fetch(struct mail *m, const char *mailbox, guint32 uid,
	bool mark_seen, mail_message_fn done, void *data);

/* Done: NULL error on success. */
typedef void (*mail_done_fn)(const char *error, void *data);
void mail_set_seen(struct mail *m, const char *mailbox, guint32 uid,
	bool seen, mail_done_fn done, void *data);
/* Moves it to another folder (created first if create, and it isn't
 * there); with to NULL, or the same folder, deletes it for good. */
void mail_move(struct mail *m, const char *mailbox, guint32 uid,
	const char *to, bool create, mail_done_fn done, void *data);
/* Sends message (from and to the addresses given) by SMTP, then keeps a
 * copy in sent, if set. */
void mail_send(struct mail *m, GBytes *message, const char *from,
	char **recipients, const char *sent, mail_done_fn done, void *data);

void folders_free(GPtrArray *folders);
void summaries_free(GPtrArray *messages);

#endif
