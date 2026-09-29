/*
 * The mail account: see account.h.
 */
#include <gmime/gmime.h>
#include <stdlib.h>
#include <string.h>
#include "account.h"

static char *config_path(void) {
	return g_build_filename(g_get_user_config_dir(), "gemmail", "settings",
		NULL);
}

static char *expand_home(const char *path) {
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
		return g_build_filename(g_get_home_dir(), path + 1, NULL);
	}
	return g_strdup(path);
}

/* imaps://host[:port], imap://host[:port]: TLS, or STARTTLS, and the
 * port, the protocol's own unless given. */
static bool parse_server(const char *url, const char *scheme, guint16 tls_port,
		guint16 starttls_port, struct server *out) {
	char *secure = g_strconcat(scheme, "s://", NULL);
	char *plain = g_strconcat(scheme, "://", NULL);
	const char *rest = NULL;
	if (g_str_has_prefix(url, secure)) {
		out->tls = true;
		rest = url + strlen(secure);
	} else if (g_str_has_prefix(url, plain)) {
		out->tls = false;
		rest = url + strlen(plain);
	}
	g_free(secure);
	g_free(plain);
	if (rest == NULL || rest[0] == '\0') {
		return false;
	}
	char *host = g_strdup(rest);
	char *slash = strchr(host, '/');
	if (slash != NULL) {
		*slash = '\0';
	}
	char *colon = strrchr(host, ':');
	out->port = out->tls ? tls_port : starttls_port;
	if (colon != NULL) {
		*colon = '\0';
		out->port = (guint16)atoi(colon + 1);
	}
	out->host = host;
	return out->host[0] != '\0' && out->port != 0;
}

struct account *account_load(char **error) {
	char *path = config_path();
	GKeyFile *kf = g_key_file_new();
	GError *e = NULL;
	struct account *a = NULL;
	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &e)) {
		*error = g_strdup_printf("No mail account set up: see %s in the "
			"README", path);
		g_clear_error(&e);
		goto out;
	}
	a = g_new0(struct account, 1);
	a->from = g_key_file_get_string(kf, "Account", "from", NULL);
	a->user = g_key_file_get_string(kf, "Account", "user", NULL);
	a->password_command = g_key_file_get_string(kf, "Account",
		"password-command", NULL);
	a->login_command = g_key_file_get_string(kf, "Passwords", "command", NULL);
	a->unlock_command = g_key_file_get_string(kf, "Passwords", "unlock", NULL);
	a->site = g_key_file_get_string(kf, "Passwords", "site", NULL);
	a->sent = g_key_file_get_string(kf, "Account", "sent", NULL);
	a->trash = g_key_file_get_string(kf, "Account", "trash", NULL);
	a->archive = g_key_file_get_string(kf, "Account", "archive", NULL);
	char *imap = g_key_file_get_string(kf, "Account", "imap", NULL);
	char *smtp = g_key_file_get_string(kf, "Account", "smtp", NULL);
	char *cert = g_key_file_get_string(kf, "Account", "certificate", NULL);
	if (a->from != NULL) {
		g_strstrip(a->from);
		InternetAddressList *list = internet_address_list_parse(NULL, a->from);
		InternetAddress *first = list != NULL &&
			internet_address_list_length(list) > 0 ?
			internet_address_list_get_address(list, 0) : NULL;
		if (first != NULL && INTERNET_ADDRESS_IS_MAILBOX(first)) {
			a->address = g_strdup(internet_address_mailbox_get_addr(
				INTERNET_ADDRESS_MAILBOX(first)));
		}
		g_clear_object(&list);
	}
	if (a->user == NULL && a->address != NULL) {
		a->user = g_strdup(a->address);
	}
	if (imap == NULL || !parse_server(g_strstrip(imap), "imap", 993, 143,
			&a->imap)) {
		*error = g_strdup_printf("%s: imap = imaps://your.server", path);
	} else if (smtp == NULL || !parse_server(g_strstrip(smtp), "smtp", 465, 587,
			&a->smtp)) {
		*error = g_strdup_printf("%s: smtp = smtps://your.server", path);
	} else if (a->address == NULL) {
		*error = g_strdup_printf("%s: from = Your Name <you@example.org>", path);
	} else if (a->password_command == NULL && a->login_command == NULL) {
		*error = g_strdup_printf("%s: password-command = a command that "
			"prints your password", path);
	} else if (cert != NULL) {
		char *file = expand_home(g_strstrip(cert));
		a->certificate = g_tls_certificate_new_from_file(file, &e);
		if (a->certificate == NULL) {
			*error = g_strdup_printf("%s: %s", file, e->message);
			g_clear_error(&e);
		}
		g_free(file);
	}
	if (*error == NULL && a->site == NULL) {
		a->site = g_strdup(a->imap.host);
	}
	g_free(imap);
	g_free(smtp);
	g_free(cert);
	if (*error != NULL) {
		account_free(a);
		a = NULL;
	}
out:
	g_key_file_free(kf);
	g_free(path);
	return a;
}

void account_free(struct account *a) {
	if (a == NULL) {
		return;
	}
	g_free(a->from);
	g_free(a->address);
	g_free(a->user);
	g_free(a->imap.host);
	g_free(a->smtp.host);
	g_free(a->password_command);
	g_free(a->login_command);
	g_free(a->unlock_command);
	g_free(a->site);
	g_free(a->sent);
	g_free(a->trash);
	g_free(a->archive);
	g_clear_object(&a->certificate);
	g_free(a);
}

char *account_password(struct account *a, char **error) {
	char *out = NULL, *err = NULL;
	int status = 0;
	GError *e = NULL;
	const char *argv[] = { "/bin/sh", "-c", a->password_command, NULL };
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
			&out, &err, &status, &e)) {
		*error = g_strdup(e->message);
		g_error_free(e);
		return NULL;
	}
	char *nl = out != NULL ? strpbrk(out, "\r\n") : NULL;
	if (nl != NULL) {
		*nl = '\0';
	}
	if (!g_spawn_check_wait_status(status, NULL) || out == NULL ||
			out[0] == '\0') {
		char *line = g_strstrip(err != NULL ? err : (err = g_strdup("")));
		char *first = strchr(line, '\n');
		if (first != NULL) {
			*first = '\0';
		}
		*error = g_strdup_printf("The password command failed%s%s",
			line[0] ? ": " : "", line);
		if (out != NULL) {
			memset(out, 0, strlen(out));
		}
		g_free(out);
		g_free(err);
		return NULL;
	}
	g_free(err);
	return out;
}
