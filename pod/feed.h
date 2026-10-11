/*
 * Podcast feeds, fetched and read: RSS 2.0 (with iTunes' tags) and Atom.
 */
#ifndef GEMPOD_FEED_H
#define GEMPOD_FEED_H

#include <libsoup/soup.h>
#include "store.h"

struct feed {
	struct podcast podcast;   /* url, etag and modified filled in */
	GPtrArray *episodes;      /* struct episode, newest first */
	bool unchanged;           /* 304: nothing new since etag/modified */
};

void feed_free(struct feed *f);

/* Fetched (only if it changed since etag/modified, given) and read; done
 * gets the feed, or NULL and why. */
typedef void (*feed_fn)(struct feed *f, const char *error, void *data);
void feed_fetch(SoupSession *session, const char *url, const char *etag,
	const char *modified, feed_fn done, void *data);

/* A feed's text, read (for tests, and OPML-less adding). */
struct feed *feed_parse(const char *url, GBytes *body, char **error);

/* HTML (show notes) as plain text, paragraphs kept. */
char *html_to_text(const char *html);

#endif
