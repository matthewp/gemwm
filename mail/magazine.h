/*
 * The Newsletters view: your newsletters as a magazine rack. First a
 * shelf, a GEM icon for each publication (its own logo, see logos.h),
 * then a publication's issues, newest first, then an issue as a message.
 * It spans every folder GemMail has seen: the messages a category's put
 * in (see categories.h), from the cache.
 */
#ifndef GEMWM_MAIL_MAGAZINE_H
#define GEMWM_MAIL_MAGAZINE_H

#include <gtk/gtk.h>
#include "cache.h"

struct magazine;

/* An issue chosen: open it, from its folder. */
typedef void (*magazine_open_fn)(const char *mailbox, guint32 uid, void *data);

struct magazine *magazine_new(struct cache *cache, magazine_open_fn open,
	void *data);
GtkWidget *magazine_widget(struct magazine *m);
/* Reads the category's messages again; where you were is kept. */
void magazine_load(struct magazine *m, const char *category);
void magazine_redraw(struct magazine *m);
void magazine_focus(struct magazine *m);

#endif
