/*
 * The mail account, from ~/.config/gemmail/settings:
 *
 *   [Account]
 *   from = Jane Doe <jane@example.org>
 *   user = jane@example.org             # the login; the From address if unset
 *   imap = imaps://imap.example.org     # imaps: TLS; imap: STARTTLS (:port)
 *   smtp = smtps://smtp.example.org     # smtps: TLS; smtp: STARTTLS (:port)
 *   password-command = pass show mail   # prints the password; or
 *   sent = Sent                         # folders; found by their flags if
 *   trash = Trash                       # the server marks them, else by
 *   archive = Archive                   # name
 *   certificate = ~/mail-server.pem     # a self-signed server's, trusted
 *
 *   [Passwords]                         # a password manager that locks,
 *   command = gemweb-bw                 # as GemWeb's (see passwords.h):
 *   unlock = gemweb-bw --unlock         # its login for site whose username
 *   site = example.org                  # is user; site: the IMAP server's
 *
 * Connections are always encrypted, and the server's certificate checked
 * against the system's (or matching the one given).
 */
#ifndef GEMWM_MAIL_ACCOUNT_H
#define GEMWM_MAIL_ACCOUNT_H

#include <gio/gio.h>
#include <stdbool.h>

struct server {
	char *host;
	guint16 port;
	bool tls; /* from the start; else STARTTLS */
};

struct account {
	char *from;      /* as written: "Name <address>" */
	char *address;   /* just the address */
	char *user;
	struct server imap, smtp;
	char *password_command;
	char *login_command, *unlock_command, *site; /* [Passwords] */
	char *sent, *trash, *archive;
	GTlsCertificate *certificate; /* trusted as is, if set */
};

/* The account, or NULL and why (no file, or something missing). */
struct account *account_load(char **error);
void account_free(struct account *a);

/* Runs the password command: the password (a secret: wipe it), or NULL
 * and why. */
char *account_password(struct account *a, char **error);

#endif
