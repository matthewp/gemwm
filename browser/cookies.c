/*
 * GemWeb's cookies: the file WebKit keeps them in, see cookies.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "cookies.h"

/* libsoup's SQLite store wrote expiry times through a 32-bit int, so any
 * after 2038 came back wrapped, often into the past. No GemWeb stored a
 * cookie before 2026, so an expiry before then was wrapped: unwrap it. */
#define GEMWEB_BEGAN 1767225600 /* 2026-01-01 */

static gint64 unwrap(gint64 expiry) {
	while (expiry < GEMWEB_BEGAN) {
		expiry += G_GINT64_CONSTANT(1) << 32;
	}
	return expiry;
}

static const char *same_site(int policy) {
	/* libsoup's SoupSameSitePolicy: none, lax, strict. */
	return policy == 0 ? "None" : policy == 2 ? "Strict" : "Lax";
}

/* Writes the cookies in the SQLite store as libsoup's cookies.txt lines
 * (Netscape's format, plus SameSite). */
static bool convert(sqlite3 *db, FILE *out) {
	sqlite3_stmt *stmt;
	if (sqlite3_prepare_v2(db, "SELECT name, value, host, path, expiry,"
			" isSecure, isHttpOnly, sameSite FROM moz_cookies"
			" WHERE expiry IS NOT NULL AND expiry != 0",
			-1, &stmt, NULL) != SQLITE_OK) {
		return false;
	}
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 0);
		const char *value = (const char *)sqlite3_column_text(stmt, 1);
		const char *host = (const char *)sqlite3_column_text(stmt, 2);
		const char *path = (const char *)sqlite3_column_text(stmt, 3);
		if (name == NULL || value == NULL || host == NULL || path == NULL ||
				strpbrk(name, "\t\r\n") || strpbrk(value, "\t\r\n")) {
			continue;
		}
		fprintf(out, "%s%s\t%s\t%s\t%s\t%" G_GINT64_FORMAT "\t%s\t%s\t%s\n",
			sqlite3_column_int(stmt, 6) ? "#HttpOnly_" : "",
			host, host[0] == '.' ? "TRUE" : "FALSE", path,
			sqlite3_column_int(stmt, 5) ? "TRUE" : "FALSE",
			unwrap(sqlite3_column_int64(stmt, 4)), name, value,
			same_site(sqlite3_column_int(stmt, 7)));
	}
	sqlite3_finalize(stmt);
	return true;
}

void cookies_prepare(const char *dir, char **path) {
	*path = g_build_filename(dir, "cookies.txt", NULL);
	/* Only you can read your logins. */
	int fd = g_open(*path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		if (errno != EEXIST) {
			g_printerr("gemweb: can't create %s: %s\n", *path,
				g_strerror(errno));
		}
		return;
	}
	/* A new file: bring over the cookies of the old SQLite store. */
	char *old = g_build_filename(dir, "cookies.sqlite", NULL);
	sqlite3 *db = NULL;
	FILE *out = fdopen(fd, "w");
	if (g_file_test(old, G_FILE_TEST_EXISTS) &&
			sqlite3_open_v2(old, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
			convert(db, out) && fflush(out) == 0 && fsync(fd) == 0) {
		/* It holds the same logins, readable by anyone: don't leave it. */
		sqlite3_close(db);
		db = NULL;
		g_unlink(old);
	}
	sqlite3_close(db);
	fclose(out);
	g_free(old);
}
