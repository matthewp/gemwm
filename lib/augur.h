/*
 * Asking Augur (github.com/matthewp/augur), the session's AI service, for
 * GemWM's programs: whether it's on, and answers. Augur has the providers,
 * keys and models; a program only says what it wants.
 *
 * Without Augur installed, or with it turned off (GemWM's [ai] enabled =
 * false, or Augur's own config), augur_enabled is false and programs hide
 * their AI features.
 */
#ifndef GEMWM_AUGUR_H
#define GEMWM_AUGUR_H

#include <glib.h>
#include <stdbool.h>

/* Starts watching whether Augur is on; changed is called when that
 * changes (and once it's first known). Call once. */
void augur_watch(const char *app_id, void (*changed)(bool enabled, void *data),
	void *data);
bool augur_enabled(void);

/* An answer: text (JSON matching schema, if one was given), or error. */
typedef void (*augur_answer_fn)(const char *text, const char *error,
	void *data);

/* Asks, with a system message and a user message. schema may be NULL; so
 * may model and tier (one of the profile's, e.g. "cheap"): with neither,
 * the model is Augur's choice for this program. */
void augur_ask(const char *system, const char *user, const char *schema,
	const char *model, const char *tier, augur_answer_fn done, void *data);

#endif
