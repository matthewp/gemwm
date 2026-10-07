/*
 * Messages, with GMime: taking one apart to show it, and putting one
 * together to send (a new one, a reply or a forward).
 */
#ifndef GEMWM_MAIL_MESSAGE_H
#define GEMWM_MAIL_MESSAGE_H

#include <glib.h>
#include <stdbool.h>

struct attachment {
	char *filename, *type;
	GBytes *data;
};

/* Whether the sender is who they say, as the receiving mail server's
 * Authentication-Results header has it: DMARC passed, or a DKIM signature
 * from the sender's domain did (VERIFIED); the header's there but neither
 * (UNVERIFIED); or there's no such header (UNCHECKED). */
enum sender_check { SENDER_UNCHECKED, SENDER_VERIFIED, SENDER_UNVERIFIED };

struct message {
	char *from, *to, *cc, *subject, *date; /* as shown */
	char *sender;            /* the From address alone, lower case */
	enum sender_check sender_check;
	char *reply_to;                        /* where replies go */
	char *message_id, *references;
	char *html, *text;       /* the body: either, both, or neither */
	GPtrArray *attachments;  /* struct attachment */
	GHashTable *inline_parts; /* Content-ID -> struct attachment (cid:) */
};

struct message *message_parse(GBytes *raw);
void message_free(struct message *m);

/* The body as plain text: the text part, else the HTML's words. */
char *message_body_text(struct message *m);

struct draft {
	char *to, *cc, *subject, *body;
	char *in_reply_to, *references;
	GPtrArray *files;  /* paths of files to attach, or NULL (not owned) */
};

/* A reply to m, to its sender (or with all, everyone but me too), the
 * text quoted; or a forward of it. */
struct draft *draft_reply(struct message *m, const char *me, bool all);
struct draft *draft_forward(struct message *m);
struct draft *draft_new(void);
void draft_free(struct draft *d);

/* The message to send, and who it goes to (every To and Cc address, a
 * NULL-ended array), or NULL and why. */
GBytes *draft_build(struct draft *d, const char *from, char ***recipients,
	char **error);

#endif
