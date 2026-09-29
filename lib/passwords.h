/*
 * Passwords from a password manager's command-line tool, for GemWeb and
 * GemMail, through two commands set in their settings:
 *
 *   command HOST   prints the logins for HOST, one per line: name,
 *                  username and password, tab-separated, with tabs,
 *                  newlines and backslashes escaped as \t, \n and \\ (jq's
 *                  @tsv). Exits 3 if the password manager is locked.
 *   unlock         reads the master password, a line on stdin, and prints
 *                  a session key; later lookups get it as
 *                  $GEM_PASSWORD_SESSION. Exits non-zero, with a line on
 *                  stderr, if it can't.
 *
 * Neither runs through a shell: the command line is split into arguments
 * and the host added as one more.
 */
#ifndef GEM_PASSWORDS_H
#define GEM_PASSWORDS_H

#include <gio/gio.h>

struct login {
	char *name, *username, *password;
};

enum passwords_result {
	PASSWORDS_FOUND,  /* logins (maybe none) */
	PASSWORDS_LOCKED, /* unlock first */
	PASSWORDS_FAILED, /* error says why */
};

/* found gets the logins (an array of struct login, freed after) and, on
 * failure, a line saying why. Not called if cancelled. */
typedef void (*passwords_found_fn)(enum passwords_result result,
	GPtrArray *logins, const char *error, void *data);
void passwords_lookup(const char *command, const char *session,
	const char *host, GCancellable *cancel, passwords_found_fn found,
	void *data);

/* unlocked gets the session key, or NULL and a line saying why. The
 * password is wiped and freed. Not called if cancelled. */
typedef void (*passwords_unlocked_fn)(const char *session, const char *error,
	void *data);
void passwords_unlock(const char *command, char *password,
	GCancellable *cancel, passwords_unlocked_fn unlocked, void *data);

/* Frees logins, wiping their passwords first. */
void logins_free(GPtrArray *logins);

/* Overwrites a secret, then frees it. */
void secret_free(char *secret);

#endif
