/*
 * The people you write to: every address you send to, learned as you
 * send, and found to start with in what's already downloaded (the To and
 * Cc of your Sent mail, and who sent what you've answered). Suggested as
 * you type in To and Cc, those you write to most and latest first.
 * Kept in ~/.local/share/gemmail/addresses.sqlite (not the cache, which
 * can go).
 */
#ifndef GEMMAIL_ADDRESSES_H
#define GEMMAIL_ADDRESSES_H

#include <glib.h>
#include <stdbool.h>

struct known_address {
	char *name;    /* may be empty */
	char *address; /* lower case */
};

void addresses_open(void);
void addresses_close(void);
/* Sent to everyone in a To or Cc header's value: each counted once more,
 * and remembered again if forgotten. */
void addresses_learn(const char *header);
/* What's already downloaded, looked through in a thread (once a start):
 * own is your address, never suggested. */
void addresses_seed(const char *own);
/* Those starting with text, by address or a word of the name, at most
 * max, leaving out any already in the field (lower case, comma
 * separated in skip); struct known_address. */
GPtrArray *addresses_suggest(const char *text, const char *skip, int max);
/* Not suggested again until you send to it. */
void addresses_forget(const char *address);
/* "Name <address>", or the address. */
char *known_address_format(const struct known_address *a);

#endif
