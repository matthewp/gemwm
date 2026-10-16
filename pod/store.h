/*
 * What GemPod knows, in SQLite (~/.local/share/gempod/gempod.sqlite): the
 * podcasts, their episodes with where you got to in each, and what's still
 * to tell the sync server. Used from the main thread only.
 */
#ifndef GEMPOD_STORE_H
#define GEMPOD_STORE_H

#include <glib.h>
#include <stdbool.h>

struct podcast {
	char *url;          /* the feed's, which is the podcast's name in sync */
	char *title, *author, *description, *image, *link;
	char *etag, *modified;  /* for asking for the feed only if it changed */
	gint64 fetched;     /* last read, unix seconds; 0 never */
	int fresh;          /* new episodes (filled in by store_podcasts) */
};

struct episode {
	char *url;          /* its enclosure's, which is its name in sync */
	char *podcast;
	char *guid, *title, *notes, *mime, *image;
	gint64 published;   /* unix seconds */
	int duration;       /* seconds, as the feed says (0: not said) */
	gint64 size;
	int position, total;    /* seconds: where you got to, of how long */
	bool played, fresh;
	gint64 action_ts;   /* when position or played last changed, anywhere */
	char *file;         /* downloaded to, or NULL */
	char *podcast_title;    /* for lists across podcasts */
};

bool store_open(char **error);
void store_close(void);

/* ---- Podcasts ---- */
GPtrArray *store_podcasts(void);                 /* struct podcast, by title */
struct podcast *store_podcast(const char *url);  /* NULL if not subscribed */
bool store_has_podcast(const char *url);
void store_add_podcast(const char *url);         /* subscribed, not yet read */
void store_update_podcast(const struct podcast *p);
void store_remove_podcast(const char *url);      /* and its episodes */
void store_rename_podcast(const char *from, const char *to);
void podcast_free(struct podcast *p);

/* ---- Episodes ---- */
/* podcast NULL: the fresh ones from every podcast, newest first. */
GPtrArray *store_episodes(const char *podcast);
/* Started and not finished, here or on another device (as synced): the
 * ones listened to last first. */
GPtrArray *store_episodes_started(void);
struct episode *store_episode(const char *url);
/* From the feed: added if it's new (fresh unless first is set: a newly
 * added podcast has only its latest fresh), else its feed fields
 * updated, where you got to kept. Returns whether it was new. */
bool store_merge_episode(const struct episode *e, bool first_read);
void store_set_position(const char *url, int position, int total);
void store_set_played(const char *url, bool played);
void store_set_fresh(const char *url, bool fresh);
void store_set_file(const char *url, const char *file);
void store_rename_episode(const char *from, const char *to);
void episode_free(struct episode *e);

/* ---- What's to go to the sync server ---- */
struct action {
	gint64 id;
	char *podcast, *episode, *guid, *action;   /* play, download, delete, new */
	gint64 timestamp;
	int started, position, total;               /* play's; -1 not given */
};
/* Queued, and the episode's own action_ts set: a play replaces an earlier
 * play of the same episode still waiting. */
void store_queue_action(const struct episode *e, const char *action,
	int started, int position, int total);
GPtrArray *store_actions(void);                 /* struct action, oldest first */
void store_actions_sent(gint64 up_to_id);
void action_free(struct action *a);

/* Another device's action, applied if it's newer than what's known; for an
 * episode not read yet, kept until it is. */
void store_apply_remote(const char *episode, const char *podcast,
	const char *action, gint64 ts, int position, int total);

/* Subscriptions changed here, waiting to be sent: subscribed or not. */
void store_queue_subscription(const char *url, bool subscribed);
/* url -> "1" (add) or "0" (remove). */
GHashTable *store_subscription_changes(void);
void store_subscriptions_sent(void);
bool store_subscription_pending(const char *url);

/* Settings that aren't settings: sync timestamps and the like. */
char *store_get(const char *key);
gint64 store_get_int(const char *key);
void store_set(const char *key, const char *value);
void store_set_int(const char *key, gint64 value);

#endif
