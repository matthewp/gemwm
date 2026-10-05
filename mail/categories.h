/*
 * GemMail's categories: what it sorts mail into with AI, through Augur
 * (lib/augur.h), when you've turned that on. A message can be in several
 * categories, or none. Each has a description, which is what the model
 * goes by. They're in ~/.config/gemmail/categories, written with defaults
 * the first time categories are turned on.
 */
#ifndef GEMWM_MAIL_CATEGORIES_H
#define GEMWM_MAIL_CATEGORIES_H

#include <glib.h>
#include <stdbool.h>
#include "cache.h"

struct category {
	char *name, *description;
};

struct categories {
	bool enabled;
	char *model, *tier;     /* what Augur uses, if not its own choice */
	GPtrArray *list;        /* struct category, in the file's order */
	char *version;          /* changes with the list or the model */
};

/* One message to categorise. */
struct to_categorise {
	guint32 uid;
	char *from;             /* the From line: name and address */
	const char *subject;
	char *text;             /* the start of its text */
};

char *categories_path(void);
/* The categories, or the defaults (not enabled) if there's no file. */
struct categories *categories_load(void);
void categories_free(struct categories *c);
/* Turns them on or off, writing the file (with the defaults, the first
 * time). NULL, or why it couldn't. */
char *categories_set_enabled(bool enabled);
bool categories_has(struct categories *c, const char *name);

/* The request for a batch (struct to_categorise), with your own past
 * choices (struct example, from cache_category_examples) as examples. */
char *categories_system_prompt(struct categories *c, GPtrArray *examples);
char *categories_user_prompt(GPtrArray *batch);
char *categories_schema(struct categories *c);
/* The answer: UID -> names, newline-separated (empty: none). NULL if it
 * can't be read. */
GHashTable *categories_parse_answer(const char *json);

/* With a classifier (Augur's Classify, lib/augur.h): a yes-no question
 * for each category, by number, its description what counts as yes; a
 * message as the questions' input; and the answers, the categories
 * whose probability is at least threshold (newline-separated). */
GVariant *categories_questions(struct categories *c);
char *categories_input(const struct to_categorise *m);
char *categories_from_answers(struct categories *c, GVariant *answers,
	double threshold);

/* A message's text, cut to what's sent. */
char *categories_excerpt(const char *text);

#endif
