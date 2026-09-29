/*
 * Converting ad-blocking filter lists in Adblock Plus's format (EasyList,
 * EasyPrivacy) to WebKit's content-blocker rules, which WebKit compiles and
 * applies itself, fast. After Adblock Plus's own abp2blocklist, for the
 * filters WebKit can express: address patterns with their common options
 * (third-party, domain=, resource types, match-case), element hiding, and
 * exceptions, including whole sites. The rest (regular expressions, $csp,
 * $redirect, extended selectors) is skipped.
 */
#ifndef GEMWEB_ADBLOCK_H
#define GEMWEB_ADBLOCK_H

#include <glib.h>

struct adblock;

struct adblock *adblock_new(void);
/* Adds a list's filters, one per line. */
void adblock_add_list(struct adblock *a, const char *text);
/* The rules as WebKit's JSON, and how many; frees a. */
GBytes *adblock_finish(struct adblock *a, guint *n_rules);

#endif
