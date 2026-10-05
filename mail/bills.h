/*
 * What's read from bills, with AI (through Augur, as categories are), for
 * the Bills ledger: who it's from, how much, when it's due, and whether
 * it's paid automatically. Only mail in the Bill category is read, a few
 * messages at a time, from what's already downloaded. Whether you've paid
 * is yours to tick, in the ledger; it's kept in the cache with the rest.
 */
#ifndef GEMWM_MAIL_BILLS_H
#define GEMWM_MAIL_BILLS_H

#include <glib.h>
#include <stdbool.h>

/* Changes when what's asked for does, so bills are read again. */
#define BILLS_VERSION "bills-1"

/* One message to read. */
struct to_read {
	char *mailbox;
	guint32 uid;
	char *from;             /* the From line: name and address */
	char *subject;
	gint64 date;            /* sent: for "due in 10 days" */
	char *text;             /* its text, cut to what's sent */
};
void to_read_free(gpointer p);

/* What's read from one. */
struct bill {
	bool is_bill;           /* asks for money; not a notice that it's paid */
	char *payee;
	gint64 amount;          /* in cents (hundredths), or -1: none given */
	char *currency;         /* ISO 4217, e.g. USD; NULL: not said */
	char *due;              /* YYYY-MM-DD, or NULL */
	char *period;           /* what it's for, e.g. "September 2026" */
	bool autopay;           /* it'll be paid without you */
};
void bill_free(gpointer p);

char *bills_system_prompt(void);
/* The batch, each with id its index in it. */
char *bills_user_prompt(GPtrArray *batch);
const char *bills_schema(void);
/* The answer: index in the batch -> struct bill. NULL if it can't be
 * read. */
GHashTable *bills_parse_answer(const char *json);

/* A bill's text, cut to what's sent: longer than for categories, since
 * the amount's often further down. */
char *bills_excerpt(const char *text);

/* An amount as it's shown: "$1,234.50", "12.00 EUR". */
char *bills_format_amount(gint64 cents, const char *currency);

#endif
