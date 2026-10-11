/*
 * gpodder.net API v2 sync: see sync.h, and
 * https://gpoddernet.readthedocs.io/en/latest/api/.
 *
 * Every request carries Basic auth (as AntennaPod's do; servers remember a
 * good one). A sync goes:
 *
 *   1. POST devices/USER/DEVICE      this computer, by name
 *   2. GET subscriptions/USER/DEVICE?since=T
 *                                    others' changes, applied here unless
 *                                    there's one of ours waiting for that
 *                                    podcast (ours is newer); the first
 *                                    time, ours that the server lacks are
 *                                    queued to go up
 *   3. POST subscriptions/USER/DEVICE {add, remove}
 *                                    ours, and any URLs it rewrote
 *   4. POST episodes/USER            our plays, news, downloads, deletes
 *   5. GET episodes/USER?since=T     everyone's, applied where newer
 *
 * The timestamps the server gives are kept, to ask from next time. Servers
 * may send our own changes back (since is inclusive): that's harmless.
 */
#include <json-glib/json-glib.h>
#include <string.h>
#include "store.h"
#include "sync.h"

enum step { STEP_DEVICE, STEP_GET_SUBS, STEP_POST_SUBS, STEP_POST_EPISODES,
	STEP_GET_EPISODES, STEP_DONE };

struct job {
	SoupSession *session;
	struct sync_account a;
	bool check_only;
	enum step step;
	sync_fn done;
	void *data;
	SoupMessage *msg;
	struct sync_result result;
	GPtrArray *new_podcasts;
	gint64 last_action;      /* the newest action sent, to clear after */
	gint64 subs_since;       /* from the server's answer */
};

char *sync_server_url(const char *given) {
	char *s = g_strstrip(g_strdup(given != NULL ? given : ""));
	while (g_str_has_suffix(s, "/")) {
		s[strlen(s) - 1] = '\0';
	}
	if (s[0] != '\0' && strstr(s, "://") == NULL) {
		char *with = g_strconcat("https://", s, NULL);
		g_free(s);
		s = with;
	}
	return s;
}

static char *escaped(const char *s) {
	return g_uri_escape_string(s, NULL, FALSE);
}

/* The API's path for what, under the server's. */
static char *api_url(struct job *j, const char *what, bool device,
		const char *query) {
	char *server = sync_server_url(j->a.server);
	char *user = escaped(j->a.username), *dev = escaped(j->a.device);
	char *url = device ?
		g_strdup_printf("%s/api/2/%s/%s/%s.json%s", server, what, user, dev,
			query != NULL ? query : "") :
		g_strdup_printf("%s/api/2/%s/%s.json%s", server, what, user,
			query != NULL ? query : "");
	g_free(server);
	g_free(user);
	g_free(dev);
	return url;
}

static void job_free(struct job *j) {
	g_free(j->a.server);
	g_free(j->a.username);
	g_free(j->a.password);
	g_free(j->a.device);
	g_free(j->a.caption);
	g_clear_object(&j->msg);
	if (j->new_podcasts != NULL) {
		g_ptr_array_unref(j->new_podcasts);
	}
	g_object_unref(j->session);
	g_free(j);
}

static void finish(struct job *j, const char *error) {
	g_ptr_array_add(j->new_podcasts, NULL);
	j->result.new_podcasts = (char **)j->new_podcasts->pdata;
	j->done(&j->result, error, j->data);
	job_free(j);
}

static void next_step(struct job *j);

static void responded(GObject *src, GAsyncResult *res, gpointer data);

static void request(struct job *j, const char *method, char *url,
		JsonNode *body) {
	SoupMessage *msg = soup_message_new(method, url);
	g_free(url);
	if (msg == NULL) {
		finish(j, "the server's address isn't right");
		if (body != NULL) {
			json_node_unref(body);
		}
		return;
	}
	char *pair = g_strdup_printf("%s:%s", j->a.username, j->a.password);
	char *b64 = g_base64_encode((const guchar *)pair, strlen(pair));
	char *auth = g_strconcat("Basic ", b64, NULL);
	SoupMessageHeaders *h = soup_message_get_request_headers(msg);
	soup_message_headers_replace(h, "Authorization", auth);
	soup_message_headers_replace(h, "Accept", "application/json");
	memset(pair, 0, strlen(pair));
	g_free(pair);
	g_free(b64);
	g_free(auth);
	if (body != NULL) {
		char *json = json_to_string(body, FALSE);
		GBytes *bytes = g_bytes_new_take(json, strlen(json));
		soup_message_set_request_body_from_bytes(msg, "application/json", bytes);
		g_bytes_unref(bytes);
		json_node_unref(body);
	}
	g_clear_object(&j->msg);
	j->msg = msg;
	soup_session_send_and_read_async(j->session, msg, G_PRIORITY_DEFAULT, NULL,
		responded, j);
}

/* ---- Applying what comes back ---- */

static const char *string_member(JsonObject *o, const char *name) {
	JsonNode *n = json_object_get_member(o, name);
	return n != NULL && JSON_NODE_HOLDS_VALUE(n) &&
		json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) :
		NULL;
}

static gint64 int_member(JsonObject *o, const char *name, gint64 otherwise) {
	JsonNode *n = json_object_get_member(o, name);
	if (n == NULL || !JSON_NODE_HOLDS_VALUE(n)) {
		return otherwise;
	}
	GType t = json_node_get_value_type(n);
	if (t == G_TYPE_INT64) {
		return json_node_get_int(n);
	}
	if (t == G_TYPE_DOUBLE) {
		return (gint64)json_node_get_double(n);
	}
	if (t == G_TYPE_STRING) {
		return g_ascii_strtoll(json_node_get_string(n), NULL, 10);
	}
	return otherwise;
}

/* [[old, new], ...]: URLs the server cleaned up, renamed here too. */
static void update_urls(JsonObject *o, bool episodes) {
	JsonNode *n = json_object_get_member(o, "update_urls");
	JsonArray *a = n != NULL && JSON_NODE_HOLDS_ARRAY(n) ?
		json_node_get_array(n) : NULL;
	for (guint i = 0; a != NULL && i < json_array_get_length(a); i++) {
		JsonNode *pair = json_array_get_element(a, i);
		if (!JSON_NODE_HOLDS_ARRAY(pair) ||
				json_array_get_length(json_node_get_array(pair)) != 2) {
			continue;
		}
		const char *from = json_array_get_string_element(json_node_get_array(pair), 0);
		const char *to = json_array_get_string_element(json_node_get_array(pair), 1);
		if (from == NULL || to == NULL || to[0] == '\0') {
			continue;
		}
		if (episodes) {
			store_rename_episode(from, to);
		} else {
			store_rename_podcast(from, to);
		}
	}
}

/* gpodder's timestamps: "2026-10-11T12:34:56" in UTC (or with a zone). */
static gint64 parse_timestamp(const char *s) {
	if (s == NULL) {
		return 0;
	}
	GTimeZone *utc = g_time_zone_new_utc();
	GDateTime *d = g_date_time_new_from_iso8601(s, utc);
	g_time_zone_unref(utc);
	if (d == NULL) {
		return g_ascii_strtoll(s, NULL, 10);
	}
	gint64 t = g_date_time_to_unix(d);
	g_date_time_unref(d);
	return t;
}

static char *format_timestamp(gint64 t) {
	GDateTime *d = g_date_time_new_from_unix_utc(t);
	char *s = g_date_time_format(d, "%Y-%m-%dT%H:%M:%S");
	g_date_time_unref(d);
	return s;
}

static void got_subscriptions(struct job *j, JsonObject *o) {
	GHashTable *pending = store_subscription_changes();
	bool first = store_get_int("subs_since") == 0 && !store_get_int("synced");
	GHashTable *on_server = g_hash_table_new(g_str_hash, g_str_equal);
	JsonNode *add = json_object_get_member(o, "add");
	JsonArray *adds = add != NULL && JSON_NODE_HOLDS_ARRAY(add) ?
		json_node_get_array(add) : NULL;
	for (guint i = 0; adds != NULL && i < json_array_get_length(adds); i++) {
		const char *url = json_array_get_string_element(adds, i);
		if (url == NULL) {
			continue;
		}
		g_hash_table_add(on_server, (char *)url);
		if (g_hash_table_contains(pending, url) || store_has_podcast(url)) {
			continue; /* ours is newer, or it's here already */
		}
		store_add_podcast(url);
		g_ptr_array_add(j->new_podcasts, g_strdup(url));
		j->result.added++;
	}
	JsonNode *rem = json_object_get_member(o, "remove");
	JsonArray *removes = rem != NULL && JSON_NODE_HOLDS_ARRAY(rem) ?
		json_node_get_array(rem) : NULL;
	for (guint i = 0; removes != NULL && i < json_array_get_length(removes); i++) {
		const char *url = json_array_get_string_element(removes, i);
		if (url == NULL || g_hash_table_contains(pending, url) ||
				!store_has_podcast(url)) {
			continue;
		}
		store_remove_podcast(url);
		j->result.removed++;
	}
	if (first) {
		/* Ours the server hasn't got go up, so the two are merged. */
		GPtrArray *mine = store_podcasts();
		for (guint i = 0; i < mine->len; i++) {
			struct podcast *p = mine->pdata[i];
			if (!g_hash_table_contains(on_server, p->url)) {
				store_queue_subscription(p->url, true);
			}
		}
		g_ptr_array_unref(mine);
	}
	g_hash_table_unref(on_server);
	g_hash_table_unref(pending);
	j->subs_since = int_member(o, "timestamp", 0);
}

static void got_episodes(struct job *j, JsonObject *o) {
	JsonNode *n = json_object_get_member(o, "actions");
	JsonArray *a = n != NULL && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) :
		NULL;
	for (guint i = 0; a != NULL && i < json_array_get_length(a); i++) {
		JsonNode *e = json_array_get_element(a, i);
		if (!JSON_NODE_HOLDS_OBJECT(e)) {
			continue;
		}
		JsonObject *x = json_node_get_object(e);
		const char *episode = string_member(x, "episode");
		const char *action = string_member(x, "action");
		if (episode == NULL || action == NULL) {
			continue;
		}
		char *act = g_ascii_strdown(action, -1);
		gint64 ts = parse_timestamp(string_member(x, "timestamp"));
		struct episode *before = store_episode(episode);
		store_apply_remote(episode, string_member(x, "podcast"), act, ts,
			(int)int_member(x, "position", 0), (int)int_member(x, "total", 0));
		struct episode *after = store_episode(episode);
		if (before != NULL && after != NULL &&
				(before->position != after->position ||
				before->played != after->played)) {
			j->result.positions++;
		}
		episode_free(before);
		episode_free(after);
		g_free(act);
	}
	gint64 ts = int_member(o, "timestamp", 0);
	if (ts > 0) {
		store_set_int("episodes_since", ts);
	}
}

/* ---- The steps ---- */

static void next_step(struct job *j) {
	switch (j->step) {
	case STEP_DEVICE: {
		JsonBuilder *b = json_builder_new();
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "caption");
		json_builder_add_string_value(b, j->a.caption != NULL ? j->a.caption :
			j->a.device);
		json_builder_set_member_name(b, "type");
		json_builder_add_string_value(b, "desktop");
		json_builder_end_object(b);
		JsonNode *body = json_builder_get_root(b);
		g_object_unref(b);
		request(j, "POST", api_url(j, "devices", true, NULL), body);
		break;
	}
	case STEP_GET_SUBS: {
		char *q = g_strdup_printf("?since=%" G_GINT64_FORMAT,
			store_get_int("subs_since"));
		request(j, "GET", api_url(j, "subscriptions", true, q), NULL);
		g_free(q);
		break;
	}
	case STEP_POST_SUBS: {
		GHashTable *changes = store_subscription_changes();
		if (g_hash_table_size(changes) == 0) {
			g_hash_table_unref(changes);
			j->step++;
			next_step(j);
			return;
		}
		JsonBuilder *b = json_builder_new();
		json_builder_begin_object(b);
		for (int k = 0; k < 2; k++) {
			json_builder_set_member_name(b, k == 0 ? "add" : "remove");
			json_builder_begin_array(b);
			GHashTableIter it;
			gpointer url, on;
			g_hash_table_iter_init(&it, changes);
			while (g_hash_table_iter_next(&it, &url, &on)) {
				if ((strcmp(on, "1") == 0) == (k == 0)) {
					json_builder_add_string_value(b, url);
				}
			}
			json_builder_end_array(b);
		}
		json_builder_end_object(b);
		JsonNode *body = json_builder_get_root(b);
		g_object_unref(b);
		g_hash_table_unref(changes);
		request(j, "POST", api_url(j, "subscriptions", true, NULL), body);
		break;
	}
	case STEP_POST_EPISODES: {
		GPtrArray *actions = store_actions();
		if (actions->len == 0) {
			g_ptr_array_unref(actions);
			j->step++;
			next_step(j);
			return;
		}
		JsonBuilder *b = json_builder_new();
		json_builder_begin_array(b);
		for (guint i = 0; i < actions->len; i++) {
			struct action *x = actions->pdata[i];
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "podcast");
			json_builder_add_string_value(b, x->podcast);
			json_builder_set_member_name(b, "episode");
			json_builder_add_string_value(b, x->episode);
			if (x->guid != NULL) {
				json_builder_set_member_name(b, "guid");
				json_builder_add_string_value(b, x->guid);
			}
			json_builder_set_member_name(b, "action");
			json_builder_add_string_value(b, x->action);
			json_builder_set_member_name(b, "device");
			json_builder_add_string_value(b, j->a.device);
			char *ts = format_timestamp(x->timestamp);
			json_builder_set_member_name(b, "timestamp");
			json_builder_add_string_value(b, ts);
			g_free(ts);
			if (strcmp(x->action, "play") == 0) {
				json_builder_set_member_name(b, "started");
				json_builder_add_int_value(b, MAX(x->started, 0));
				json_builder_set_member_name(b, "position");
				json_builder_add_int_value(b, MAX(x->position, 0));
				json_builder_set_member_name(b, "total");
				json_builder_add_int_value(b, MAX(x->total, 0));
			}
			json_builder_end_object(b);
			j->last_action = x->id;
		}
		json_builder_end_array(b);
		JsonNode *body = json_builder_get_root(b);
		g_object_unref(b);
		g_ptr_array_unref(actions);
		request(j, "POST", api_url(j, "episodes", false, NULL), body);
		break;
	}
	case STEP_GET_EPISODES: {
		char *q = g_strdup_printf("?since=%" G_GINT64_FORMAT,
			store_get_int("episodes_since"));
		request(j, "GET", api_url(j, "episodes", false, q), NULL);
		g_free(q);
		break;
	}
	case STEP_DONE:
		if (!j->check_only) {
			store_set_int("synced", g_get_real_time() / G_USEC_PER_SEC);
		}
		finish(j, NULL);
		break;
	}
}

static void responded(GObject *src, GAsyncResult *res, gpointer data) {
	struct job *j = data;
	GError *error = NULL;
	GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &error);
	if (body == NULL) {
		char *why = g_strdup_printf("Can't reach the server: %s", error->message);
		g_error_free(error);
		finish(j, why);
		g_free(why);
		return;
	}
	guint status = soup_message_get_status(j->msg);
	if (status == SOUP_STATUS_UNAUTHORIZED || status == SOUP_STATUS_FORBIDDEN) {
		g_bytes_unref(body);
		finish(j, "The server didn't take the username and password.");
		return;
	}
	if (status == SOUP_STATUS_NOT_FOUND && j->step == STEP_DEVICE) {
		g_bytes_unref(body);
		finish(j, "That doesn't look like a gpodder sync server (no API there).");
		return;
	}
	if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		char *why = g_strdup_printf("The server said %u %s.", status,
			soup_message_get_reason_phrase(j->msg));
		g_bytes_unref(body);
		finish(j, why);
		g_free(why);
		return;
	}
	JsonObject *o = NULL;
	JsonParser *parser = json_parser_new();
	gsize len;
	const char *text = g_bytes_get_data(body, &len);
	if (len > 0 && json_parser_load_from_data(parser, text, (gssize)len, NULL)) {
		JsonNode *root = json_parser_get_root(parser);
		o = root != NULL && JSON_NODE_HOLDS_OBJECT(root) ?
			json_node_get_object(root) : NULL;
	}
	switch (j->step) {
	case STEP_DEVICE:
		if (j->check_only) {
			j->step = STEP_DONE;
		}
		break;
	case STEP_GET_SUBS: {
		if (o != NULL) {
			got_subscriptions(j, o);
		}
		/* Nothing of ours to send: up to date from here. Else once ours
		 * have gone. */
		GHashTable *ours = store_subscription_changes();
		if (g_hash_table_size(ours) == 0 && j->subs_since > 0) {
			store_set_int("subs_since", j->subs_since);
		}
		g_hash_table_unref(ours);
		break;
	}
	case STEP_POST_SUBS:
		store_subscriptions_sent();
		if (o != NULL) {
			update_urls(o, false);
		}
		/* From here: what we just sent comes back once, harmlessly. */
		if (j->subs_since > 0) {
			store_set_int("subs_since", j->subs_since);
		}
		break;
	case STEP_POST_EPISODES:
		store_actions_sent(j->last_action);
		if (o != NULL) {
			update_urls(o, true);
		}
		break;
	case STEP_GET_EPISODES:
		if (o != NULL) {
			got_episodes(j, o);
		}
		break;
	case STEP_DONE:
		break;
	}
	g_object_unref(parser);
	g_bytes_unref(body);
	if (j->step < STEP_DONE) {
		j->step++;
	}
	next_step(j);
}

static void start(SoupSession *session, const struct sync_account *a,
		bool check_only, sync_fn done, void *data) {
	struct job *j = g_new0(struct job, 1);
	j->session = g_object_ref(session);
	j->a.server = g_strdup(a->server);
	j->a.username = g_strdup(a->username);
	j->a.password = g_strdup(a->password);
	j->a.device = g_strdup(a->device);
	j->a.caption = g_strdup(a->caption);
	j->check_only = check_only;
	j->done = done;
	j->data = data;
	j->new_podcasts = g_ptr_array_new_with_free_func(g_free);
	next_step(j);
}

void sync_check(SoupSession *session, const struct sync_account *a,
		sync_fn done, void *data) {
	start(session, a, true, done, data);
}

void sync_run(SoupSession *session, const struct sync_account *a,
		sync_fn done, void *data) {
	start(session, a, false, done, data);
}
