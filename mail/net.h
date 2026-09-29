/*
 * Connections for libetpan's IMAP and SMTP over GIO, whose TLS checks the
 * server's whole certificate chain against the system's trusted ones and
 * its name (libetpan's own TLS doesn't). Blocking: for the mail thread.
 */
#ifndef GEMWM_MAIL_NET_H
#define GEMWM_MAIL_NET_H

#include <gio/gio.h>
#include <libetpan/libetpan.h>
#include <stdbool.h>

/* Connects to host:port, with TLS at once if tls. pinned, if set, is the
 * one certificate trusted, whatever the system thinks of it. */
mailstream *net_connect(const char *host, guint16 port, bool tls,
	GTlsCertificate *pinned, GError **error);

/* After the protocol's STARTTLS: TLS from here on, over the same socket. */
bool net_starttls(mailstream *stream, const char *host, guint16 port,
	GTlsCertificate *pinned, GError **error);

#endif
