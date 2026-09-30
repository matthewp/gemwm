/*
 * The mail thread: see imap.h.
 */
#include <gmime/gmime.h>
#include <string.h>
#include "cache.h"
#include "imap.h"
#include "net.h"

#define TIMEOUT 60 /* seconds */

struct mail {
	struct account *account;
	GThread *thread;
	GAsyncQueue *jobs;
	mailimap *imap;     /* NULL until connected */
	char *selected;     /* the mailbox selected, if any */
	struct cache *cache; /* the thread's own */
	guint64 queued;     /* jobs so far: the queue's order, within priority */

	/* The password, once known (a secret); asked for through the main
	 * thread when the command gives none, or it's wrong. */
	char *password;
	GMutex lock;
	GCond answered;
	bool waiting, cancelled;
	char *answer;
	mail_password_fn ask;
	void *ask_data;
};

/* What goes first: what someone's waiting on, before fetching ahead. */
enum { PRIORITY_NOW, PRIORITY_LATER };

struct job {
	int priority;
	guint64 order;
	bool again; /* more to do: back in the queue, not finished */
	void (*run)(struct mail *m, struct job *j);
	GSourceFunc finish; /* on the main thread, given the job */
	char *mailbox, *other;
	guint32 uid;
	guint limit;
	bool flag;
	GBytes *bytes;
	char *from;
	char **recipients;
	GPtrArray *list;
	GArray *uids;
	char *error;
	GCallback done;
	void *data;
};

/* ---- The password ------------------------------------------------------- */

static void secret_free(char *s) {
	if (s != NULL) {
		memset(s, 0, strlen(s));
		g_free(s);
	}
}

struct ask {
	struct mail *m;
	char *why;
};

static gboolean ask_on_main(gpointer data) {
	struct ask *a = data;
	a->m->ask(a->why, a->m->ask_data);
	g_free(a->why);
	g_free(a);
	return G_SOURCE_REMOVE;
}

/* The password, from the command, else from the main thread (why says
 * why it's asking: with no why, it looks the password up in the password
 * manager, which may ask to be unlocked); NULL if none's to be had. On
 * the mail thread. */
static char *get_password(struct mail *m, const char *why) {
	if (m->password != NULL) {
		return m->password;
	}
	char *error = NULL;
	if (why == NULL && m->account->password_command != NULL) {
		m->password = account_password(m->account, &error);
		if (m->password != NULL) {
			return m->password;
		}
	}
	struct ask *a = g_new(struct ask, 1);
	a->m = m;
	a->why = g_strdup(why != NULL ? why : error);
	g_free(error);
	g_mutex_lock(&m->lock);
	m->waiting = true;
	m->cancelled = false;
	g_idle_add(ask_on_main, a);
	while (m->waiting) {
		g_cond_wait(&m->answered, &m->lock);
	}
	m->password = m->cancelled ? NULL : g_steal_pointer(&m->answer);
	g_mutex_unlock(&m->lock);
	return m->password;
}

void mail_set_password(struct mail *m, const char *password) {
	g_mutex_lock(&m->lock);
	secret_free(m->answer);
	m->answer = g_strdup(password);
	m->waiting = false;
	g_cond_signal(&m->answered);
	g_mutex_unlock(&m->lock);
}

void mail_cancel_password(struct mail *m) {
	g_mutex_lock(&m->lock);
	m->cancelled = true;
	m->waiting = false;
	g_cond_signal(&m->answered);
	g_mutex_unlock(&m->lock);
}

/* ---- The IMAP connection ------------------------------------------------ */

static void disconnect(struct mail *m) {
	if (m->imap != NULL) {
		mailimap_free(m->imap);
		m->imap = NULL;
	}
	g_clear_pointer(&m->selected, g_free);
}

/* What went wrong: the server's words if it said any. */
static char *imap_error(struct mail *m, const char *doing, int r) {
	const char *said = m->imap != NULL && m->imap->imap_response != NULL ?
		m->imap->imap_response : NULL;
	if (said != NULL && said[0] != '\0') {
		return g_strdup_printf("%s: %s", doing, said);
	}
	return g_strdup_printf("%s (error %d)", doing, r);
}

/* Errors after which the connection is no use. */
static bool broken(int r) {
	return r == MAILIMAP_ERROR_STREAM || r == MAILIMAP_ERROR_PARSE ||
		r == MAILIMAP_ERROR_CONNECTION_REFUSED || r == MAILIMAP_ERROR_FATAL;
}

static void refresh_capabilities(struct mail *m) {
	struct mailimap_capability_data *caps = NULL;
	if (mailimap_capability(m->imap, &caps) == MAILIMAP_NO_ERROR && caps) {
		mailimap_capability_data_free(caps);
	}
}

static bool connect_imap(struct mail *m, char **error) {
	if (m->imap != NULL) {
		return true;
	}
	struct account *a = m->account;
	GError *e = NULL;
	mailstream *stream = net_connect(a->imap.host, a->imap.port, a->imap.tls,
		a->certificate, &e);
	if (stream == NULL) {
		*error = g_strdup_printf("Can't reach %s: %s", a->imap.host, e->message);
		g_error_free(e);
		return false;
	}
	m->imap = mailimap_new(0, NULL);
	mailimap_set_timeout(m->imap, TIMEOUT);
	int r = mailimap_connect(m->imap, stream);
	if (r != MAILIMAP_NO_ERROR_AUTHENTICATED &&
			r != MAILIMAP_NO_ERROR_NON_AUTHENTICATED) {
		*error = imap_error(m, "The mail server didn't answer", r);
		disconnect(m);
		return false;
	}
	if (!a->imap.tls) {
		r = mailimap_starttls(m->imap);
		if (r != MAILIMAP_NO_ERROR ||
				!net_starttls(m->imap->imap_stream, a->imap.host, a->imap.port,
				a->certificate, &e)) {
			*error = g_strdup_printf("%s won't encrypt the connection (STARTTLS)%s%s",
				a->imap.host, e ? ": " : "", e ? e->message : "");
			g_clear_error(&e);
			disconnect(m);
			return false;
		}
		refresh_capabilities(m);
	}
	if (r == MAILIMAP_NO_ERROR_AUTHENTICATED && a->imap.tls) {
		return true; /* preauthenticated */
	}
	const char *why = NULL;
	for (int tries = 0; tries < 3; tries++) {
		char *password = get_password(m, why);
		if (password == NULL) {
			*error = g_strdup("No password given");
			disconnect(m);
			return false;
		}
		r = mailimap_login(m->imap, a->user, password);
		if (r == MAILIMAP_NO_ERROR) {
			refresh_capabilities(m); /* MOVE and the like, now we're in */
			return true;
		}
		if (r != MAILIMAP_ERROR_LOGIN) {
			break;
		}
		g_clear_pointer(&m->password, secret_free);
		why = "The mail server didn't take that password";
	}
	*error = imap_error(m, "Couldn't log in", r);
	disconnect(m);
	return false;
}

static int select_mailbox(struct mail *m, const char *mailbox) {
	if (g_strcmp0(m->selected, mailbox) == 0) {
		return MAILIMAP_NO_ERROR;
	}
	g_clear_pointer(&m->selected, g_free);
	int r = mailimap_select(m->imap, mailbox);
	if (r == MAILIMAP_NO_ERROR) {
		m->selected = g_strdup(mailbox);
	}
	return r;
}

/* ---- Folders ------------------------------------------------------------ */

/* IMAP's modified UTF-7 (RFC 3501), as UTF-8: "&" starts base64 of UTF-16
 * (with "," for "/"), "-" ends it, and "&-" is "&". */
static char *decode_mutf7(const char *s) {
	GString *out = g_string_new(NULL);
	while (*s != '\0') {
		if (*s != '&') {
			g_string_append_c(out, *s++);
			continue;
		}
		const char *end = strchr(s + 1, '-');
		if (end == NULL) {
			g_string_append(out, s);
			break;
		}
		if (end == s + 1) {
			g_string_append_c(out, '&');
		} else {
			char *b64 = g_strndup(s + 1, end - s - 1);
			g_strdelimit(b64, ",", '/');
			GString *padded = g_string_new(b64);
			while (padded->len % 4 != 0) {
				g_string_append_c(padded, '=');
			}
			gsize len;
			guchar *raw = g_base64_decode(padded->str, &len);
			gunichar2 *u16 = g_new(gunichar2, len / 2 + 1);
			for (gsize i = 0; i + 1 < len; i += 2) {
				u16[i / 2] = (gunichar2)(raw[i] << 8 | raw[i + 1]);
			}
			char *u8 = g_utf16_to_utf8(u16, len / 2, NULL, NULL, NULL);
			if (u8 != NULL) {
				g_string_append(out, u8);
			}
			g_free(u8);
			g_free(u16);
			g_free(raw);
			g_string_free(padded, TRUE);
			g_free(b64);
		}
		s = end + 1;
	}
	return g_string_free(out, FALSE);
}

static enum folder_role role_by_flag(const char *flag) {
	static const struct { const char *flag; enum folder_role role; } roles[] = {
		{ "Drafts", ROLE_DRAFTS }, { "Sent", ROLE_SENT },
		{ "Archive", ROLE_ARCHIVE }, { "All", ROLE_ALL },
		{ "Junk", ROLE_JUNK }, { "Trash", ROLE_TRASH },
	};
	if (flag[0] == '\\') {
		flag++;
	}
	for (guint i = 0; i < G_N_ELEMENTS(roles); i++) {
		if (g_ascii_strcasecmp(flag, roles[i].flag) == 0) {
			return roles[i].role;
		}
	}
	return ROLE_NONE;
}

/* Servers that don't flag their special folders get them known by name. */
static enum folder_role role_by_name(const char *name) {
	static const struct { const char *name; enum folder_role role; } roles[] = {
		{ "Drafts", ROLE_DRAFTS }, { "Sent", ROLE_SENT },
		{ "Sent Items", ROLE_SENT }, { "Sent Mail", ROLE_SENT },
		{ "Sent Messages", ROLE_SENT }, { "Archive", ROLE_ARCHIVE },
		{ "Archives", ROLE_ARCHIVE }, { "Junk", ROLE_JUNK },
		{ "Spam", ROLE_JUNK }, { "Trash", ROLE_TRASH },
		{ "Deleted Items", ROLE_TRASH }, { "Deleted Messages", ROLE_TRASH },
		{ "Bin", ROLE_TRASH },
	};
	for (guint i = 0; i < G_N_ELEMENTS(roles); i++) {
		if (g_ascii_strcasecmp(name, roles[i].name) == 0) {
			return roles[i].role;
		}
	}
	return ROLE_NONE;
}

void folders_free(GPtrArray *folders) {
	for (guint i = 0; folders != NULL && i < folders->len; i++) {
		struct folder *f = folders->pdata[i];
		g_free(f->mailbox);
		g_free(f->name);
		g_free(f);
	}
	if (folders != NULL) {
		g_ptr_array_free(folders, TRUE);
	}
}

static void folder_status(struct mail *m, struct folder *f) {
	struct mailimap_status_att_list *atts = mailimap_status_att_list_new_empty();
	mailimap_status_att_list_add(atts, MAILIMAP_STATUS_ATT_MESSAGES);
	mailimap_status_att_list_add(atts, MAILIMAP_STATUS_ATT_UNSEEN);
	struct mailimap_mailbox_data_status *status = NULL;
	if (mailimap_status(m->imap, f->mailbox, atts, &status) == MAILIMAP_NO_ERROR &&
			status != NULL) {
		for (clistiter *c = clist_begin(status->st_info_list); c != NULL;
				c = clist_next(c)) {
			struct mailimap_status_info *info = clist_content(c);
			if (info->st_att == MAILIMAP_STATUS_ATT_MESSAGES) {
				f->messages = info->st_value;
			} else if (info->st_att == MAILIMAP_STATUS_ATT_UNSEEN) {
				f->unseen = info->st_value;
			}
		}
		mailimap_mailbox_data_status_free(status);
	}
	mailimap_status_att_list_free(atts);
}

static int run_list_folders(struct mail *m, struct job *j) {
	clist *list = NULL;
	int r = mailimap_list(m->imap, "", "*", &list);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Couldn't list the folders", r);
		return r;
	}
	j->list = g_ptr_array_new();
	for (clistiter *c = clist_begin(list); c != NULL; c = clist_next(c)) {
		struct mailimap_mailbox_list *mb = clist_content(c);
		struct folder *f = g_new0(struct folder, 1);
		f->mailbox = g_strdup(mb->mb_name);
		char *full = decode_mutf7(mb->mb_name);
		char delim = mb->mb_delimiter;
		const char *last = delim ? strrchr(full, delim) : NULL;
		f->name = g_strdup(last != NULL ? last + 1 : full);
		for (const char *p = full; delim && *p; p++) {
			f->depth += *p == delim;
		}
		f->selectable = true;
		struct mailimap_mbx_list_flags *flags = mb->mb_flag;
		if (flags != NULL) {
			if (flags->mbf_type == MAILIMAP_MBX_LIST_FLAGS_SFLAG &&
					flags->mbf_sflag == MAILIMAP_MBX_LIST_SFLAG_NOSELECT) {
				f->selectable = false;
			}
			for (clistiter *o = clist_begin(flags->mbf_oflags); o != NULL;
					o = clist_next(o)) {
				struct mailimap_mbx_list_oflag *of = clist_content(o);
				if (of->of_type == MAILIMAP_MBX_LIST_OFLAG_FLAG_EXT &&
						of->of_flag_ext != NULL) {
					if (g_ascii_strcasecmp(of->of_flag_ext, "Noselect") == 0 ||
							g_ascii_strcasecmp(of->of_flag_ext, "NonExistent") == 0) {
						f->selectable = false;
					} else if (f->role == ROLE_NONE) {
						f->role = role_by_flag(of->of_flag_ext);
					}
				}
			}
		}
		if (g_ascii_strcasecmp(mb->mb_name, "INBOX") == 0) {
			f->role = ROLE_INBOX;
			g_free(f->name);
			f->name = g_strdup("Inbox");
		} else if (f->role == ROLE_NONE && f->depth == 0) {
			f->role = role_by_name(full);
		}
		/* The account's own say. */
		if (g_strcmp0(mb->mb_name, m->account->sent) == 0) {
			f->role = ROLE_SENT;
		} else if (g_strcmp0(mb->mb_name, m->account->trash) == 0) {
			f->role = ROLE_TRASH;
		} else if (g_strcmp0(mb->mb_name, m->account->archive) == 0) {
			f->role = ROLE_ARCHIVE;
		}
		g_free(full);
		if (f->selectable) {
			folder_status(m, f);
		}
		if (f->role == ROLE_INBOX) {
			g_ptr_array_insert(j->list, 0, f);
		} else {
			g_ptr_array_add(j->list, f);
		}
	}
	mailimap_list_result_free(list);
	cache_set_folders(m->cache, j->list);
	return MAILIMAP_NO_ERROR;
}

/* ---- Messages ----------------------------------------------------------- */

void summaries_free(GPtrArray *messages) {
	for (guint i = 0; messages != NULL && i < messages->len; i++) {
		struct summary *s = messages->pdata[i];
		g_free(s->from);
		g_free(s->subject);
		g_free(s);
	}
	if (messages != NULL) {
		g_ptr_array_free(messages, TRUE);
	}
}

/* From, Subject and Date, decoded, from a message's headers. */
static void parse_headers(struct summary *s, const char *text, size_t len) {
	GMimeStream *stream = g_mime_stream_mem_new_with_buffer(text, len);
	GMimeParser *parser = g_mime_parser_new_with_stream(stream);
	GMimeMessage *msg = g_mime_parser_construct_message(parser, NULL);
	if (msg != NULL) {
		const char *subject = g_mime_message_get_subject(msg);
		s->subject = g_strdup(subject != NULL ? subject : "");
		InternetAddressList *from = g_mime_message_get_from(msg);
		InternetAddress *who = from != NULL &&
			internet_address_list_length(from) > 0 ?
			internet_address_list_get_address(from, 0) : NULL;
		const char *name = who != NULL ? internet_address_get_name(who) : NULL;
		if (name != NULL && name[0] != '\0') {
			s->from = g_strdup(name);
		} else if (who != NULL && INTERNET_ADDRESS_IS_MAILBOX(who)) {
			s->from = g_strdup(internet_address_mailbox_get_addr(
				INTERNET_ADDRESS_MAILBOX(who)));
		}
		GDateTime *date = g_mime_message_get_date(msg);
		s->date = date != NULL ? g_date_time_to_unix(date) : 0;
		g_object_unref(msg);
	}
	if (s->from == NULL) {
		s->from = g_strdup("");
	}
	if (s->subject == NULL) {
		s->subject = g_strdup("");
	}
	g_object_unref(parser);
	g_object_unref(stream);
}

static void read_flags(struct summary *s, struct mailimap_msg_att_dynamic *dyn) {
	for (clistiter *c = dyn != NULL && dyn->att_list != NULL ?
			clist_begin(dyn->att_list) : NULL;
			c != NULL; c = clist_next(c)) {
		struct mailimap_flag_fetch *ff = clist_content(c);
		if (ff->fl_type != MAILIMAP_FLAG_FETCH_OTHER || ff->fl_flag == NULL) {
			continue;
		}
		switch (ff->fl_flag->fl_type) {
		case MAILIMAP_FLAG_SEEN:
			s->seen = true;
			break;
		case MAILIMAP_FLAG_ANSWERED:
			s->answered = true;
			break;
		case MAILIMAP_FLAG_FLAGGED:
			s->flagged = true;
			break;
		}
	}
}

/* A fetched message's summary: whatever of it was asked for. */
static struct summary *read_summary(struct mailimap_msg_att *att) {
	struct summary *s = g_new0(struct summary, 1);
	for (clistiter *i = clist_begin(att->att_list); i != NULL;
			i = clist_next(i)) {
		struct mailimap_msg_att_item *item = clist_content(i);
		if (item->att_type == MAILIMAP_MSG_ATT_ITEM_DYNAMIC) {
			read_flags(s, item->att_data.att_dyn);
			continue;
		}
		if (item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC) {
			continue;
		}
		struct mailimap_msg_att_static *st = item->att_data.att_static;
		if (st->att_type == MAILIMAP_MSG_ATT_UID) {
			s->uid = st->att_data.att_uid;
		} else if (st->att_type == MAILIMAP_MSG_ATT_RFC822_SIZE) {
			s->size = st->att_data.att_rfc822_size;
		} else if (st->att_type == MAILIMAP_MSG_ATT_BODY_SECTION &&
				st->att_data.att_body_section != NULL &&
				st->att_data.att_body_section->sec_body_part != NULL) {
			parse_headers(s, st->att_data.att_body_section->sec_body_part,
				st->att_data.att_body_section->sec_length);
		}
	}
	return s;
}

/* The UIDs as few ranges as they'll go (sorted, lowest first). */
static struct mailimap_set *uid_set(const guint32 *uids, guint n) {
	struct mailimap_set *set = mailimap_set_new_empty();
	for (guint i = 0; i < n;) {
		guint k = i;
		while (k + 1 < n && uids[k + 1] == uids[k] + 1) {
			k++;
		}
		mailimap_set_add_interval(set, uids[i], uids[k]);
		i = k + 1;
	}
	return set;
}

static int compare_uids(gconstpointer a, gconstpointer b) {
	guint32 x = *(const guint32 *)a, y = *(const guint32 *)b;
	return x < y ? -1 : x > y;
}

static bool has_uid(GArray *sorted, guint32 uid) {
	return bsearch(&uid, sorted->data, sorted->len, sizeof(guint32),
		compare_uids) != NULL;
}

/* Summaries of the messages given, into the cache: a hundred at a time,
 * so a big first sync isn't one huge answer. */
static int fetch_summaries(struct mail *m, const char *mailbox, GArray *uids) {
	for (guint at = 0; at < uids->len; at += 100) {
		guint n = MIN(100, uids->len - at);
		struct mailimap_set *set = uid_set(&g_array_index(uids, guint32, at), n);
		struct mailimap_fetch_type *type =
			mailimap_fetch_type_new_fetch_att_list_empty();
		mailimap_fetch_type_new_fetch_att_list_add(type,
			mailimap_fetch_att_new_uid());
		mailimap_fetch_type_new_fetch_att_list_add(type,
			mailimap_fetch_att_new_flags());
		mailimap_fetch_type_new_fetch_att_list_add(type,
			mailimap_fetch_att_new_rfc822_size());
		clist *fields = clist_new();
		clist_append(fields, strdup("From"));
		clist_append(fields, strdup("Subject"));
		clist_append(fields, strdup("Date"));
		mailimap_fetch_type_new_fetch_att_list_add(type,
			mailimap_fetch_att_new_body_peek_section(
				mailimap_section_new_header_fields(
				mailimap_header_list_new(fields))));
		clist *result = NULL;
		int r = mailimap_uid_fetch(m->imap, set, type, &result);
		mailimap_fetch_type_free(type);
		mailimap_set_free(set);
		if (r != MAILIMAP_NO_ERROR) {
			return r;
		}
		cache_begin(m->cache);
		for (clistiter *c = clist_begin(result); c != NULL; c = clist_next(c)) {
			struct summary *s = read_summary(clist_content(c));
			if (s->from == NULL) {
				parse_headers(s, "", 0);
			}
			if (s->uid != 0) {
				cache_put(m->cache, mailbox, s);
			}
			g_free(s->from);
			g_free(s->subject);
			g_free(s);
		}
		cache_commit(m->cache);
		mailimap_fetch_list_free(result);
	}
	return MAILIMAP_NO_ERROR;
}

/* The flags of the messages from first on (only those changed since
 * modseq, if the server keeps track), into the cache. */
static int fetch_flags(struct mail *m, const char *mailbox, guint32 first,
		guint64 modseq) {
	struct mailimap_set *set = mailimap_set_new_interval(first, 0);
	struct mailimap_fetch_type *type =
		mailimap_fetch_type_new_fetch_att_list_empty();
	mailimap_fetch_type_new_fetch_att_list_add(type, mailimap_fetch_att_new_uid());
	mailimap_fetch_type_new_fetch_att_list_add(type,
		mailimap_fetch_att_new_flags());
	clist *result = NULL;
	int r = modseq != 0 ?
		mailimap_uid_fetch_changedsince(m->imap, set, type, modseq, &result) :
		mailimap_uid_fetch(m->imap, set, type, &result);
	mailimap_fetch_type_free(type);
	mailimap_set_free(set);
	if (r != MAILIMAP_NO_ERROR) {
		return r;
	}
	cache_begin(m->cache);
	for (clistiter *c = clist_begin(result); c != NULL; c = clist_next(c)) {
		struct summary *s = read_summary(clist_content(c));
		cache_set_flags(m->cache, mailbox, s->uid, s->seen, s->answered,
			s->flagged);
		g_free(s);
	}
	cache_commit(m->cache);
	mailimap_fetch_list_free(result);
	return MAILIMAP_NO_ERROR;
}

/* Brings the cache of a folder up to the server: its newest limit
 * messages, asking only what's changed since last time. Then the list is
 * the cache's. */
static int run_list_messages(struct mail *m, struct job *j) {
	/* Selected afresh, for the latest count. */
	g_clear_pointer(&m->selected, g_free);
	guint64 modseq = 0;
	bool condstore = mailimap_has_condstore(m->imap);
	int r = condstore ?
		mailimap_select_condstore(m->imap, j->mailbox, &modseq) :
		mailimap_select(m->imap, j->mailbox);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Couldn't open the folder", r);
		return r;
	}
	m->selected = g_strdup(j->mailbox);
	struct mailimap_selection_info *sel = m->imap->imap_selection_info;
	struct folder_state now = {
		.uidvalidity = sel != NULL ? sel->sel_uidvalidity : 0,
		.uidnext = sel != NULL ? sel->sel_uidnext : 0,
		.exists = sel != NULL ? sel->sel_exists : 0,
		.modseq = modseq,
	};
	struct folder_state was;
	bool known = cache_state(m->cache, j->mailbox, &was);
	if (known && was.uidvalidity != now.uidvalidity) {
		/* The folder was remade: every UID means something else now. */
		cache_forget_folder(m->cache, j->mailbox);
		known = false;
	}
	/* Nothing's happened since: no new mail, none gone, no flag changed. */
	if (known && now.modseq != 0 && now.modseq == was.modseq &&
			now.uidnext == was.uidnext && now.exists == was.exists &&
			now.uidnext != 0) {
		goto done;
	}
	/* Which messages are the newest limit: their UIDs, from the last
	 * limit by position. */
	GArray *wanted = g_array_new(FALSE, FALSE, sizeof(guint32));
	if (now.exists > 0) {
		guint32 first = now.exists > j->limit ? now.exists - j->limit + 1 : 1;
		struct mailimap_search_key *key = mailimap_search_key_new_set(
			mailimap_set_new_interval(first, now.exists));
		clist *found = NULL;
		r = mailimap_uid_search(m->imap, NULL, key, &found);
		mailimap_search_key_free(key);
		if (r != MAILIMAP_NO_ERROR) {
			g_array_free(wanted, TRUE);
			j->error = imap_error(m, "Couldn't read the folder", r);
			return r;
		}
		for (clistiter *c = clist_begin(found); c != NULL; c = clist_next(c)) {
			g_array_append_val(wanted, *(guint32 *)clist_content(c));
		}
		mailimap_search_result_free(found);
		g_array_sort(wanted, compare_uids);
	}
	/* Gone from the server, or too old to keep: out of the cache. */
	GArray *cached = cache_uids(m->cache, j->mailbox);
	cache_begin(m->cache);
	for (guint i = 0; i < cached->len; i++) {
		guint32 uid = g_array_index(cached, guint32, i);
		if (!has_uid(wanted, uid)) {
			cache_remove(m->cache, j->mailbox, uid);
		}
	}
	cache_commit(m->cache);
	/* The ones already had: their flags, as they are now. */
	if (known && cached->len > 0 && wanted->len > 0) {
		r = fetch_flags(m, j->mailbox, g_array_index(wanted, guint32, 0),
			was.modseq != 0 && condstore ? was.modseq : 0);
	}
	/* The ones not had yet. */
	GArray *missing = g_array_new(FALSE, FALSE, sizeof(guint32));
	for (guint i = 0; i < wanted->len; i++) {
		guint32 uid = g_array_index(wanted, guint32, i);
		if (!has_uid(cached, uid)) {
			g_array_append_val(missing, uid);
		}
	}
	if (r == MAILIMAP_NO_ERROR) {
		r = fetch_summaries(m, j->mailbox, missing);
	}
	g_array_free(missing, TRUE);
	g_array_free(cached, TRUE);
	g_array_free(wanted, TRUE);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Couldn't read the folder", r);
		return r;
	}
done:
	cache_set_state(m->cache, j->mailbox, &now);
	j->list = cache_messages(m->cache, j->mailbox, j->limit);
	return MAILIMAP_NO_ERROR;
}

static int store_flag(struct mail *m, struct mailimap_set *set,
		struct mailimap_flag *flag, bool add) {
	struct mailimap_flag_list *flags = mailimap_flag_list_new_empty();
	mailimap_flag_list_add(flags, flag);
	struct mailimap_store_att_flags *store = add ?
		mailimap_store_att_flags_new_add_flags_silent(flags) :
		mailimap_store_att_flags_new_remove_flags_silent(flags);
	int r = mailimap_uid_store(m->imap, set, store);
	mailimap_store_att_flags_free(store);
	return r;
}

/* A fetched message's whole text, and its UID. */
static GBytes *read_body(struct mailimap_msg_att *att, guint32 *uid) {
	GBytes *bytes = NULL;
	for (clistiter *i = clist_begin(att->att_list); i != NULL;
			i = clist_next(i)) {
		struct mailimap_msg_att_item *item = clist_content(i);
		if (item->att_type != MAILIMAP_MSG_ATT_ITEM_STATIC) {
			continue;
		}
		struct mailimap_msg_att_static *st = item->att_data.att_static;
		if (st->att_type == MAILIMAP_MSG_ATT_UID) {
			*uid = st->att_data.att_uid;
		} else if (st->att_type == MAILIMAP_MSG_ATT_BODY_SECTION &&
				st->att_data.att_body_section != NULL &&
				st->att_data.att_body_section->sec_body_part != NULL &&
				bytes == NULL) {
			bytes = g_bytes_new(st->att_data.att_body_section->sec_body_part,
				st->att_data.att_body_section->sec_length);
		}
	}
	return bytes;
}

static int run_fetch(struct mail *m, struct job *j) {
	int r = select_mailbox(m, j->mailbox);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Couldn't open the folder", r);
		return r;
	}
	struct mailimap_set *set = mailimap_set_new_single(j->uid);
	struct mailimap_fetch_type *type = mailimap_fetch_type_new_fetch_att_list_empty();
	mailimap_fetch_type_new_fetch_att_list_add(type,
		mailimap_fetch_att_new_body_peek_section(mailimap_section_new(NULL)));
	clist *result = NULL;
	r = mailimap_uid_fetch(m->imap, set, type, &result);
	mailimap_fetch_type_free(type);
	mailimap_set_free(set);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Couldn't fetch the message", r);
		return r;
	}
	for (clistiter *c = clist_begin(result); c != NULL && j->bytes == NULL;
			c = clist_next(c)) {
		guint32 uid;
		j->bytes = read_body(clist_content(c), &uid);
	}
	mailimap_fetch_list_free(result);
	if (j->bytes == NULL) {
		j->error = g_strdup("That message is gone");
		return MAILIMAP_NO_ERROR;
	}
	cache_set_body(m->cache, j->mailbox, j->uid, j->bytes);
	if (j->flag) {
		struct mailimap_set *one = mailimap_set_new_single(j->uid);
		if (store_flag(m, one, mailimap_flag_new_seen(), true) ==
				MAILIMAP_NO_ERROR) {
			cache_set_seen(m->cache, j->mailbox, j->uid, true);
		}
		mailimap_set_free(one);
	}
	return MAILIMAP_NO_ERROR;
}

/* Fetches ahead: the next few messages not yet had, into the cache; the
 * rest, after whatever else is waiting. */
static int run_prefetch(struct mail *m, struct job *j) {
	GArray *some = g_array_new(FALSE, FALSE, sizeof(guint32));
	while (j->uids->len > 0 && some->len < 10) {
		guint32 uid = g_array_index(j->uids, guint32, 0);
		g_array_remove_index(j->uids, 0);
		if (!cache_has_body(m->cache, j->mailbox, uid)) {
			g_array_append_val(some, uid);
		}
	}
	int r = MAILIMAP_NO_ERROR;
	if (some->len > 0) {
		r = select_mailbox(m, j->mailbox);
	}
	if (some->len > 0 && r == MAILIMAP_NO_ERROR) {
		g_array_sort(some, compare_uids);
		struct mailimap_set *set = uid_set((guint32 *)some->data, some->len);
		struct mailimap_fetch_type *type =
			mailimap_fetch_type_new_fetch_att_list_empty();
		mailimap_fetch_type_new_fetch_att_list_add(type,
			mailimap_fetch_att_new_uid());
		mailimap_fetch_type_new_fetch_att_list_add(type,
			mailimap_fetch_att_new_body_peek_section(mailimap_section_new(NULL)));
		clist *result = NULL;
		r = mailimap_uid_fetch(m->imap, set, type, &result);
		mailimap_fetch_type_free(type);
		mailimap_set_free(set);
		for (clistiter *c = r == MAILIMAP_NO_ERROR ? clist_begin(result) : NULL;
				c != NULL; c = clist_next(c)) {
			guint32 uid = 0;
			GBytes *body = read_body(clist_content(c), &uid);
			if (body != NULL && uid != 0) {
				cache_set_body(m->cache, j->mailbox, uid, body);
			}
			g_clear_pointer(&body, g_bytes_unref);
		}
		if (r == MAILIMAP_NO_ERROR) {
			mailimap_fetch_list_free(result);
		}
	}
	g_array_free(some, TRUE);
	j->again = r == MAILIMAP_NO_ERROR && j->uids->len > 0;
	return r;
}

static int run_set_seen(struct mail *m, struct job *j) {
	int r = select_mailbox(m, j->mailbox);
	if (r == MAILIMAP_NO_ERROR) {
		struct mailimap_set *set = uid_set((guint32 *)j->uids->data,
			j->uids->len);
		r = store_flag(m, set, mailimap_flag_new_seen(), j->flag);
		mailimap_set_free(set);
	}
	if (r == MAILIMAP_NO_ERROR) {
		cache_begin(m->cache);
		for (guint i = 0; i < j->uids->len; i++) {
			cache_set_seen(m->cache, j->mailbox,
				g_array_index(j->uids, guint32, i), j->flag);
		}
		cache_commit(m->cache);
	}
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, j->uids->len > 1 ?
			"Couldn't mark the messages" : "Couldn't mark the message", r);
	}
	return r;
}

static int run_move(struct mail *m, struct job *j) {
	int r;
	if (j->other != NULL && j->flag) {
		/* Made if it isn't there; if it is, the server says so, and
		 * that's fine. */
		mailimap_create(m->imap, j->other);
	}
	r = select_mailbox(m, j->mailbox);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Couldn't open the folder", r);
		return r;
	}
	struct mailimap_set *set = uid_set((guint32 *)j->uids->data, j->uids->len);
	if (j->other != NULL && strcmp(j->other, j->mailbox) != 0) {
		if (mailimap_has_extension(m->imap, (char *)"MOVE")) {
			r = mailimap_uid_move(m->imap, set, j->other);
		} else {
			r = mailimap_uid_copy(m->imap, set, j->other);
			if (r == MAILIMAP_NO_ERROR) {
				r = store_flag(m, set, mailimap_flag_new_deleted(), true);
			}
			if (r == MAILIMAP_NO_ERROR) {
				r = mailimap_expunge(m->imap);
			}
		}
	} else {
		r = store_flag(m, set, mailimap_flag_new_deleted(), true);
		if (r == MAILIMAP_NO_ERROR) {
			r = mailimap_expunge(m->imap);
		}
	}
	mailimap_set_free(set);
	if (r == MAILIMAP_NO_ERROR) {
		cache_begin(m->cache);
		for (guint i = 0; i < j->uids->len; i++) {
			cache_remove(m->cache, j->mailbox, g_array_index(j->uids, guint32, i));
		}
		cache_commit(m->cache);
	}
	if (r != MAILIMAP_NO_ERROR) {
		bool many = j->uids->len > 1;
		j->error = imap_error(m, j->other != NULL ?
			(many ? "Couldn't move the messages" : "Couldn't move the message") :
			(many ? "Couldn't delete the messages" :
			"Couldn't delete the message"), r);
	}
	return r;
}

/* ---- Sending ------------------------------------------------------------ */

static char *smtp_error(mailsmtp *s, const char *doing, int r) {
	const char *said = s != NULL ? s->response : NULL;
	if (said != NULL && said[0] != '\0') {
		return g_strdup_printf("%s: %s", doing, g_strstrip(g_strdup(said)));
	}
	return g_strdup_printf("%s (%s)", doing, mailsmtp_strerror(r));
}

static void send_smtp(struct mail *m, struct job *j) {
	struct account *a = m->account;
	GError *e = NULL;
	mailstream *stream = net_connect(a->smtp.host, a->smtp.port, a->smtp.tls,
		a->certificate, &e);
	if (stream == NULL) {
		j->error = g_strdup_printf("Can't reach %s: %s", a->smtp.host,
			e->message);
		g_error_free(e);
		return;
	}
	mailsmtp *s = mailsmtp_new(0, NULL);
	mailsmtp_set_timeout(s, TIMEOUT);
	int r = mailsmtp_connect(s, stream);
	if (r != MAILSMTP_NO_ERROR) {
		j->error = smtp_error(s, "The mail server didn't answer", r);
		goto out;
	}
	if ((r = mailesmtp_ehlo(s)) != MAILSMTP_NO_ERROR) {
		j->error = smtp_error(s, "The mail server wouldn't talk", r);
		goto out;
	}
	if (!a->smtp.tls) {
		r = mailesmtp_starttls(s);
		if (r != MAILSMTP_NO_ERROR ||
				!net_starttls(s->stream, a->smtp.host, a->smtp.port,
				a->certificate, &e)) {
			j->error = g_strdup_printf("%s won't encrypt the connection "
				"(STARTTLS)%s%s", a->smtp.host, e ? ": " : "",
				e ? e->message : "");
			g_clear_error(&e);
			goto out;
		}
		if ((r = mailesmtp_ehlo(s)) != MAILSMTP_NO_ERROR) {
			j->error = smtp_error(s, "The mail server wouldn't talk", r);
			goto out;
		}
	}
	char *password = get_password(m, NULL);
	if (password == NULL) {
		j->error = g_strdup("No password given");
		goto out;
	}
	if ((r = mailsmtp_auth(s, a->user, password)) != MAILSMTP_NO_ERROR) {
		j->error = smtp_error(s, "Couldn't log in to send", r);
		goto out;
	}
	if ((r = mailsmtp_mail(s, j->from)) != MAILSMTP_NO_ERROR) {
		j->error = smtp_error(s, "The mail server won't send from you", r);
		goto out;
	}
	for (int i = 0; j->recipients[i] != NULL; i++) {
		if ((r = mailsmtp_rcpt(s, j->recipients[i])) != MAILSMTP_NO_ERROR) {
			char *doing = g_strdup_printf("It won't send to %s",
				j->recipients[i]);
			j->error = smtp_error(s, doing, r);
			g_free(doing);
			goto out;
		}
	}
	gsize len;
	const char *data = g_bytes_get_data(j->bytes, &len);
	if ((r = mailsmtp_data(s)) != MAILSMTP_NO_ERROR ||
			(r = mailsmtp_data_message(s, data, len)) != MAILSMTP_NO_ERROR) {
		j->error = smtp_error(s, "The message wasn't sent", r);
		goto out;
	}
	mailsmtp_quit(s);
out:
	mailsmtp_free(s);
}

static int run_send(struct mail *m, struct job *j) {
	if (!j->flag) {
		send_smtp(m, j);
		j->flag = true; /* sent: a retry only saves the copy */
		if (j->error != NULL) {
			g_clear_pointer(&j->mailbox, g_free); /* no copy of what wasn't */
			return MAILIMAP_NO_ERROR;
		}
	}
	if (j->mailbox == NULL) {
		return MAILIMAP_NO_ERROR;
	}
	gsize len;
	const char *data = g_bytes_get_data(j->bytes, &len);
	struct mailimap_flag_list *flags = mailimap_flag_list_new_empty();
	mailimap_flag_list_add(flags, mailimap_flag_new_seen());
	int r = mailimap_append(m->imap, j->mailbox, flags, NULL, data, len);
	mailimap_flag_list_free(flags);
	if (r != MAILIMAP_NO_ERROR) {
		j->error = imap_error(m, "Sent, but not saved in Sent", r);
	}
	return r;
}

/* ---- The thread --------------------------------------------------------- */

typedef int (*job_fn)(struct mail *m, struct job *j);

/* Runs a job on the connection, connecting first; if the connection had
 * gone stale, once more on a new one. */
static void run_with(struct mail *m, struct job *j, job_fn fn) {
	for (int attempt = 0; attempt < 2; attempt++) {
		bool fresh = m->imap == NULL;
		if (!connect_imap(m, &j->error)) {
			return;
		}
		int r = fn(m, j);
		if (!broken(r)) {
			return;
		}
		disconnect(m);
		if (fresh) {
			return;
		}
		g_clear_pointer(&j->error, g_free);
		if (j->list != NULL) {
			g_ptr_array_set_size(j->list, 0);
		}
	}
}

static gint job_order(gconstpointer a, gconstpointer b, gpointer data) {
	const struct job *x = a, *y = b;
	if (x->priority != y->priority) {
		return x->priority - y->priority;
	}
	return x->order < y->order ? -1 : x->order > y->order;
}

/* Into the queue, behind the jobs as pressing, ahead of the rest. */
static void queue(struct mail *m, struct job *j) {
	g_async_queue_lock(m->jobs);
	j->order = m->queued++;
	g_async_queue_push_sorted_unlocked(m->jobs, j, job_order, NULL);
	g_async_queue_unlock(m->jobs);
}

static gpointer thread_main(gpointer data) {
	struct mail *m = data;
	m->cache = cache_open();
	for (;;) {
		struct job *j = g_async_queue_pop(m->jobs);
		j->run(m, j);
		if (j->again) {
			j->again = false;
			queue(m, j);
		} else {
			g_idle_add(j->finish, j);
		}
	}
	return NULL;
}

struct mail *mail_new(struct account *account, mail_password_fn password,
		void *data) {
	struct mail *m = g_new0(struct mail, 1);
	m->account = account;
	m->ask = password;
	m->ask_data = data;
	g_mutex_init(&m->lock);
	g_cond_init(&m->answered);
	m->jobs = g_async_queue_new();
	m->thread = g_thread_new("mail", thread_main, m);
	return m;
}

static struct job *job_new(void (*run)(struct mail *, struct job *),
		GSourceFunc finish, GCallback done, void *data) {
	struct job *j = g_new0(struct job, 1);
	j->run = run;
	j->finish = finish;
	j->done = done;
	j->data = data;
	return j;
}

static void job_free(struct job *j) {
	g_free(j->mailbox);
	g_free(j->other);
	g_free(j->from);
	g_strfreev(j->recipients);
	if (j->uids != NULL) {
		g_array_free(j->uids, TRUE);
	}
	g_clear_pointer(&j->bytes, g_bytes_unref);
	g_free(j->error);
	g_free(j);
}

/* Each job: what runs on the thread, and what hands it back. */

static void do_list_folders(struct mail *m, struct job *j) {
	run_with(m, j, run_list_folders);
}

static gboolean finish_folders(struct job *j) {
	((mail_folders_fn)j->done)(j->error ? NULL : j->list, j->error, j->data);
	folders_free(j->list);
	job_free(j);
	return G_SOURCE_REMOVE;
}

void mail_list_folders(struct mail *m, mail_folders_fn done, void *data) {
	queue(m, job_new(do_list_folders, (GSourceFunc)finish_folders,
		G_CALLBACK(done), data));
}

static void do_list_messages(struct mail *m, struct job *j) {
	run_with(m, j, run_list_messages);
}

static gboolean finish_messages(struct job *j) {
	((mail_messages_fn)j->done)(j->error ? NULL : j->list, j->error, j->data);
	summaries_free(j->list);
	job_free(j);
	return G_SOURCE_REMOVE;
}

void mail_list_messages(struct mail *m, const char *mailbox, guint limit,
		mail_messages_fn done, void *data) {
	struct job *j = job_new(do_list_messages,
		(GSourceFunc)finish_messages, G_CALLBACK(done), data);
	j->mailbox = g_strdup(mailbox);
	j->limit = limit;
	queue(m, j);
}

static void do_fetch(struct mail *m, struct job *j) {
	run_with(m, j, run_fetch);
}

static gboolean finish_fetch(struct job *j) {
	((mail_message_fn)j->done)(j->error ? NULL : j->bytes, j->error, j->data);
	job_free(j);
	return G_SOURCE_REMOVE;
}

void mail_fetch(struct mail *m, const char *mailbox, guint32 uid,
		bool mark_seen, mail_message_fn done, void *data) {
	struct job *j = job_new(do_fetch, (GSourceFunc)finish_fetch,
		G_CALLBACK(done), data);
	j->mailbox = g_strdup(mailbox);
	j->uid = uid;
	j->flag = mark_seen;
	queue(m, j);
}

static gboolean finish_done(struct job *j) {
	if (j->done != NULL) {
		((mail_done_fn)j->done)(j->error, j->data);
	}
	job_free(j);
	return G_SOURCE_REMOVE;
}

static void do_set_seen(struct mail *m, struct job *j) {
	run_with(m, j, run_set_seen);
}

/* The messages a job's for, lowest first. */
static void set_uids(struct job *j, const guint32 *uids, guint n) {
	j->uids = g_array_new(FALSE, FALSE, sizeof(guint32));
	g_array_append_vals(j->uids, uids, n);
	g_array_sort(j->uids, compare_uids);
}

void mail_set_seen(struct mail *m, const char *mailbox, const guint32 *uids,
		guint n, bool seen, mail_done_fn done, void *data) {
	struct job *j = job_new(do_set_seen, (GSourceFunc)finish_done,
		G_CALLBACK(done), data);
	j->mailbox = g_strdup(mailbox);
	set_uids(j, uids, n);
	j->flag = seen;
	queue(m, j);
}

static void do_move(struct mail *m, struct job *j) {
	run_with(m, j, run_move);
}

void mail_move(struct mail *m, const char *mailbox, const guint32 *uids,
		guint n, const char *to, bool create, mail_done_fn done, void *data) {
	struct job *j = job_new(do_move, (GSourceFunc)finish_done,
		G_CALLBACK(done), data);
	j->mailbox = g_strdup(mailbox);
	j->other = g_strdup(to);
	set_uids(j, uids, n);
	j->flag = create;
	queue(m, j);
}

static void do_send(struct mail *m, struct job *j) {
	if (j->mailbox == NULL) {
		run_send(m, j); /* no copy: no IMAP needed */
	} else {
		run_with(m, j, run_send);
	}
}

void mail_send(struct mail *m, GBytes *message, const char *from,
		char **recipients, const char *sent, mail_done_fn done, void *data) {
	struct job *j = job_new(do_send, (GSourceFunc)finish_done,
		G_CALLBACK(done), data);
	j->bytes = g_bytes_ref(message);
	j->from = g_strdup(from);
	j->recipients = g_strdupv(recipients);
	j->mailbox = g_strdup(sent);
	queue(m, j);
}

static void do_prefetch(struct mail *m, struct job *j) {
	run_with(m, j, run_prefetch);
}

static gboolean finish_prefetch(struct job *j) {
	job_free(j);
	return G_SOURCE_REMOVE;
}

void mail_prefetch(struct mail *m, const char *mailbox, const guint32 *uids,
		guint n) {
	if (n == 0) {
		return;
	}
	struct job *j = job_new(do_prefetch, (GSourceFunc)finish_prefetch, NULL,
		NULL);
	j->priority = PRIORITY_LATER;
	j->mailbox = g_strdup(mailbox);
	j->uids = g_array_new(FALSE, FALSE, sizeof(guint32));
	g_array_append_vals(j->uids, uids, n);
	queue(m, j);
}
