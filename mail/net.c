/*
 * libetpan streams over GIO: see net.h.
 */
#include "net.h"

#define TIMEOUT 60 /* seconds, for any one read or write */

struct net {
	GSocketConnection *socket;
	GIOStream *io; /* the socket, or TLS over it */
};

static ssize_t net_read(mailstream_low *low, void *buf, size_t len) {
	struct net *n = low->data;
	return g_input_stream_read(g_io_stream_get_input_stream(n->io), buf, len,
		NULL, NULL);
}

static ssize_t net_write(mailstream_low *low, const void *buf, size_t len) {
	struct net *n = low->data;
	return g_output_stream_write(g_io_stream_get_output_stream(n->io), buf, len,
		NULL, NULL);
}

static int net_close(mailstream_low *low) {
	struct net *n = low->data;
	g_io_stream_close(n->io, NULL, NULL);
	return 0;
}

static int net_get_fd(mailstream_low *low) {
	struct net *n = low->data;
	return g_socket_get_fd(g_socket_connection_get_socket(n->socket));
}

static void net_free(mailstream_low *low) {
	struct net *n = low->data;
	g_object_unref(n->io);
	g_object_unref(n->socket);
	g_free(n);
	free(low);
}

static void net_cancel(mailstream_low *low) {
	struct net *n = low->data;
	g_socket_shutdown(g_socket_connection_get_socket(n->socket), TRUE, TRUE,
		NULL);
}

static mailstream_low_driver driver = {
	.mailstream_read = net_read,
	.mailstream_write = net_write,
	.mailstream_close = net_close,
	.mailstream_get_fd = net_get_fd,
	.mailstream_free = net_free,
	.mailstream_cancel = net_cancel,
};

/* A pinned certificate is trusted even though the system doesn't know it
 * (self-signed); but only that one. */
static gboolean accept_pinned(GTlsConnection *tls, GTlsCertificate *peer,
		GTlsCertificateFlags errors, gpointer pinned) {
	return g_tls_certificate_is_same(peer, pinned);
}

static GIOStream *tls_over(GIOStream *base, const char *host, guint16 port,
		GTlsCertificate *pinned, GError **error) {
	GSocketConnectable *identity = g_network_address_new(host, port);
	GIOStream *tls = g_tls_client_connection_new(base, identity, error);
	g_object_unref(identity);
	if (tls == NULL) {
		return NULL;
	}
	if (pinned != NULL) {
		g_signal_connect(tls, "accept-certificate", G_CALLBACK(accept_pinned),
			pinned);
	}
	if (!g_tls_connection_handshake(G_TLS_CONNECTION(tls), NULL, error)) {
		g_object_unref(tls);
		return NULL;
	}
	return tls;
}

static mailstream *stream_for(GSocketConnection *socket, GIOStream *io) {
	struct net *n = g_new0(struct net, 1);
	n->socket = g_object_ref(socket);
	n->io = g_object_ref(io);
	return mailstream_new(mailstream_low_new(n, &driver), 8192);
}

mailstream *net_connect(const char *host, guint16 port, bool tls,
		GTlsCertificate *pinned, GError **error) {
	GSocketClient *client = g_socket_client_new();
	g_socket_client_set_timeout(client, TIMEOUT);
	GSocketConnection *socket = g_socket_client_connect_to_host(client, host,
		port, NULL, error);
	g_object_unref(client);
	if (socket == NULL) {
		return NULL;
	}
	GIOStream *io = G_IO_STREAM(g_object_ref(socket));
	if (tls) {
		g_object_unref(io);
		io = tls_over(G_IO_STREAM(socket), host, port, pinned, error);
		if (io == NULL) {
			g_object_unref(socket);
			return NULL;
		}
	}
	mailstream *stream = stream_for(socket, io);
	g_object_unref(io);
	g_object_unref(socket);
	return stream;
}

bool net_starttls(mailstream *stream, const char *host, guint16 port,
		GTlsCertificate *pinned, GError **error) {
	mailstream_low *old = mailstream_get_low(stream);
	struct net *n = old->data;
	GIOStream *tls = tls_over(G_IO_STREAM(n->socket), host, port, pinned,
		error);
	if (tls == NULL) {
		return false;
	}
	mailstream_set_low(stream, mailstream_low_new(
		g_memdup2(&(struct net){ g_object_ref(n->socket), tls },
			sizeof(struct net)), &driver));
	/* The old one's TLS-less view of the socket goes; the socket stays. */
	net_free(old);
	return true;
}
