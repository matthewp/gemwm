/*
 * Feeds: see feed.h.
 */
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <string.h>
#include "feed.h"

#define ITUNES "http://www.itunes.com/dtds/podcast-1.0.dtd"
#define CONTENT "http://purl.org/rss/1.0/modules/content/"
#define ATOM "http://www.w3.org/2005/Atom"

void feed_free(struct feed *f) {
	if (f == NULL) {
		return;
	}
	g_free(f->podcast.url);
	g_free(f->podcast.title);
	g_free(f->podcast.author);
	g_free(f->podcast.description);
	g_free(f->podcast.image);
	g_free(f->podcast.link);
	g_free(f->podcast.etag);
	g_free(f->podcast.modified);
	if (f->episodes != NULL) {
		g_ptr_array_unref(f->episodes);
	}
	g_free(f);
}

/* ---- HTML to text ---- */

static bool block(const char *name) {
	static const char *const blocks[] = { "p", "div", "br", "li", "ul", "ol",
		"h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "tr", "pre", NULL };
	for (int i = 0; blocks[i] != NULL; i++) {
		if (g_ascii_strcasecmp(name, blocks[i]) == 0) {
			return true;
		}
	}
	return false;
}

static void text_of(xmlNode *n, GString *out) {
	for (; n != NULL; n = n->next) {
		if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
			/* Runs of space as one, as a browser shows them. */
			for (const char *p = (const char *)n->content; p != NULL && *p; p++) {
				bool space = g_ascii_isspace(*p);
				if (space && (out->len == 0 || out->str[out->len - 1] == ' ' ||
						out->str[out->len - 1] == '\n')) {
					continue;
				}
				g_string_append_c(out, space ? ' ' : *p);
			}
		} else if (n->type == XML_ELEMENT_NODE) {
			const char *name = (const char *)n->name;
			if (g_ascii_strcasecmp(name, "script") == 0 ||
					g_ascii_strcasecmp(name, "style") == 0) {
				continue;
			}
			bool b = block(name);
			if (g_ascii_strcasecmp(name, "li") == 0) {
				while (out->len > 0 && out->str[out->len - 1] == ' ') {
					g_string_truncate(out, out->len - 1);
				}
				if (out->len > 0 && out->str[out->len - 1] != '\n') {
					g_string_append_c(out, '\n');
				}
				g_string_append(out, "* ");
			}
			text_of(n->children, out);
			if (b) {
				while (out->len > 0 && out->str[out->len - 1] == ' ') {
					g_string_truncate(out, out->len - 1);
				}
				g_string_append_c(out, '\n');
				if (g_ascii_strcasecmp(name, "p") == 0) {
					g_string_append_c(out, '\n');
				}
			}
		}
	}
}

char *html_to_text(const char *html) {
	if (html == NULL || html[0] == '\0') {
		return NULL;
	}
	/* Plain text already: kept as it is. */
	if (strchr(html, '<') == NULL && strchr(html, '&') == NULL) {
		return g_strstrip(g_strdup(html));
	}
	char *wrapped = g_strconcat("<html><body>", html, "</body></html>", NULL);
	htmlDocPtr doc = htmlReadMemory(wrapped, strlen(wrapped), NULL, "UTF-8",
		HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET |
		HTML_PARSE_RECOVER);
	g_free(wrapped);
	if (doc == NULL) {
		return g_strdup(html);
	}
	GString *out = g_string_new(NULL);
	text_of(xmlDocGetRootElement(doc), out);
	xmlFreeDoc(doc);
	/* No more than one blank line at a time. */
	char *s = g_string_free(out, FALSE);
	GRegex *blank = g_regex_new("\n{3,}", 0, 0, NULL);
	char *tidy = g_regex_replace_literal(blank, s, -1, 0, "\n\n", 0, NULL);
	g_regex_unref(blank);
	g_free(s);
	return g_strstrip(tidy);
}

/* ---- Reading ---- */

static bool is(xmlNode *n, const char *ns, const char *name) {
	if (n->type != XML_ELEMENT_NODE || strcmp((const char *)n->name, name) != 0) {
		return false;
	}
	const char *href = n->ns != NULL ? (const char *)n->ns->href : NULL;
	return ns == NULL ? href == NULL || strcmp(href, ATOM) == 0 ||
		strncmp(href, "http://purl.org/rss", 19) == 0 :
		href != NULL && strcmp(href, ns) == 0;
}

static char *content(xmlNode *n) {
	xmlChar *c = xmlNodeGetContent(n);
	char *s = c != NULL ? g_strstrip(g_strdup((const char *)c)) : NULL;
	xmlFree(c);
	if (s != NULL && s[0] == '\0') {
		g_clear_pointer(&s, g_free);
	}
	return s;
}

static char *attr(xmlNode *n, const char *name) {
	xmlChar *v = xmlGetProp(n, (const xmlChar *)name);
	char *s = v != NULL ? g_strstrip(g_strdup((const char *)v)) : NULL;
	xmlFree(v);
	return s;
}

/* "1:02:03", "62:03" or "3723". */
static int parse_duration(const char *s) {
	if (s == NULL) {
		return 0;
	}
	int parts[3] = { 0 }, n = 0;
	char **bits = g_strsplit(s, ":", 3);
	for (int i = 0; bits[i] != NULL && n < 3; i++) {
		parts[n++] = atoi(bits[i]);
	}
	g_strfreev(bits);
	return n == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] :
		n == 2 ? parts[0] * 60 + parts[1] : parts[0];
}

static gint64 parse_date(const char *s) {
	if (s == NULL) {
		return 0;
	}
	GDateTime *d = soup_date_time_new_from_http_string(s);
	if (d == NULL) {
		d = g_date_time_new_from_iso8601(s, NULL);
	}
	if (d == NULL) {
		return 0;
	}
	gint64 t = g_date_time_to_unix(d);
	g_date_time_unref(d);
	return t;
}

static void set(char **field, char *value) {
	if (value != NULL) {
		g_free(*field);
		*field = value;
	}
}

static struct episode *rss_item(xmlNode *item, const char *feed_url) {
	struct episode *e = g_new0(struct episode, 1);
	char *description = NULL, *encoded = NULL, *summary = NULL;
	for (xmlNode *c = item->children; c != NULL; c = c->next) {
		if (is(c, NULL, "title")) {
			set(&e->title, content(c));
		} else if (is(c, NULL, "enclosure") && e->url == NULL) {
			e->url = attr(c, "url");
			e->mime = attr(c, "type");
			char *len = attr(c, "length");
			e->size = len != NULL ? g_ascii_strtoll(len, NULL, 10) : 0;
			g_free(len);
		} else if (is(c, NULL, "guid")) {
			set(&e->guid, content(c));
		} else if (is(c, NULL, "pubDate")) {
			char *d = content(c);
			e->published = parse_date(d);
			g_free(d);
		} else if (is(c, NULL, "description")) {
			set(&description, content(c));
		} else if (is(c, CONTENT, "encoded")) {
			set(&encoded, content(c));
		} else if (is(c, ITUNES, "summary")) {
			set(&summary, content(c));
		} else if (is(c, ITUNES, "duration")) {
			char *d = content(c);
			e->duration = parse_duration(d);
			g_free(d);
		} else if (is(c, ITUNES, "image")) {
			set(&e->image, attr(c, "href"));
		}
	}
	const char *notes = encoded != NULL ? encoded : description != NULL ?
		description : summary;
	e->notes = html_to_text(notes);
	g_free(description);
	g_free(encoded);
	g_free(summary);
	if (e->url == NULL) {
		episode_free(e);
		return NULL; /* not a podcast episode: no file to play */
	}
	e->podcast = g_strdup(feed_url);
	return e;
}

static struct episode *atom_entry(xmlNode *entry, const char *feed_url) {
	struct episode *e = g_new0(struct episode, 1);
	char *summary = NULL, *body = NULL;
	for (xmlNode *c = entry->children; c != NULL; c = c->next) {
		if (is(c, ATOM, "title")) {
			set(&e->title, content(c));
		} else if (is(c, ATOM, "link")) {
			char *rel = attr(c, "rel");
			if (g_strcmp0(rel, "enclosure") == 0 && e->url == NULL) {
				e->url = attr(c, "href");
				e->mime = attr(c, "type");
				char *len = attr(c, "length");
				e->size = len != NULL ? g_ascii_strtoll(len, NULL, 10) : 0;
				g_free(len);
			}
			g_free(rel);
		} else if (is(c, ATOM, "id")) {
			set(&e->guid, content(c));
		} else if (is(c, ATOM, "published") || (is(c, ATOM, "updated") &&
				e->published == 0)) {
			char *d = content(c);
			e->published = parse_date(d);
			g_free(d);
		} else if (is(c, ATOM, "summary")) {
			set(&summary, content(c));
		} else if (is(c, ATOM, "content")) {
			set(&body, content(c));
		} else if (is(c, ITUNES, "duration")) {
			char *d = content(c);
			e->duration = parse_duration(d);
			g_free(d);
		}
	}
	e->notes = html_to_text(body != NULL ? body : summary);
	g_free(summary);
	g_free(body);
	if (e->url == NULL) {
		episode_free(e);
		return NULL;
	}
	e->podcast = g_strdup(feed_url);
	return e;
}

static int newest_first(gconstpointer a, gconstpointer b) {
	const struct episode *x = *(struct episode **)a, *y = *(struct episode **)b;
	return x->published < y->published ? 1 : x->published > y->published ? -1 : 0;
}

struct feed *feed_parse(const char *url, GBytes *body, char **error) {
	gsize len;
	const char *data = g_bytes_get_data(body, &len);
	xmlDocPtr doc = xmlReadMemory(data, (int)len, url, NULL, XML_PARSE_NOERROR |
		XML_PARSE_NOWARNING | XML_PARSE_NONET | XML_PARSE_RECOVER |
		XML_PARSE_NOCDATA);
	xmlNode *root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
	if (root == NULL) {
		*error = g_strdup("it isn't a feed (no XML)");
		if (doc != NULL) {
			xmlFreeDoc(doc);
		}
		return NULL;
	}
	struct feed *f = g_new0(struct feed, 1);
	f->podcast.url = g_strdup(url);
	f->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)episode_free);
	struct podcast *p = &f->podcast;
	if (strcmp((const char *)root->name, "rss") == 0) {
		xmlNode *channel = NULL;
		for (xmlNode *c = root->children; c != NULL && channel == NULL; c = c->next) {
			if (is(c, NULL, "channel")) {
				channel = c;
			}
		}
		char *summary = NULL, *description = NULL;
		for (xmlNode *c = channel != NULL ? channel->children : NULL; c != NULL;
				c = c->next) {
			if (is(c, NULL, "title")) {
				set(&p->title, content(c));
			} else if (is(c, NULL, "link")) {
				set(&p->link, content(c));
			} else if (is(c, NULL, "description")) {
				set(&description, content(c));
			} else if (is(c, ITUNES, "summary")) {
				set(&summary, content(c));
			} else if (is(c, ITUNES, "author")) {
				set(&p->author, content(c));
			} else if (is(c, ITUNES, "image")) {
				set(&p->image, attr(c, "href"));
			} else if (is(c, NULL, "image") && p->image == NULL) {
				for (xmlNode *i = c->children; i != NULL; i = i->next) {
					if (is(i, NULL, "url")) {
						set(&p->image, content(i));
					}
				}
			} else if (is(c, NULL, "item")) {
				struct episode *e = rss_item(c, url);
				if (e != NULL) {
					g_ptr_array_add(f->episodes, e);
				}
			}
		}
		p->description = html_to_text(description != NULL ? description : summary);
		g_free(description);
		g_free(summary);
	} else if (strcmp((const char *)root->name, "feed") == 0) {
		for (xmlNode *c = root->children; c != NULL; c = c->next) {
			if (is(c, ATOM, "title")) {
				set(&p->title, content(c));
			} else if (is(c, ATOM, "subtitle")) {
				set(&p->description, content(c));
			} else if (is(c, ATOM, "author")) {
				for (xmlNode *a = c->children; a != NULL; a = a->next) {
					if (is(a, ATOM, "name")) {
						set(&p->author, content(a));
					}
				}
			} else if (is(c, ATOM, "logo") || (is(c, ATOM, "icon") &&
					p->image == NULL)) {
				set(&p->image, content(c));
			} else if (is(c, ITUNES, "image")) {
				set(&p->image, attr(c, "href"));
			} else if (is(c, ATOM, "entry")) {
				struct episode *e = atom_entry(c, url);
				if (e != NULL) {
					g_ptr_array_add(f->episodes, e);
				}
			}
		}
	} else {
		*error = g_strdup("it isn't a podcast feed (not RSS or Atom)");
		xmlFreeDoc(doc);
		feed_free(f);
		return NULL;
	}
	xmlFreeDoc(doc);
	g_ptr_array_sort(f->episodes, newest_first);
	return f;
}

/* ---- Fetching ---- */

struct fetch {
	char *url;
	feed_fn done;
	void *data;
	SoupMessage *msg;
};

static void fetched(GObject *src, GAsyncResult *res, gpointer data) {
	struct fetch *x = data;
	GError *error = NULL;
	GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &error);
	guint status = soup_message_get_status(x->msg);
	if (body == NULL) {
		x->done(NULL, error->message, x->data);
		g_error_free(error);
	} else if (status == SOUP_STATUS_NOT_MODIFIED) {
		struct feed *f = g_new0(struct feed, 1);
		f->podcast.url = g_strdup(x->url);
		f->unchanged = true;
		x->done(f, NULL, x->data);
		feed_free(f);
	} else if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		char *why = g_strdup_printf("the server said %u %s", status,
			soup_message_get_reason_phrase(x->msg));
		x->done(NULL, why, x->data);
		g_free(why);
	} else {
		char *why = NULL;
		struct feed *f = feed_parse(x->url, body, &why);
		if (f != NULL) {
			SoupMessageHeaders *h = soup_message_get_response_headers(x->msg);
			f->podcast.etag = g_strdup(soup_message_headers_get_one(h, "ETag"));
			f->podcast.modified = g_strdup(soup_message_headers_get_one(h,
				"Last-Modified"));
		}
		x->done(f, why, x->data);
		feed_free(f);
		g_free(why);
	}
	if (body != NULL) {
		g_bytes_unref(body);
	}
	g_object_unref(x->msg);
	g_free(x->url);
	g_free(x);
}

void feed_fetch(SoupSession *session, const char *url, const char *etag,
		const char *modified, feed_fn done, void *data) {
	SoupMessage *msg = soup_message_new("GET", url);
	if (msg == NULL) {
		done(NULL, "that isn't a web address", data);
		return;
	}
	SoupMessageHeaders *h = soup_message_get_request_headers(msg);
	if (etag != NULL) {
		soup_message_headers_append(h, "If-None-Match", etag);
	}
	if (modified != NULL) {
		soup_message_headers_append(h, "If-Modified-Since", modified);
	}
	struct fetch *x = g_new0(struct fetch, 1);
	x->url = g_strdup(url);
	x->done = done;
	x->data = data;
	x->msg = msg;
	soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, NULL,
		fetched, x);
}
