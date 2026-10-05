#include <json-glib/json-glib.h>
#include <string.h>
#include "categories.h"

#define GROUP "Categories"
#define EXCERPT 1500            /* characters of a message's text sent */

static const struct category defaults[] = {
	{ "Personal", "Written to you by someone you know, not sent to a list." },
	{ "Work", "About your job: colleagues, clients, meetings, projects." },
	{ "Newsletter", "A newsletter, digest or blog post sent to many readers." },
	{ "Promotion", "Selling something: sales, offers, coupons, marketing." },
	{ "Bill", "Asks you to pay, or says a payment is due or was taken." },
	{ "Receipt", "Confirms a purchase, order, booking or donation you made." },
	{ "Shipping", "A delivery: shipped, out for delivery, delivered." },
	{ "Travel", "Flights, trains, hotels, car hire, itineraries." },
	{ "Social", "Notifications from social networks, forums and chat." },
	{ "Sports", "About sports: scores, teams, fixtures, fantasy leagues." },
};

static const char header[] =
	"# GemMail's categories: what it sorts your mail into with AI (through\n"
	"# Augur), while Options > Categorize with AI is on. A message can be in\n"
	"# several, or none.\n"
	"#\n"
	"# Each [Name] below is a category, and its description is what the model\n"
	"# goes by: say what belongs there. Add, change or take away any of them;\n"
	"# mail is categorised again when they change.\n"
	"#\n"
	"# Where Augur has a classifier (classifier = in a profile, such as\n"
	"# typesafe/jev-1.13), it answers, a message at a time, and a message goes\n"
	"# in a category it's at least 80% sure of; choose which profile's with\n"
	"# classify-profile = under [app org.gemwm.GemMail] in Augur's config.\n"
	"# Without one, a chat model answers: to choose it, set model = (a model)\n"
	"# or tier = (one of Augur's tiers) under [Categories]; else it's\n"
	"# Augur's choice for GemMail.\n";

char *categories_path(void) {
	return g_build_filename(g_get_user_config_dir(), "gemmail", "categories",
		NULL);
}

static void category_free(gpointer p) {
	struct category *c = p;
	g_free(c->name);
	g_free(c->description);
	g_free(c);
}

void categories_free(struct categories *c) {
	if (c == NULL) {
		return;
	}
	g_free(c->model);
	g_free(c->tier);
	g_ptr_array_unref(c->list);
	g_free(c->version);
	g_free(c);
}

static void add(struct categories *c, const char *name, const char *desc) {
	struct category *k = g_new0(struct category, 1);
	k->name = g_strdup(name);
	k->description = g_strdup(desc != NULL ? desc : "");
	g_ptr_array_add(c->list, k);
}

static char *value(GKeyFile *kf, const char *group, const char *key) {
	char *v = g_key_file_get_string(kf, group, key, NULL);
	if (v != NULL) {
		g_strstrip(v);
		if (v[0] == '\0') {
			g_clear_pointer(&v, g_free);
		}
	}
	return v;
}

/* What the answers depend on: the categories and the model. */
static void set_version(struct categories *c) {
	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
	g_checksum_update(sum, (const guchar *)(c->model ? c->model : ""), -1);
	g_checksum_update(sum, (const guchar *)"\n", 1);
	g_checksum_update(sum, (const guchar *)(c->tier ? c->tier : ""), -1);
	for (guint i = 0; i < c->list->len; i++) {
		struct category *k = c->list->pdata[i];
		g_checksum_update(sum, (const guchar *)"\n", 1);
		g_checksum_update(sum, (const guchar *)k->name, -1);
		g_checksum_update(sum, (const guchar *)"\t", 1);
		g_checksum_update(sum, (const guchar *)k->description, -1);
	}
	c->version = g_strndup(g_checksum_get_string(sum), 16);
	g_checksum_free(sum);
}

struct categories *categories_load(void) {
	struct categories *c = g_new0(struct categories, 1);
	c->list = g_ptr_array_new_with_free_func(category_free);
	char *path = categories_path();
	GKeyFile *kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		c->enabled = g_key_file_get_boolean(kf, GROUP, "enabled", NULL);
		c->model = value(kf, GROUP, "model");
		c->tier = value(kf, GROUP, "tier");
		char **groups = g_key_file_get_groups(kf, NULL);
		for (int i = 0; groups[i] != NULL; i++) {
			if (strcmp(groups[i], GROUP) != 0) {
				char *desc = value(kf, groups[i], "description");
				add(c, groups[i], desc);
				g_free(desc);
			}
		}
		g_strfreev(groups);
	} else {
		for (size_t i = 0; i < G_N_ELEMENTS(defaults); i++) {
			add(c, defaults[i].name, defaults[i].description);
		}
	}
	g_key_file_free(kf);
	g_free(path);
	set_version(c);
	return c;
}

/* A new file, written out as text: GKeyFile would mangle the comments. */
static bool write_new(const char *path, bool enabled, GError **error) {
	GString *s = g_string_new(header);
	g_string_append_printf(s, "\n[" GROUP "]\nenabled = %s\n",
		enabled ? "true" : "false");
	for (size_t i = 0; i < G_N_ELEMENTS(defaults); i++) {
		g_string_append_printf(s, "\n[%s]\ndescription = %s\n",
			defaults[i].name, defaults[i].description);
	}
	bool ok = g_file_set_contents(path, s->str, -1, error);
	g_string_free(s, TRUE);
	return ok;
}

char *categories_set_enabled(bool enabled) {
	char *path = categories_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	GKeyFile *kf = g_key_file_new();
	GError *error = NULL;
	bool ok;
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL)) {
		g_key_file_set_boolean(kf, GROUP, "enabled", enabled);
		ok = g_key_file_save_to_file(kf, path, &error);
	} else {
		ok = write_new(path, enabled, &error);
	}
	char *why = NULL;
	if (!ok) {
		why = g_strdup(error->message);
		g_error_free(error);
	}
	g_key_file_free(kf);
	g_free(path);
	return why;
}

bool categories_has(struct categories *c, const char *name) {
	for (guint i = 0; i < c->list->len; i++) {
		if (strcmp(((struct category *)c->list->pdata[i])->name, name) == 0) {
			return true;
		}
	}
	return false;
}

/* ---- Asking ---------------------------------------------------------------- */

char *categories_excerpt(const char *text) {
	if (text == NULL) {
		return g_strdup("");
	}
	/* Runs of blank space squeezed to one. */
	GString *s = g_string_new(NULL);
	bool space = false;
	int n = 0;
	for (const char *p = text; *p != '\0' && n < EXCERPT;
			p = g_utf8_next_char(p)) {
		gunichar ch = g_utf8_get_char(p);
		if (g_unichar_isspace(ch)) {
			space = true;
			continue;
		}
		if (space && s->len > 0) {
			g_string_append_c(s, ' ');
			n++;
		}
		space = false;
		g_string_append_unichar(s, ch);
		n++;
	}
	return g_string_free(s, FALSE);
}

char *categories_system_prompt(struct categories *c, GPtrArray *examples) {
	GString *s = g_string_new(
		"You sort email into categories. A message can be in several "
		"categories, or in none if none fits. Go by what each category's "
		"description says belongs in it.\n\nThe categories:\n");
	for (guint i = 0; i < c->list->len; i++) {
		struct category *k = c->list->pdata[i];
		g_string_append_printf(s, "- %s: %s\n", k->name, k->description);
	}
	if (examples != NULL && examples->len > 0) {
		g_string_append(s, "\nHow the user has sorted some messages "
			"themselves; follow their lead:\n");
		for (guint i = 0; i < examples->len; i++) {
			struct example *e = examples->pdata[i];
			char *names = g_strdelimit(g_strdup(e->names), "\n", ',');
			g_string_append_printf(s, "- From %s, \"%s\": %s\n",
				e->from ? e->from : "", e->subject ? e->subject : "",
				names[0] ? names : "(none)");
			g_free(names);
		}
	}
	g_string_append(s, "\nAnswer for every message, by its id.");
	return g_string_free(s, FALSE);
}

char *categories_user_prompt(GPtrArray *batch) {
	GString *s = g_string_new(NULL);
	for (guint i = 0; i < batch->len; i++) {
		struct to_categorise *m = batch->pdata[i];
		g_string_append_printf(s, "<message id=\"%u\">\nFrom: %s\nSubject: %s\n"
			"%s\n</message>\n\n", m->uid, m->from ? m->from : "",
			m->subject ? m->subject : "", m->text ? m->text : "");
	}
	return g_string_free(s, FALSE);
}

char *categories_schema(struct categories *c) {
	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "messages");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "array");
	json_builder_set_member_name(b, "items");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "object");
	json_builder_set_member_name(b, "properties");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "id");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "integer");
	json_builder_end_object(b);
	json_builder_set_member_name(b, "categories");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "type");
	json_builder_add_string_value(b, "array");
	json_builder_set_member_name(b, "uniqueItems");
	json_builder_add_boolean_value(b, TRUE);
	json_builder_set_member_name(b, "items");
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "enum");
	json_builder_begin_array(b);
	for (guint i = 0; i < c->list->len; i++) {
		json_builder_add_string_value(b,
			((struct category *)c->list->pdata[i])->name);
	}
	json_builder_end_array(b);
	json_builder_end_object(b); /* items */
	json_builder_end_object(b); /* categories */
	json_builder_end_object(b); /* properties */
	json_builder_set_member_name(b, "required");
	json_builder_begin_array(b);
	json_builder_add_string_value(b, "id");
	json_builder_add_string_value(b, "categories");
	json_builder_end_array(b);
	json_builder_set_member_name(b, "additionalProperties");
	json_builder_add_boolean_value(b, FALSE);
	json_builder_end_object(b); /* message */
	json_builder_end_object(b); /* messages */
	json_builder_end_object(b); /* properties */
	json_builder_set_member_name(b, "required");
	json_builder_begin_array(b);
	json_builder_add_string_value(b, "messages");
	json_builder_end_array(b);
	json_builder_set_member_name(b, "additionalProperties");
	json_builder_add_boolean_value(b, FALSE);
	json_builder_end_object(b);
	JsonNode *root = json_builder_get_root(b);
	char *json = json_to_string(root, FALSE);
	json_node_unref(root);
	g_object_unref(b);
	return json;
}

GHashTable *categories_parse_answer(const char *json) {
	JsonParser *parser = json_parser_new();
	GHashTable *t = NULL;
	if (json_parser_load_from_data(parser, json, -1, NULL) &&
			JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		JsonObject *o = json_node_get_object(json_parser_get_root(parser));
		JsonNode *m = json_object_get_member(o, "messages");
		if (m != NULL && JSON_NODE_HOLDS_ARRAY(m)) {
			t = g_hash_table_new_full(NULL, NULL, NULL, g_free);
			JsonArray *a = json_node_get_array(m);
			for (guint i = 0; i < json_array_get_length(a); i++) {
				JsonNode *e = json_array_get_element(a, i);
				if (!JSON_NODE_HOLDS_OBJECT(e)) {
					continue;
				}
				JsonObject *eo = json_node_get_object(e);
				gint64 id = json_object_get_int_member_with_default(eo, "id", -1);
				JsonNode *cats = json_object_get_member(eo, "categories");
				if (id < 0 || cats == NULL || !JSON_NODE_HOLDS_ARRAY(cats)) {
					continue;
				}
				GString *names = g_string_new(NULL);
				JsonArray *ca = json_node_get_array(cats);
				for (guint j = 0; j < json_array_get_length(ca); j++) {
					const char *name = json_array_get_string_element(ca, j);
					if (name != NULL) {
						g_string_append_printf(names, "%s%s", names->len ? "\n" : "",
							name);
					}
				}
				g_hash_table_insert(t, GUINT_TO_POINTER((guint32)id),
					g_string_free(names, FALSE));
			}
		}
	}
	g_object_unref(parser);
	return t;
}

/* ---- With a classifier (Augur's Classify) ------------------------------ */

GVariant *categories_questions(struct categories *c) {
	GVariantBuilder qs;
	g_variant_builder_init(&qs, G_VARIANT_TYPE("a{sv}"));
	for (guint i = 0; i < c->list->len; i++) {
		struct category *k = c->list->pdata[i];
		/* By number: a category's name needn't be a question's. */
		char *name = g_strdup_printf("c%u", i);
		char *question = g_strdup_printf(
			"Does this email belong in the category %s?", k->name);
		GVariantBuilder q;
		g_variant_builder_init(&q, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&q, "{sv}", "type", g_variant_new_string("yes-no"));
		g_variant_builder_add(&q, "{sv}", "instructions",
			g_variant_new_string(question));
		g_variant_builder_add(&q, "{sv}", "yes",
			g_variant_new_string(k->description));
		g_variant_builder_add(&qs, "{sv}", name, g_variant_builder_end(&q));
		g_free(question);
		g_free(name);
	}
	return g_variant_builder_end(&qs);
}

char *categories_input(const struct to_categorise *m) {
	return g_strdup_printf("From: %s\nSubject: %s\n\n%s", m->from ? m->from : "",
		m->subject ? m->subject : "", m->text ? m->text : "");
}

char *categories_from_answers(struct categories *c, GVariant *answers,
		double threshold) {
	GString *out = g_string_new(NULL);
	for (guint i = 0; i < c->list->len; i++) {
		char *name = g_strdup_printf("c%u", i);
		GVariant *a = g_variant_lookup_value(answers, name, G_VARIANT_TYPE_VARDICT);
		double p = 0;
		if (a != NULL) {
			g_variant_lookup(a, "probability", "d", &p);
			g_variant_unref(a);
		}
		if (p >= threshold) {
			g_string_append_printf(out, "%s%s", out->len ? "\n" : "",
				((struct category *)c->list->pdata[i])->name);
		}
		g_free(name);
	}
	return g_string_free(out, FALSE);
}
