/*
 * Running the password manager's commands: see passwords.h.
 */
#include <string.h>
#include "passwords.h"

#define LOCKED_EXIT 3

void secret_free(char *secret) {
	if (secret != NULL) {
		memset(secret, 0, strlen(secret));
		g_free(secret);
	}
}

static void login_free(struct login *l) {
	g_free(l->name);
	g_free(l->username);
	secret_free(l->password);
	g_free(l);
}

void logins_free(GPtrArray *logins) {
	if (logins != NULL) {
		g_ptr_array_set_free_func(logins, (GDestroyNotify)login_free);
		g_ptr_array_free(logins, TRUE);
	}
}

/* A field as jq's @tsv writes it: \t, \n, \r and \\ escaped. */
static char *unescape(const char *s) {
	GString *out = g_string_new(NULL);
	for (; *s != '\0'; s++) {
		if (*s == '\\' && s[1] != '\0') {
			s++;
			g_string_append_c(out, *s == 't' ? '\t' : *s == 'n' ? '\n' :
				*s == 'r' ? '\r' : *s);
		} else {
			g_string_append_c(out, *s);
		}
	}
	return g_string_free(out, FALSE);
}

static GPtrArray *parse_logins(char *text) {
	GPtrArray *logins = g_ptr_array_new();
	char **lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i] != NULL; i++) {
		char **f = g_strsplit(lines[i], "\t", 3);
		if (g_strv_length(f) == 3) {
			struct login *l = g_new(struct login, 1);
			l->name = unescape(f[0]);
			l->username = unescape(f[1]);
			l->password = unescape(f[2]);
			g_ptr_array_add(logins, l);
		}
		memset(lines[i], 0, strlen(lines[i]));
		for (int j = 0; f[j] != NULL; j++) {
			memset(f[j], 0, strlen(f[j]));
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	return logins;
}

/* The first line of what a command said on stderr, or a stand-in. */
static char *first_line(const char *err, const char *otherwise) {
	char *s = g_strstrip(g_strdup(err != NULL ? err : ""));
	char *nl = strchr(s, '\n');
	if (nl != NULL) {
		*nl = '\0';
	}
	if (s[0] == '\0') {
		g_free(s);
		return g_strdup(otherwise);
	}
	return s;
}

struct job {
	GSubprocess *proc;
	char *input; /* stdin, a secret */
	passwords_found_fn found;
	passwords_unlocked_fn unlocked;
	void *data;
};

static void job_free(struct job *j) {
	g_object_unref(j->proc);
	secret_free(j->input);
	g_free(j);
}

/* Starts argv (the command line, and one more argument if extra isn't
 * NULL), with the session key in its environment if there is one. */
static GSubprocess *start(const char *command, const char *extra,
		const char *session, GError **error) {
	int argc;
	char **argv;
	if (!g_shell_parse_argv(command, &argc, &argv, error)) {
		return NULL;
	}
	if (extra != NULL) {
		argv = g_renew(char *, argv, argc + 2);
		argv[argc] = g_strdup(extra);
		argv[argc + 1] = NULL;
	}
	GSubprocessLauncher *launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE);
	if (session != NULL) {
		g_subprocess_launcher_setenv(launcher, "GEMWEB_PASSWORD_SESSION",
			session, TRUE);
	} else {
		g_subprocess_launcher_unsetenv(launcher, "GEMWEB_PASSWORD_SESSION");
	}
	GSubprocess *proc = g_subprocess_launcher_spawnv(launcher,
		(const char *const *)argv, error);
	g_object_unref(launcher);
	g_strfreev(argv);
	return proc;
}

static void lookup_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct job *j = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	if (!g_subprocess_communicate_utf8_finish(j->proc, result, &out, &err,
			&error)) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			j->found(PASSWORDS_FAILED, NULL, error->message, j->data);
		} else {
			g_subprocess_force_exit(j->proc);
		}
		g_error_free(error);
	} else if (g_subprocess_get_if_exited(j->proc) &&
			g_subprocess_get_exit_status(j->proc) == LOCKED_EXIT) {
		j->found(PASSWORDS_LOCKED, NULL, NULL, j->data);
	} else if (!g_subprocess_get_successful(j->proc)) {
		char *why = first_line(err, "The password manager failed");
		j->found(PASSWORDS_FAILED, NULL, why, j->data);
		g_free(why);
	} else {
		GPtrArray *logins = parse_logins(out);
		j->found(PASSWORDS_FOUND, logins, NULL, j->data);
		logins_free(logins);
	}
	secret_free(out);
	g_free(err);
	job_free(j);
}

void passwords_lookup(const char *command, const char *session,
		const char *host, GCancellable *cancel, passwords_found_fn found,
		void *data) {
	GError *error = NULL;
	GSubprocess *proc = start(command, host, session, &error);
	if (proc == NULL) {
		found(PASSWORDS_FAILED, NULL, error->message, data);
		g_error_free(error);
		return;
	}
	struct job *j = g_new0(struct job, 1);
	j->proc = proc;
	j->found = found;
	j->data = data;
	g_subprocess_communicate_utf8_async(proc, NULL, cancel, lookup_done, j);
}

static void unlock_done(GObject *source, GAsyncResult *result, gpointer data) {
	struct job *j = data;
	char *out = NULL, *err = NULL;
	GError *error = NULL;
	if (!g_subprocess_communicate_utf8_finish(j->proc, result, &out, &err,
			&error)) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			j->unlocked(NULL, error->message, j->data);
		} else {
			g_subprocess_force_exit(j->proc);
		}
		g_error_free(error);
	} else if (!g_subprocess_get_successful(j->proc) || out == NULL ||
			g_strstrip(out)[0] == '\0') {
		char *why = first_line(err, "Couldn't unlock");
		j->unlocked(NULL, why, j->data);
		g_free(why);
	} else {
		j->unlocked(out, NULL, j->data);
	}
	secret_free(out);
	g_free(err);
	job_free(j);
}

void passwords_unlock(const char *command, char *password,
		GCancellable *cancel, passwords_unlocked_fn unlocked, void *data) {
	GError *error = NULL;
	GSubprocess *proc = start(command, NULL, NULL, &error);
	if (proc == NULL) {
		secret_free(password);
		unlocked(NULL, error->message, data);
		g_error_free(error);
		return;
	}
	struct job *j = g_new0(struct job, 1);
	j->proc = proc;
	j->input = g_strconcat(password, "\n", NULL);
	secret_free(password);
	j->unlocked = unlocked;
	j->data = data;
	g_subprocess_communicate_utf8_async(proc, j->input, cancel, unlock_done, j);
}
