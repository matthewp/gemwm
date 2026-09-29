/*
 * Where GemWeb keeps cookies, and so logins: cookies.txt in its data
 * directory, readable only by you. Not libsoup's SQLite store, which
 * writes expiry times through a 32-bit int: a login cookie meant to last
 * past 2038 came back expired, and you were logged out.
 */
#ifndef GEMWEB_COOKIES_H
#define GEMWEB_COOKIES_H

#include <stdbool.h>

/* Sets path to the cookie file in dir, creating it if need be, and the
 * first time, moving the old SQLite store's cookies into it. */
void cookies_prepare(const char *dir, char **path);

#endif
