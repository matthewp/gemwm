#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bills.h"

#define EXCERPT 4000            /* characters of a bill's text sent */

void to_read_free(gpointer p) {
	struct to_read *t = p;
	g_free(t->mailbox);
	g_free(t->from);
	g_free(t->subject);
	g_free(t->text);
	g_free(t);
}

void bill_free(gpointer p) {
	struct bill *b = p;
	g_free(b->payee);
	g_free(b->currency);
	g_free(b->due);
	g_free(b->period);
	g_free(b);
}

char *bills_system_prompt(void) {
	return g_strdup(
		"You read bills from email for the user's ledger of what they owe. "
		"For each message, say:\n"
		"- is_bill: true if it asks the user to pay something (a bill, an "
		"invoice, a statement with a balance due, a renewal about to be "
		"charged); false if it isn't, or only confirms a payment already "
		"made.\n"
		"- payee: who's to be paid, as a short name (\"Duke Energy\", not "
		"\"Duke Energy Customer Service\").\n"
		"- amount: the amount due, as a number (123.45), or null if none is "
		"given. The total due now, not a past balance or a minimum, unless "
		"only a minimum is given.\n"
		"- currency: its ISO 4217 code (USD, EUR, GBP), or null if you "
		"can't tell.\n"
		"- due: the date it's due, as YYYY-MM-DD, or null. Work out "
		"\"in 10 days\" from the date the message was sent.\n"
		"- period: what it's for, briefly, such as \"September 2026\" or "
		"\"Annual plan\", or null.\n"
		"- autopay: true if it says it'll be paid automatically (autopay, "
		"direct debit, charged to the card on file).\n"
		"Answer for every message, by its id.");
}

char *bills_user_prompt(GPtrArray *batch) {
	GString *s = g_string_new(NULL);
	for (guint i = 0; i < batch->len; i++) {
		struct to_read *t = batch->pdata[i];
		GDateTime *d = g_date_time_new_from_unix_utc(t->date);
		char *sent = g_date_time_format(d, "%Y-%m-%d");
		g_date_time_unref(d);
		g_string_append_printf(s, "<message id=\"%u\">\nFrom: %s\nSubject: %s\n"
			"Sent: %s\n%s\n</message>\n\n", i, t->from ? t->from : "",
			t->subject ? t->subject : "", sent, t->text ? t->text : "");
		g_free(sent);
	}
	return g_string_free(s, FALSE);
}

const char *bills_schema(void) {
	return "{\"type\":\"object\",\"additionalProperties\":false,"
		"\"required\":[\"messages\"],\"properties\":{\"messages\":{"
		"\"type\":\"array\",\"items\":{\"type\":\"object\","
		"\"additionalProperties\":false,"
		"\"required\":[\"id\",\"is_bill\",\"payee\",\"amount\",\"currency\","
		"\"due\",\"period\",\"autopay\"],"
		"\"properties\":{"
		"\"id\":{\"type\":\"integer\"},"
		"\"is_bill\":{\"type\":\"boolean\"},"
		"\"payee\":{\"type\":[\"string\",\"null\"]},"
		"\"amount\":{\"type\":[\"number\",\"null\"]},"
		"\"currency\":{\"type\":[\"string\",\"null\"]},"
		"\"due\":{\"type\":[\"string\",\"null\"]},"
		"\"period\":{\"type\":[\"string\",\"null\"]},"
		"\"autopay\":{\"type\":\"boolean\"}}}}}}";
}

static char *string_member(JsonObject *o, const char *name) {
	JsonNode *n = json_object_get_member(o, name);
	if (n == NULL || JSON_NODE_TYPE(n) != JSON_NODE_VALUE ||
			json_node_get_value_type(n) != G_TYPE_STRING) {
		return NULL;
	}
	const char *s = json_node_get_string(n);
	return s != NULL && s[0] != '\0' ? g_strstrip(g_strdup(s)) : NULL;
}

/* Only a real date, YYYY-MM-DD. */
static char *date_member(JsonObject *o, const char *name) {
	char *s = string_member(o, name);
	int y, m, d;
	if (s != NULL && (strlen(s) != 10 || sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 ||
			!g_date_valid_dmy(d, m, y))) {
		g_clear_pointer(&s, g_free);
	}
	return s;
}

GHashTable *bills_parse_answer(const char *json) {
	JsonParser *p = json_parser_new();
	if (!json_parser_load_from_data(p, json, -1, NULL) ||
			!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
		g_object_unref(p);
		return NULL;
	}
	JsonObject *root = json_node_get_object(json_parser_get_root(p));
	JsonArray *list = json_object_has_member(root, "messages") ?
		json_object_get_array_member(root, "messages") : NULL;
	if (list == NULL) {
		g_object_unref(p);
		return NULL;
	}
	GHashTable *answer = g_hash_table_new_full(NULL, NULL, NULL, bill_free);
	for (guint i = 0; i < json_array_get_length(list); i++) {
		JsonNode *n = json_array_get_element(list, i);
		if (!JSON_NODE_HOLDS_OBJECT(n)) {
			continue;
		}
		JsonObject *o = json_node_get_object(n);
		if (!json_object_has_member(o, "id")) {
			continue;
		}
		struct bill *b = g_new0(struct bill, 1);
		b->is_bill = json_object_get_boolean_member_with_default(o, "is_bill",
			FALSE);
		b->autopay = json_object_get_boolean_member_with_default(o, "autopay",
			FALSE);
		b->payee = string_member(o, "payee");
		b->period = string_member(o, "period");
		b->due = date_member(o, "due");
		char *currency = string_member(o, "currency");
		b->currency = currency != NULL ? g_ascii_strup(currency, -1) : NULL;
		g_free(currency);
		JsonNode *a = json_object_get_member(o, "amount");
		b->amount = a != NULL && JSON_NODE_TYPE(a) == JSON_NODE_VALUE &&
			json_node_get_value_type(a) != G_TYPE_STRING &&
			json_node_get_double(a) >= 0 ?
			(gint64)llround(json_node_get_double(a) * 100) : -1;
		g_hash_table_insert(answer,
			GINT_TO_POINTER((int)json_object_get_int_member(o, "id")), b);
	}
	g_object_unref(p);
	return answer;
}

char *bills_excerpt(const char *text) {
	if (text == NULL) {
		return g_strdup("");
	}
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

char *bills_format_amount(gint64 cents, const char *currency) {
	if (cents < 0) {
		return g_strdup("");
	}
	/* The whole part, with thousands' commas. */
	char whole[32];
	g_snprintf(whole, sizeof whole, "%" G_GINT64_FORMAT, cents / 100);
	GString *w = g_string_new(NULL);
	int len = strlen(whole);
	for (int i = 0; i < len; i++) {
		if (i > 0 && (len - i) % 3 == 0) {
			g_string_append_c(w, ',');
		}
		g_string_append_c(w, whole[i]);
	}
	g_string_append_printf(w, ".%02d", (int)(cents % 100));
	/* Dollars with their sign; the rest by code (GEM's font has no euro
	 * or pound sign). */
	if (g_strcmp0(currency, "USD") == 0) {
		g_string_prepend_c(w, '$');
	} else if (currency != NULL) {
		g_string_append_printf(w, " %s", currency);
	}
	return g_string_free(w, FALSE);
}
