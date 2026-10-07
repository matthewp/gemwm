/*
 * Messages with GMime: see message.h.
 */
#include <gio/gio.h>
#include <gmime/gmime.h>
#include <string.h>
#include "message.h"

static void attachment_free(struct attachment *a) {
	g_free(a->filename);
	g_free(a->type);
	g_bytes_unref(a->data);
	g_free(a);
}

static char *addresses(InternetAddressList *list) {
	return list != NULL && internet_address_list_length(list) > 0 ?
		internet_address_list_to_string(list, NULL, FALSE) : NULL;
}

/* The first From address alone, lower case: who the message says it's
 * from, without the name, which anyone can write. */
static char *sender_address(InternetAddressList *list) {
	for (int i = 0; list != NULL && i < internet_address_list_length(list); i++) {
		InternetAddress *a = internet_address_list_get_address(list, i);
		if (INTERNET_ADDRESS_IS_MAILBOX(a)) {
			const char *addr = internet_address_mailbox_get_addr(
				INTERNET_ADDRESS_MAILBOX(a));
			return addr != NULL && strchr(addr, '@') != NULL ?
				g_ascii_strdown(addr, -1) : NULL;
		}
	}
	return NULL;
}

/* A result's property, e.g. header.d: the value after key, up to a
 * space or the end. */
static char *property(const char *clause, const char *key) {
	const char *at = strstr(clause, key);
	if (at == NULL) {
		return NULL;
	}
	at += strlen(key);
	size_t n = strcspn(at, " \t\r\n;()");
	return n > 0 ? g_strndup(at, n) : NULL;
}

/* The sender's domain is domain, or under it (relaxed alignment:
 * news.example.org signed by example.org). */
static bool within(const char *sender_domain, const char *domain) {
	size_t a = strlen(sender_domain), b = strlen(domain);
	return a == b ? strcmp(sender_domain, domain) == 0 :
		a > b && sender_domain[a - b - 1] == '.' &&
		strcmp(sender_domain + a - b, domain) == 0;
}

/* What the server that received it made of the sender. Its header is the
 * topmost Authentication-Results; any below were there when it arrived,
 * and could have been written by anyone. */
static enum sender_check check_sender(GMimeObject *msg, const char *sender) {
	GMimeHeaderList *headers = g_mime_object_get_header_list(msg);
	const char *value = NULL;
	for (int i = 0; i < g_mime_header_list_get_count(headers); i++) {
		GMimeHeader *h = g_mime_header_list_get_header_at(headers, i);
		if (g_ascii_strcasecmp(g_mime_header_get_name(h),
				"Authentication-Results") == 0) {
			value = g_mime_header_get_value(h);
			break;
		}
	}
	if (value == NULL) {
		return SENDER_UNCHECKED;
	}
	const char *domain = sender != NULL ? strchr(sender, '@') + 1 : "";
	char *lower = g_ascii_strdown(value, -1);
	char **clauses = g_strsplit(lower, ";", -1);
	enum sender_check check = SENDER_UNVERIFIED;
	for (int i = 0; clauses[i] != NULL && check != SENDER_VERIFIED; i++) {
		const char *c = g_strstrip(clauses[i]);
		if (g_str_has_prefix(c, "dmarc=pass")) {
			check = SENDER_VERIFIED;
		} else if (g_str_has_prefix(c, "dkim=pass")) {
			char *d = property(c, "header.d=");
			if (d == NULL) {
				char *id = property(c, "header.i=");
				char *at = id != NULL ? strchr(id, '@') : NULL;
				d = at != NULL ? g_strdup(at + 1) : NULL;
				g_free(id);
			}
			if (d != NULL && domain[0] != '\0' && within(domain, d)) {
				check = SENDER_VERIFIED;
			}
			g_free(d);
		}
	}
	g_strfreev(clauses);
	g_free(lower);
	return check;
}

/* A part's content, decoded from its transfer encoding. */
static GBytes *part_bytes(GMimePart *part) {
	GMimeDataWrapper *content = g_mime_part_get_content(part);
	GMimeStream *out = g_mime_stream_mem_new();
	if (content != NULL) {
		g_mime_data_wrapper_write_to_stream(content, out);
	}
	GByteArray *array = g_mime_stream_mem_get_byte_array(
		GMIME_STREAM_MEM(out));
	GBytes *bytes = g_bytes_new(array->data, array->len);
	g_object_unref(out);
	return bytes;
}

static void each_part(GMimeObject *parent, GMimeObject *object, gpointer data) {
	struct message *m = data;
	if (GMIME_IS_MESSAGE_PART(object)) {
		/* An attached message: kept whole, as .eml. */
		GMimeMessage *inner = g_mime_message_part_get_message(
			GMIME_MESSAGE_PART(object));
		if (inner != NULL) {
			char *raw = g_mime_object_to_string(GMIME_OBJECT(inner), NULL);
			const char *subject = g_mime_message_get_subject(inner);
			struct attachment *a = g_new0(struct attachment, 1);
			a->filename = g_strdup_printf("%s.eml",
				subject != NULL && subject[0] ? subject : "message");
			g_strdelimit(a->filename, "/", '-');
			a->type = g_strdup("message/rfc822");
			a->data = g_bytes_new_take(raw, strlen(raw));
			g_ptr_array_add(m->attachments, a);
		}
		return;
	}
	if (!GMIME_IS_PART(object)) {
		return;
	}
	GMimePart *part = GMIME_PART(object);
	GMimeContentType *type = g_mime_object_get_content_type(object);
	const char *filename = g_mime_part_get_filename(part);
	bool attached = g_mime_part_is_attachment(part);
	if (!attached && GMIME_IS_TEXT_PART(object) &&
			g_mime_content_type_is_type(type, "text", "html") && m->html == NULL) {
		m->html = g_mime_text_part_get_text(GMIME_TEXT_PART(object));
		return;
	}
	if (!attached && GMIME_IS_TEXT_PART(object) &&
			g_mime_content_type_is_type(type, "text", "plain") && m->text == NULL) {
		m->text = g_mime_text_part_get_text(GMIME_TEXT_PART(object));
		return;
	}
	struct attachment *a = g_new0(struct attachment, 1);
	a->filename = g_strdup(filename != NULL ? filename : "attachment");
	g_strdelimit(a->filename, "/", '-');
	a->type = g_mime_content_type_get_mime_type(type);
	a->data = part_bytes(part);
	const char *cid = g_mime_part_get_content_id(part);
	if (cid != NULL && !attached) {
		/* Shown in the HTML (cid:), not listed. */
		g_hash_table_insert(m->inline_parts, g_strdup(cid), a);
	} else {
		g_ptr_array_add(m->attachments, a);
	}
}

struct message *message_parse(GBytes *raw) {
	gsize len;
	const char *data = g_bytes_get_data(raw, &len);
	GMimeStream *stream = g_mime_stream_mem_new_with_buffer(data, len);
	GMimeParser *parser = g_mime_parser_new_with_stream(stream);
	GMimeMessage *msg = g_mime_parser_construct_message(parser, NULL);
	g_object_unref(parser);
	g_object_unref(stream);
	if (msg == NULL) {
		return NULL;
	}
	struct message *m = g_new0(struct message, 1);
	m->attachments = g_ptr_array_new_with_free_func(
		(GDestroyNotify)attachment_free);
	m->inline_parts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)attachment_free);
	m->from = addresses(g_mime_message_get_from(msg));
	m->sender = sender_address(g_mime_message_get_from(msg));
	m->sender_check = check_sender(GMIME_OBJECT(msg), m->sender);
	m->to = addresses(g_mime_message_get_to(msg));
	m->cc = addresses(g_mime_message_get_cc(msg));
	m->reply_to = addresses(g_mime_message_get_reply_to(msg));
	const char *subject = g_mime_message_get_subject(msg);
	m->subject = g_strdup(subject != NULL ? subject : "");
	GDateTime *date = g_mime_message_get_date(msg);
	if (date != NULL) {
		GDateTime *local = g_date_time_to_local(date);
		m->date = g_date_time_format(local, "%a %e %b %Y, %H:%M");
		g_date_time_unref(local);
	}
	const char *id = g_mime_message_get_message_id(msg);
	m->message_id = id != NULL ? g_strdup_printf("<%s>", id) : NULL;
	const char *refs = g_mime_object_get_header(GMIME_OBJECT(msg), "References");
	m->references = refs != NULL ? g_strdup(refs) : NULL;
	g_mime_message_foreach(msg, each_part, m);
	g_object_unref(msg);
	return m;
}

void message_free(struct message *m) {
	if (m == NULL) {
		return;
	}
	g_free(m->from);
	g_free(m->sender);
	g_free(m->to);
	g_free(m->cc);
	g_free(m->reply_to);
	g_free(m->subject);
	g_free(m->date);
	g_free(m->message_id);
	g_free(m->references);
	g_free(m->html);
	g_free(m->text);
	g_ptr_array_unref(m->attachments);
	g_hash_table_unref(m->inline_parts);
	g_free(m);
}

/* HTML's words, roughly: tags out, block ends as new lines, the common
 * entities back to characters. For quoting in replies. */
static char *html_to_text(const char *html) {
	static const struct { const char *entity, *text; } entities[] = {
		{ "&nbsp;", " " }, { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" },
		{ "&quot;", "\"" }, { "&#39;", "'" }, { "&apos;", "'" },
	};
	GString *out = g_string_new(NULL);
	bool skip = false; /* inside <style> or <script> */
	for (const char *p = html; *p != '\0'; ) {
		if (*p == '<') {
			const char *end = strchr(p, '>');
			if (end == NULL) {
				break;
			}
			char *tag = g_ascii_strdown(p + 1, end - p - 1);
			if (g_str_has_prefix(tag, "style") || g_str_has_prefix(tag, "script")) {
				skip = true;
			} else if (g_str_has_prefix(tag, "/style") ||
					g_str_has_prefix(tag, "/script")) {
				skip = false;
			} else if (g_str_has_prefix(tag, "br") || g_str_has_prefix(tag, "/p") ||
					g_str_has_prefix(tag, "/div") || g_str_has_prefix(tag, "/tr") ||
					g_str_has_prefix(tag, "/li") || g_str_has_prefix(tag, "/h")) {
				g_string_append_c(out, '\n');
			}
			g_free(tag);
			p = end + 1;
			continue;
		}
		if (skip) {
			p++;
			continue;
		}
		if (*p == '&') {
			bool matched = false;
			for (guint i = 0; i < G_N_ELEMENTS(entities); i++) {
				size_t n = strlen(entities[i].entity);
				if (g_ascii_strncasecmp(p, entities[i].entity, n) == 0) {
					g_string_append(out, entities[i].text);
					p += n;
					matched = true;
					break;
				}
			}
			if (matched) {
				continue;
			}
		}
		g_string_append_c(out, *p++);
	}
	/* No runs of blank lines. */
	char *text = g_string_free(out, FALSE);
	GRegex *blank = g_regex_new("\n[ \t]*\n([ \t]*\n)+", 0, 0, NULL);
	char *tidy = g_regex_replace_literal(blank, text, -1, 0, "\n\n", 0, NULL);
	g_regex_unref(blank);
	g_free(text);
	return g_strstrip(tidy);
}

char *message_body_text(struct message *m) {
	if (m->text != NULL) {
		return g_strdup(m->text);
	}
	return m->html != NULL ? html_to_text(m->html) : g_strdup("");
}

struct draft *draft_new(void) {
	struct draft *d = g_new0(struct draft, 1);
	d->to = g_strdup("");
	d->cc = g_strdup("");
	d->subject = g_strdup("");
	d->body = g_strdup("");
	return d;
}

void draft_free(struct draft *d) {
	if (d == NULL) {
		return;
	}
	g_free(d->to);
	g_free(d->cc);
	g_free(d->subject);
	g_free(d->body);
	g_free(d->in_reply_to);
	g_free(d->references);
	g_free(d);
}

/* "Re: " or "Fwd: " once, not stacked. */
static char *prefixed(const char *subject, const char *prefix) {
	size_t n = strlen(prefix);
	if (g_ascii_strncasecmp(subject, prefix, n) == 0) {
		return g_strdup(subject);
	}
	return g_strconcat(prefix, subject, NULL);
}

/* The addresses in list, but not me, nor any already in seen. */
static void add_others(GString *out, const char *list, const char *me,
		GHashTable *seen) {
	InternetAddressList *parsed = list != NULL ?
		internet_address_list_parse(NULL, list) : NULL;
	for (int i = 0; parsed != NULL && i < internet_address_list_length(parsed);
			i++) {
		InternetAddress *a = internet_address_list_get_address(parsed, i);
		if (!INTERNET_ADDRESS_IS_MAILBOX(a)) {
			continue;
		}
		const char *addr = internet_address_mailbox_get_addr(
			INTERNET_ADDRESS_MAILBOX(a));
		char *key = g_ascii_strdown(addr, -1);
		if ((me != NULL && g_ascii_strcasecmp(addr, me) == 0) ||
				g_hash_table_contains(seen, key)) {
			g_free(key);
			continue;
		}
		g_hash_table_add(seen, key);
		char *one = internet_address_to_string(a, NULL, FALSE);
		g_string_append_printf(out, "%s%s", out->len ? ", " : "", one);
		g_free(one);
	}
	g_clear_object(&parsed);
}

struct draft *draft_reply(struct message *m, const char *me, bool all) {
	struct draft *d = g_new0(struct draft, 1);
	GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		NULL);
	GString *to = g_string_new(NULL), *cc = g_string_new(NULL);
	add_others(to, m->reply_to != NULL ? m->reply_to : m->from, NULL, seen);
	if (all) {
		add_others(to, m->to, me, seen);
		add_others(cc, m->cc, me, seen);
	}
	g_hash_table_unref(seen);
	d->to = g_string_free(to, FALSE);
	d->cc = g_string_free(cc, FALSE);
	d->subject = prefixed(m->subject, "Re: ");
	d->in_reply_to = g_strdup(m->message_id);
	d->references = m->references != NULL && m->message_id != NULL ?
		g_strconcat(m->references, " ", m->message_id, NULL) :
		g_strdup(m->message_id);

	char *text = message_body_text(m);
	char **lines = g_strsplit(text, "\n", -1);
	GString *body = g_string_new("\n\n");
	g_string_append_printf(body, "On %s, %s wrote:\n",
		m->date != NULL ? m->date : "an earlier date",
		m->from != NULL ? m->from : "someone");
	for (int i = 0; lines[i] != NULL; i++) {
		char *line = g_strchomp(lines[i]);
		g_string_append_printf(body, ">%s%s\n", line[0] == '>' ? "" : " ", line);
	}
	d->body = g_string_free(body, FALSE);
	g_strfreev(lines);
	g_free(text);
	return d;
}

struct draft *draft_forward(struct message *m) {
	struct draft *d = draft_new();
	g_free(d->subject);
	d->subject = prefixed(m->subject, "Fwd: ");
	char *text = message_body_text(m);
	g_free(d->body);
	d->body = g_strdup_printf("\n\n---------- Forwarded message ----------\n"
		"From: %s\nDate: %s\nSubject: %s\nTo: %s\n\n%s\n",
		m->from ? m->from : "", m->date ? m->date : "", m->subject,
		m->to ? m->to : "", text);
	g_free(text);
	return d;
}

/* The mailbox addresses in list, added to out; false if any part of it
 * isn't an address. */
static bool collect(InternetAddressList *list, GPtrArray *out) {
	for (int i = 0; i < internet_address_list_length(list); i++) {
		InternetAddress *a = internet_address_list_get_address(list, i);
		if (INTERNET_ADDRESS_IS_GROUP(a)) {
			if (!collect(internet_address_group_get_members(
					INTERNET_ADDRESS_GROUP(a)), out)) {
				return false;
			}
			continue;
		}
		const char *addr = internet_address_mailbox_get_addr(
			INTERNET_ADDRESS_MAILBOX(a));
		if (addr == NULL || strchr(addr, '@') == NULL) {
			return false;
		}
		g_ptr_array_add(out, g_strdup(addr));
	}
	return true;
}

GBytes *draft_build(struct draft *d, const char *from, char ***recipients,
		char **error) {
	InternetAddressList *to = internet_address_list_parse(NULL, d->to);
	InternetAddressList *cc = d->cc[0] != '\0' ?
		internet_address_list_parse(NULL, d->cc) : NULL;
	InternetAddressList *me = internet_address_list_parse(NULL, from);
	GPtrArray *rcpts = g_ptr_array_new_with_free_func(g_free);
	GBytes *bytes = NULL;
	if (to == NULL || internet_address_list_length(to) == 0 ||
			!collect(to, rcpts)) {
		*error = g_strdup("Who's it to? The To line needs addresses");
		goto out;
	}
	if (d->cc[0] != '\0' && (cc == NULL || !collect(cc, rcpts))) {
		*error = g_strdup("The Cc line has something that isn't an address");
		goto out;
	}

	GMimeMessage *msg = g_mime_message_new(TRUE);
	internet_address_list_append(g_mime_message_get_from(msg), me);
	internet_address_list_append(g_mime_message_get_to(msg), to);
	if (cc != NULL) {
		internet_address_list_append(g_mime_message_get_cc(msg), cc);
	}
	g_mime_message_set_subject(msg, d->subject, "utf-8");
	GDateTime *now = g_date_time_new_now_local();
	g_mime_message_set_date(msg, now);
	g_date_time_unref(now);
	const char *at = strrchr(from, '@');
	char *domain = g_strdup(at != NULL ? at + 1 : "localhost");
	char *end = strpbrk(domain, "> ");
	if (end != NULL) {
		*end = '\0';
	}
	char *id = g_mime_utils_generate_message_id(domain);
	g_mime_message_set_message_id(msg, id);
	g_free(id);
	g_free(domain);
	if (d->in_reply_to != NULL) {
		g_mime_object_set_header(GMIME_OBJECT(msg), "In-Reply-To",
			d->in_reply_to, NULL);
	}
	if (d->references != NULL) {
		g_mime_object_set_header(GMIME_OBJECT(msg), "References",
			d->references, NULL);
	}
	g_mime_object_set_header(GMIME_OBJECT(msg), "User-Agent", "GemMail",
		NULL);
	/* UTF-8, always (GMime would pick Latin-1 when the text fits it),
	 * quoted-printable when it isn't ASCII, for old servers' sake. */
	GMimeTextPart *body = g_mime_text_part_new_with_subtype("plain");
	GMimeStream *text_stream = g_mime_stream_mem_new_with_buffer(d->body,
		strlen(d->body));
	GMimeDataWrapper *wrapper = g_mime_data_wrapper_new_with_stream(
		text_stream, GMIME_CONTENT_ENCODING_DEFAULT);
	g_mime_part_set_content(GMIME_PART(body), wrapper);
	g_mime_text_part_set_charset(body, "utf-8");
	g_mime_part_set_content_encoding(GMIME_PART(body),
		g_mime_part_get_best_content_encoding(GMIME_PART(body),
		GMIME_ENCODING_CONSTRAINT_7BIT));
	g_object_unref(wrapper);
	g_object_unref(text_stream);
	if (d->files == NULL || d->files->len == 0) {
		g_mime_message_set_mime_part(msg, GMIME_OBJECT(body));
	} else {
		/* The text, then each file: multipart/mixed. */
		GMimeMultipart *mixed = g_mime_multipart_new_with_subtype("mixed");
		g_mime_multipart_add(mixed, GMIME_OBJECT(body));
		for (guint i = 0; i < d->files->len; i++) {
			const char *path = d->files->pdata[i];
			char *data = NULL;
			gsize len = 0;
			GError *e = NULL;
			if (!g_file_get_contents(path, &data, &len, &e)) {
				*error = g_strdup_printf("Couldn't attach %s: %s", path, e->message);
				g_error_free(e);
				g_object_unref(mixed);
				g_object_unref(body);
				g_object_unref(msg);
				goto out;
			}
			char *guess = g_content_type_guess(path, (const guchar *)data, len,
				NULL);
			char *type = g_content_type_get_mime_type(guess);
			char *slash = type != NULL ? strchr(type, '/') : NULL;
			GMimePart *part = slash != NULL ?
				g_mime_part_new_with_type((*slash = '\0', type), slash + 1) :
				g_mime_part_new_with_type("application", "octet-stream");
			GMimeStream *stream = g_mime_stream_mem_new_with_buffer(data, len);
			GMimeDataWrapper *content = g_mime_data_wrapper_new_with_stream(stream,
				GMIME_CONTENT_ENCODING_DEFAULT);
			g_mime_part_set_content(part, content);
			char *name = g_path_get_basename(path);
			g_mime_part_set_filename(part, name);
			g_mime_part_set_content_encoding(part, GMIME_CONTENT_ENCODING_BASE64);
			g_mime_multipart_add(mixed, GMIME_OBJECT(part));
			g_free(name);
			g_object_unref(content);
			g_object_unref(stream);
			g_object_unref(part);
			g_free(type);
			g_free(guess);
			g_free(data);
		}
		g_mime_message_set_mime_part(msg, GMIME_OBJECT(mixed));
		g_object_unref(mixed);
	}
	g_object_unref(body);

	/* As mail travels: CRLF line ends. */
	GMimeFormatOptions *options = g_mime_format_options_new();
	g_mime_format_options_set_newline_format(options, GMIME_NEWLINE_FORMAT_DOS);
	char *text = g_mime_object_to_string(GMIME_OBJECT(msg), options);
	g_mime_format_options_free(options);
	g_object_unref(msg);
	bytes = g_bytes_new_take(text, strlen(text));
	g_ptr_array_add(rcpts, NULL);
	*recipients = (char **)g_ptr_array_free(rcpts, FALSE);
	rcpts = NULL;
out:
	if (rcpts != NULL) {
		g_ptr_array_free(rcpts, TRUE);
	}
	g_clear_object(&to);
	g_clear_object(&cc);
	g_clear_object(&me);
	return bytes;
}
