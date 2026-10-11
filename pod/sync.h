/*
 * Syncing with a gpodder.net-style server (the API v2 that gpodder.net,
 * mygpo, opodsync and barking speak): the subscriptions every device
 * shares, and where you got to in each episode.
 */
#ifndef GEMPOD_SYNC_H
#define GEMPOD_SYNC_H

#include <libsoup/soup.h>
#include <stdbool.h>

struct sync_account {
	char *server;     /* https://host, or a path under it */
	char *username, *password;
	char *device;     /* this computer's id there */
	char *caption;    /* its name there */
};

/* What a sync changed here, for the window to show. */
struct sync_result {
	int added, removed;          /* podcasts */
	int positions;               /* episodes' places from other devices */
	char **new_podcasts;         /* urls subscribed elsewhere, to read now */
};

typedef void (*sync_fn)(const struct sync_result *r, const char *error,
	void *data);

/* Signs in (and says this computer's there): error NULL if it worked. */
void sync_check(SoupSession *session, const struct sync_account *a,
	sync_fn done, void *data);
/* Subscriptions both ways, then episode actions both ways. */
void sync_run(SoupSession *session, const struct sync_account *a,
	sync_fn done, void *data);

/* "host" or "host/" as https://host. */
char *sync_server_url(const char *given);

#endif
