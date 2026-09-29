/*
 * Adblock Plus filters to WebKit content-blocker rules: see adblock.h.
 *
 * WebKit applies rules in order, and "ignore-previous-rules" cancels the
 * rules before it for what it matches, so the rules come out as blocking,
 * then one that lets pages you go to yourself load, then element hiding,
 * then exceptions. One malformed address pattern
 * makes WebKit reject the whole list, so patterns are built only from what
 * WebKit's matcher is known to take: literals, ".*", "?", classes and
 * optional groups.
 */
#include <stdbool.h>
#include <string.h>
#include "adblock.h"

#define SCHEME "https?://"
/* ABP's "^": anything but a letter, digit or one of -_.% */
#define SEPARATOR "[^-_.%A-Za-z0-9]"
#define SELECTORS_PER_RULE 50 /* an invalid selector loses its rule */

/* Request types, as in ABP's options. */
enum {
	T_IMAGE = 1 << 0,
	T_STYLESHEET = 1 << 1,
	T_SCRIPT = 1 << 2,
	T_FONT = 1 << 3,
	T_MEDIA = 1 << 4,
	T_OBJECT = 1 << 5,
	T_XHR = 1 << 6,
	T_SUBDOCUMENT = 1 << 7,
	T_OTHER = 1 << 8,
	T_PING = 1 << 9,
	T_POPUP = 1 << 10,
	T_DOCUMENT = 1 << 11,  /* exceptions: the whole site */
	T_ELEMHIDE = 1 << 12,  /* exceptions: no element hiding */
	T_GENERICHIDE = 1 << 13,
	T_GENERICBLOCK = 1 << 14,
	T_DEFAULT = T_IMAGE | T_STYLESHEET | T_SCRIPT | T_FONT | T_MEDIA |
		T_OBJECT | T_XHR | T_SUBDOCUMENT | T_OTHER | T_PING,
};

struct filter {
	char *pattern;
	guint types;
	int third_party; /* -1 either, 0 first-party only, 1 third-party only */
	bool match_case;
	GPtrArray *included, *excluded; /* domain= */
	bool exception;
};

struct adblock {
	GPtrArray *blocking, *exceptions; /* struct filter */
	GPtrArray *generic_selectors;
	GHashTable *domain_selectors;   /* domain -> GPtrArray of selectors */
	/* Selectors with #@# exceptions: selector -> the sites (GPtrArray),
	 * or just "" for everywhere. */
	GHashTable *selector_exceptions;
	GHashTable *hide_exception_domains;    /* $elemhide: no hiding at all */
	GHashTable *generic_exception_domains; /* $generichide */
	GPtrArray *block_exception_domains;    /* $genericblock */
};

static void filter_free(struct filter *f) {
	g_free(f->pattern);
	g_ptr_array_unref(f->included);
	g_ptr_array_unref(f->excluded);
	g_free(f);
}

struct adblock *adblock_new(void) {
	struct adblock *a = g_new0(struct adblock, 1);
	a->blocking = g_ptr_array_new_with_free_func((GDestroyNotify)filter_free);
	a->exceptions = g_ptr_array_new_with_free_func((GDestroyNotify)filter_free);
	a->generic_selectors = g_ptr_array_new_with_free_func(g_free);
	a->domain_selectors = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, (GDestroyNotify)g_ptr_array_unref);
	a->selector_exceptions = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, (GDestroyNotify)g_ptr_array_unref);
	a->hide_exception_domains = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, NULL);
	a->generic_exception_domains = g_hash_table_new_full(g_str_hash,
		g_str_equal, g_free, NULL);
	a->block_exception_domains = g_ptr_array_new_with_free_func(g_free);
	return a;
}

static bool ascii(const char *s) {
	for (; *s != '\0'; s++) {
		if ((unsigned char)*s >= 0x80) {
			return false;
		}
	}
	return true;
}

/* a,b,~c (or a|b|~c for domain=) into included and excluded, lower case. */
static void parse_domains(const char *list, const char *seps,
		GPtrArray *included, GPtrArray *excluded) {
	char **names = g_strsplit_set(list, seps, -1);
	for (int i = 0; names[i] != NULL; i++) {
		char *n = g_strstrip(names[i]);
		if (n[0] == '~' && n[1] != '\0') {
			g_ptr_array_add(excluded, g_ascii_strdown(n + 1, -1));
		} else if (n[0] != '\0' && n[0] != '~') {
			g_ptr_array_add(included, g_ascii_strdown(n, -1));
		}
	}
	g_strfreev(names);
}

/* ---- Element hiding ------------------------------------------------------ */

/* Selectors WebKit can't use: ABP's and uBlock's extended ones, and CSS
 * injection. */
static bool plain_selector(const char *s) {
	static const char *const extended[] = { ":-abp-", ":has-text(",
		":xpath(", ":matches-css", ":matches-attr(", ":matches-path(",
		":style(", ":remove(", ":upward(", ":contains(", ":min-text-length(",
		":watch-attr(", ":others(", "{", "}" };
	for (guint i = 0; i < G_N_ELEMENTS(extended); i++) {
		if (strstr(s, extended[i]) != NULL) {
			return false;
		}
	}
	return s[0] != '\0';
}

static bool everywhere(GPtrArray *sites) {
	return sites->len > 0 && ((char *)sites->pdata[0])[0] == '\0';
}

static void add_hiding(struct adblock *a, const char *domains,
		const char *selector, bool exception) {
	if (!plain_selector(selector)) {
		return;
	}
	GPtrArray *included = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *excluded = g_ptr_array_new_with_free_func(g_free);
	parse_domains(domains, ",", included, excluded);
	if (exception) {
		GPtrArray *sites = g_hash_table_lookup(a->selector_exceptions, selector);
		if (sites == NULL) {
			sites = g_ptr_array_new_with_free_func(g_free);
			g_hash_table_insert(a->selector_exceptions, g_strdup(selector), sites);
		}
		if (included->len == 0) {
			g_ptr_array_set_size(sites, 0);
			g_ptr_array_add(sites, g_strdup(""));
		} else if (!everywhere(sites)) {
			for (guint i = 0; i < included->len; i++) {
				g_ptr_array_add(sites, g_strdup(included->pdata[i]));
			}
		}
		g_ptr_array_unref(included);
		g_ptr_array_unref(excluded);
		return;
	}
	/* Like abp2blocklist: a rule with exceptions for some domains is left
	 * out, as WebKit can't say "these, but not those". */
	if (excluded->len == 0 && included->len == 0) {
		g_ptr_array_add(a->generic_selectors, g_strdup(selector));
	} else if (excluded->len == 0) {
		for (guint i = 0; i < included->len; i++) {
			const char *d = included->pdata[i];
			GPtrArray *group = g_hash_table_lookup(a->domain_selectors, d);
			if (group == NULL) {
				group = g_ptr_array_new_with_free_func(g_free);
				g_hash_table_insert(a->domain_selectors, g_strdup(d), group);
			}
			g_ptr_array_add(group, g_strdup(selector));
		}
	}
	g_ptr_array_unref(included);
	g_ptr_array_unref(excluded);
}

/* ---- Address filters ----------------------------------------------------- */

/* Sets f's options from ABP's; false if any is one WebKit can't do. */
static bool parse_options(struct filter *f, const char *options) {
	static const struct { const char *name; guint type; } types[] = {
		{ "image", T_IMAGE }, { "stylesheet", T_STYLESHEET },
		{ "css", T_STYLESHEET }, { "script", T_SCRIPT }, { "font", T_FONT },
		{ "media", T_MEDIA }, { "object", T_OBJECT },
		{ "xmlhttprequest", T_XHR }, { "xhr", T_XHR },
		{ "subdocument", T_SUBDOCUMENT }, { "frame", T_SUBDOCUMENT },
		{ "other", T_OTHER }, { "ping", T_PING }, { "popup", T_POPUP },
		{ "document", T_DOCUMENT }, { "doc", T_DOCUMENT },
		{ "elemhide", T_ELEMHIDE }, { "ehide", T_ELEMHIDE },
		{ "generichide", T_GENERICHIDE }, { "ghide", T_GENERICHIDE },
		{ "genericblock", T_GENERICBLOCK },
	};
	guint positive = 0, negative = 0;
	bool ok = true;
	char **opts = g_strsplit(options, ",", -1);
	for (int i = 0; ok && opts[i] != NULL; i++) {
		char *o = g_strstrip(opts[i]);
		bool not = o[0] == '~';
		const char *name = not ? o + 1 : o;
		guint type = 0;
		for (guint j = 0; j < G_N_ELEMENTS(types); j++) {
			if (g_ascii_strcasecmp(name, types[j].name) == 0) {
				type = types[j].type;
			}
		}
		if (type != 0) {
			if (not) {
				negative |= type;
			} else {
				positive |= type;
			}
		} else if (strcmp(name, "third-party") == 0 || strcmp(name, "3p") == 0) {
			f->third_party = not ? 0 : 1;
		} else if (strcmp(name, "first-party") == 0 || strcmp(name, "1p") == 0) {
			f->third_party = not ? 1 : 0;
		} else if (strcmp(name, "match-case") == 0 && !not) {
			f->match_case = true;
		} else if (g_str_has_prefix(o, "domain=")) {
			parse_domains(o + 7, "|", f->included, f->excluded);
		} else {
			ok = false; /* $csp, $redirect, $websocket, $sitekey... */
		}
	}
	g_strfreev(opts);
	f->types = positive != 0 ? positive : T_DEFAULT & ~negative;
	return ok;
}

static void add_address(struct adblock *a, const char *line) {
	bool exception = g_str_has_prefix(line, "@@");
	const char *text = exception ? line + 2 : line;
	struct filter *f = g_new0(struct filter, 1);
	f->third_party = -1;
	f->exception = exception;
	f->included = g_ptr_array_new_with_free_func(g_free);
	f->excluded = g_ptr_array_new_with_free_func(g_free);
	const char *dollar = strrchr(text, '$');
	/* A "$" is options unless it's in a regular expression. */
	if (dollar != NULL && !(text[0] == '/' && strchr(dollar, '/') != NULL)) {
		f->pattern = g_strndup(text, dollar - text);
		if (!parse_options(f, dollar + 1)) {
			filter_free(f);
			return;
		}
	} else {
		f->pattern = g_strdup(text);
		f->types = T_DEFAULT;
	}
	size_t n = strlen(f->pattern);
	/* Regular expressions: WebKit's are too different to translate. */
	if (n >= 2 && f->pattern[0] == '/' && f->pattern[n - 1] == '/') {
		filter_free(f);
		return;
	}
	g_ptr_array_add(exception ? a->exceptions : a->blocking, f);
}

void adblock_add_list(struct adblock *a, const char *text) {
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i] != NULL; i++) {
		char *l = g_strstrip(lines[i]);
		if (l[0] == '\0' || l[0] == '!' || l[0] == '[' || !ascii(l)) {
			continue;
		}
		char *mark;
		if ((mark = strstr(l, "#@#")) != NULL) {
			*mark = '\0';
			add_hiding(a, l, mark + 3, true);
		} else if (strstr(l, "#?#") != NULL || strstr(l, "#$#") != NULL ||
				strstr(l, "#@?#") != NULL || strstr(l, "#@$#") != NULL ||
				strstr(l, "#%#") != NULL) {
			continue; /* extended hiding, snippets, scripts */
		} else if ((mark = strstr(l, "##")) != NULL) {
			*mark = '\0';
			add_hiding(a, l, mark + 2, false);
		} else {
			add_address(a, l);
		}
	}
	g_strfreev(lines);
}

/* ---- Writing WebKit's rules --------------------------------------------- */

static void json_string(GString *out, const char *s) {
	g_string_append_c(out, '"');
	for (; *s != '\0'; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\') {
			g_string_append_c(out, '\\');
			g_string_append_c(out, c);
		} else if (c < 0x20) {
			g_string_append_printf(out, "\\u%04x", c);
		} else {
			g_string_append_c(out, c);
		}
	}
	g_string_append_c(out, '"');
}

static void regex_escape(GString *out, const char *s) {
	for (; *s != '\0'; s++) {
		if (strchr(".*+?^${}()|[]\\", *s) != NULL) {
			g_string_append_c(out, '\\');
		}
		g_string_append_c(out, *s);
	}
}

/* The domains as WebKit's list: "*d" matches d and its subdomains. */
static void json_domains(GString *out, const char *key, GPtrArray *domains) {
	g_string_append_printf(out, ",\"%s\":[", key);
	for (guint i = 0; i < domains->len; i++) {
		char *d = g_strconcat("*", (char *)domains->pdata[i], NULL);
		g_string_append(out, i ? "," : "");
		json_string(out, d);
		g_free(d);
	}
	g_string_append_c(out, ']');
}

struct pattern {
	GString *regex;
	char *hostname;     /* after ||, if any */
	bool just_hostname; /* nothing after it */
	bool lowercase_ok;  /* safe to match in lower case */
};

/* An ABP address pattern as WebKit's regular expression, as
 * abp2blocklist's parseFilterRegexpSource does. */
static struct pattern parse_pattern(const char *text) {
	struct pattern p = { g_string_new(NULL), NULL, false, false };
	size_t n = strlen(text);
	ssize_t host_start = -1;
	bool host_done = false;
	for (size_t i = 0; i < n; i++) {
		char c = text[i];
		if (host_done) {
			p.just_hostname = false;
		}
		if (host_start >= 0 && !host_done) {
			bool ending = c == '*' || c == '^' || c == '?' || c == '/' ||
				c == '|';
			if (!ending && i != n - 1) {
				continue;
			}
			size_t end = ending ? i : i + 1;
			p.hostname = g_ascii_strdown(text + host_start, end - host_start);
			host_done = p.just_hostname = true;
			regex_escape(p.regex, p.hostname);
			if (!ending) {
				break;
			}
		}
		switch (c) {
		case '*':
			if (p.regex->len > 0 && i < n - 1 && text[i + 1] != '*') {
				g_string_append(p.regex, ".*");
			}
			break;
		case '^':
			if (i == 0) {
				g_string_append(p.regex, "^" SCHEME "(.*" SEPARATOR ")?");
			} else if (i == n - 1 && p.just_hostname) {
				/* After a host, an address always goes on: "/", ":" or
				 * "?". The simpler pattern compiles far smaller. */
				g_string_append(p.regex, SEPARATOR);
			} else if (i == n - 1) {
				g_string_append(p.regex, "(" SEPARATOR ".*)?$");
			} else {
				g_string_append(p.regex, SEPARATOR);
			}
			break;
		case '|':
			if (i == 0) {
				g_string_append_c(p.regex, '^');
			} else if (i == n - 1) {
				g_string_append_c(p.regex, '$');
			} else if (i == 1 && text[0] == '|') {
				host_start = i + 1;
				p.lowercase_ok = true;
				g_string_append(p.regex, SCHEME "([^/]+\\.)?");
			} else {
				g_string_append(p.regex, "\\|");
			}
			break;
		case '/':
			if (!host_done && i >= 2 && text[i - 2] == ':' && text[i - 1] == '/') {
				host_start = i + 1;
				p.lowercase_ok = true;
			}
			g_string_append_c(p.regex, '/');
			break;
		case '.': case '+': case '$': case '?': case '{': case '}':
		case '(': case ')': case '[': case ']': case '\\':
			g_string_append_c(p.regex, '\\');
			g_string_append_c(p.regex, c);
			break;
		default:
			if (host_done && g_ascii_isalpha(c)) {
				p.lowercase_ok = false;
			}
			if (c == ' ' || c == '"' || c == '<' || c == '>' || c == '`') {
				g_string_append_printf(p.regex, "%%%02X", c);
			} else {
				g_string_append_c(p.regex, c);
			}
		}
	}
	return p;
}

/* A filter's rule: its pattern, the types and parties it applies to, and
 * its domains, blocking or cancelling what's before. */
static bool add_address_rule(GString *out, struct filter *f,
		GPtrArray *exception_domains) {
	struct pattern p = parse_pattern(f->pattern);
	bool added = false;
	/* A whole site allowed: everything before is off there. */
	if (f->exception && (f->types & T_DOCUMENT) && p.just_hostname) {
		g_string_append(out, ",{\"trigger\":{\"url-filter\":\".*\","
			"\"if-domain\":[");
		char *d = g_strconcat("*", p.hostname, NULL);
		json_string(out, d);
		g_free(d);
		g_string_append(out, "]},\"action\":{\"type\":\"ignore-previous-rules\"}}");
		added = true;
	}
	guint t = f->types;
	GPtrArray *types = g_ptr_array_new();
	if (t & T_IMAGE) g_ptr_array_add(types, "image");
	if (t & T_STYLESHEET) g_ptr_array_add(types, "style-sheet");
	if (t & T_SCRIPT) g_ptr_array_add(types, "script");
	if (t & T_FONT) g_ptr_array_add(types, "font");
	if (t & (T_MEDIA | T_OBJECT)) g_ptr_array_add(types, "media");
	if (t & T_POPUP) g_ptr_array_add(types, "popup");
	if (t & (T_XHR | T_OTHER | T_PING)) g_ptr_array_add(types, "raw");
	/* WebKit can't tell frames from pages: a rule without a host would
	 * block whole pages, so it leaves frames alone. */
	if ((t & T_SUBDOCUMENT) && (f->exception || p.hostname != NULL)) {
		g_ptr_array_add(types, "document");
	}
	if (types->len == 0 || p.regex->len == 0) {
		goto out;
	}

	GString *filter = g_string_new(NULL);
	if (p.regex->str[0] != '^') {
		g_string_append(filter, "^");
		if (!g_str_has_prefix(p.regex->str, SCHEME)) {
			g_string_append(filter, SCHEME ".*");
		}
	}
	g_string_append(filter, p.regex->str);
	if (p.lowercase_ok && !f->match_case) {
		char *lower = g_ascii_strdown(filter->str, -1);
		g_string_assign(filter, lower);
		g_free(lower);
	}
	bool sensitive = p.lowercase_ok || f->match_case;

	g_string_append(out, ",{\"trigger\":{\"url-filter\":");
	json_string(out, filter->str);
	if (sensitive) {
		g_string_append(out, ",\"url-filter-is-case-sensitive\":true");
	}
	g_string_append(out, ",\"resource-type\":[");
	for (guint i = 0; i < types->len; i++) {
		g_string_append(out, i ? "," : "");
		json_string(out, types->pdata[i]);
	}
	g_string_append_c(out, ']');
	if (f->third_party >= 0) {
		g_string_append(out, f->third_party ? ",\"load-type\":[\"third-party\"]" :
			",\"load-type\":[\"first-party\"]");
	}
	GPtrArray *excluded = g_ptr_array_new();
	g_ptr_array_extend(excluded, f->excluded, NULL, NULL);
	if (exception_domains != NULL) {
		g_ptr_array_extend(excluded, exception_domains, NULL, NULL);
	}
	if (f->included->len > 0) {
		json_domains(out, "if-domain", f->included);
	} else if (excluded->len > 0) {
		json_domains(out, "unless-domain", excluded);
	}
	g_ptr_array_unref(excluded);
	g_string_append_printf(out, "},\"action\":{\"type\":\"%s\"}}",
		f->exception ? "ignore-previous-rules" : "block");
	g_string_free(filter, TRUE);
	added = true;
out:
	g_ptr_array_unref(types);
	g_string_free(p.regex, TRUE);
	g_free(p.hostname);
	return added;
}

static void css_rule(GString *out, const char *domain, GPtrArray *unless,
		GPtrArray *more_unless, const char *selectors) {
	GString *filter = g_string_new("^" SCHEME);
	if (domain != NULL) {
		g_string_append(filter, "([^/:]*\\.)?");
		regex_escape(filter, domain);
		g_string_append(filter, "[/:]");
	}
	g_string_append(out, ",{\"trigger\":{\"url-filter\":");
	json_string(out, filter->str);
	g_string_append(out, ",\"url-filter-is-case-sensitive\":true");
	GPtrArray *sites = g_ptr_array_new();
	if (unless != NULL) {
		g_ptr_array_extend(sites, unless, NULL, NULL);
	}
	if (more_unless != NULL) {
		g_ptr_array_extend(sites, more_unless, NULL, NULL);
	}
	if (sites->len > 0) {
		json_domains(out, "unless-domain", sites);
	}
	g_ptr_array_unref(sites);
	g_string_append(out, "},\"action\":{\"type\":\"css-display-none\","
		"\"selector\":");
	json_string(out, selectors);
	g_string_append(out, "}}");
	g_string_free(filter, TRUE);
}

/* Hides selectors on domain's pages (or all pages), a few to a rule. A
 * selector with exceptions for some sites gets a rule of its own, off on
 * those; one with an exception everywhere is left out. */
static guint add_css_rules(GString *out, struct adblock *a, GPtrArray *selectors,
		const char *domain, GPtrArray *unless) {
	guint rules = 0;
	GString *list = g_string_new(NULL);
	int n = 0;
	for (guint i = 0; i <= selectors->len; i++) {
		const char *s = i < selectors->len ? selectors->pdata[i] : NULL;
		GPtrArray *sites = s != NULL ?
			g_hash_table_lookup(a->selector_exceptions, s) : NULL;
		if (sites != NULL) {
			if (!everywhere(sites)) {
				css_rule(out, domain, unless, sites, s);
				rules++;
			}
			continue;
		}
		if (s != NULL) {
			g_string_append(list, list->len ? ", " : "");
			g_string_append(list, s);
			n++;
		}
		if ((s == NULL || n == SELECTORS_PER_RULE) && list->len > 0) {
			css_rule(out, domain, unless, NULL, list->str);
			rules++;
			g_string_truncate(list, 0);
			n = 0;
		}
	}
	g_string_free(list, TRUE);
	return rules;
}

/* The hostname of an exception that's just a host (@@||example.com^). */
static char *exception_host(struct filter *f) {
	struct pattern p = parse_pattern(f->pattern);
	char *host = p.just_hostname ? g_steal_pointer(&p.hostname) : NULL;
	g_string_free(p.regex, TRUE);
	g_free(p.hostname);
	return host;
}

GBytes *adblock_finish(struct adblock *a, guint *n_rules) {
	guint n = 0;
	/* Exceptions that turn off hiding, or generic hiding or blocking, for
	 * a site. */
	for (guint i = 0; i < a->exceptions->len; i++) {
		struct filter *f = a->exceptions->pdata[i];
		if (!(f->types & (T_ELEMHIDE | T_GENERICHIDE | T_GENERICBLOCK))) {
			continue;
		}
		char *host = exception_host(f);
		if (host == NULL) {
			continue;
		}
		if (f->types & T_ELEMHIDE) {
			g_hash_table_add(a->hide_exception_domains, g_strdup(host));
		}
		if (f->types & (T_ELEMHIDE | T_GENERICHIDE)) {
			g_hash_table_add(a->generic_exception_domains, g_strdup(host));
		}
		if (f->types & T_GENERICBLOCK) {
			g_ptr_array_add(a->block_exception_domains, g_strdup(host));
		}
		g_free(host);
	}

	GString *out = g_string_new("[");
	/* A rule that matches nothing, so every later one can start with ",". */
	g_string_append(out, "{\"trigger\":{\"url-filter\":\"^gemweb-none:\"},"
		"\"action\":{\"type\":\"block\"}}");

	for (guint i = 0; i < a->blocking->len; i++) {
		struct filter *f = a->blocking->pdata[i];
		if (f->types & (T_DOCUMENT | T_ELEMHIDE | T_GENERICHIDE |
				T_GENERICBLOCK)) {
			continue; /* blocking whole pages: not for an ad blocker */
		}
		n += add_address_rule(out, f, f->included->len == 0 ?
			a->block_exception_domains : NULL);
	}
	/* Frames from an ad host are blocked, but not a page there you go to
	 * yourself: a page is first-party to itself, frames from elsewhere
	 * aren't. (Before the element hiding, which this would cancel.) */
	g_string_append(out, ",{\"trigger\":{\"url-filter\":\".*\","
		"\"resource-type\":[\"document\"],\"load-type\":[\"first-party\"]},"
		"\"action\":{\"type\":\"ignore-previous-rules\"}}");

	GPtrArray *unless = g_ptr_array_new();
	GHashTableIter it;
	gpointer key, value;
	g_hash_table_iter_init(&it, a->generic_exception_domains);
	while (g_hash_table_iter_next(&it, &key, NULL)) {
		g_ptr_array_add(unless, key);
	}
	n += add_css_rules(out, a, a->generic_selectors, NULL, unless);
	g_ptr_array_unref(unless);
	g_hash_table_iter_init(&it, a->domain_selectors);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		if (!g_hash_table_contains(a->hide_exception_domains, key)) {
			n += add_css_rules(out, a, value, key, NULL);
		}
	}

	for (guint i = 0; i < a->exceptions->len; i++) {
		struct filter *f = a->exceptions->pdata[i];
		f->types &= ~(T_ELEMHIDE | T_GENERICHIDE | T_GENERICBLOCK);
		if (f->types != 0) {
			n += add_address_rule(out, f, NULL);
		}
	}
	g_string_append(out, "]");

	g_ptr_array_unref(a->blocking);
	g_ptr_array_unref(a->exceptions);
	g_ptr_array_unref(a->generic_selectors);
	g_hash_table_unref(a->domain_selectors);
	g_hash_table_unref(a->selector_exceptions);
	g_hash_table_unref(a->hide_exception_domains);
	g_hash_table_unref(a->generic_exception_domains);
	g_ptr_array_unref(a->block_exception_domains);
	g_free(a);
	*n_rules = n;
	return g_string_free_to_bytes(out);
}
