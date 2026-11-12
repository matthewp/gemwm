/*
 * The Flagged view: every flagged message, from every folder, newest
 * first, with the folder it's in. GemMail fills it, first from what its
 * cache knows and then from the server's answer. Return or a double-click
 * opens one; F unflags it, and it goes from the list.
 */
#ifndef GEMWM_MAIL_FLAGVIEW_H
#define GEMWM_MAIL_FLAGVIEW_H

#include <gtk/gtk.h>
#include <stdbool.h>

struct flagview;

/* A message chosen to open, or to unflag: from its folder. */
typedef void (*flagview_fn)(const char *mailbox, guint32 uid, void *data);

struct flagview *flagview_new(flagview_fn open, flagview_fn unflag,
	void *data);
GtkWidget *flagview_widget(struct flagview *v);
/* Shows messages (struct summary, each with its mailbox; copied), their
 * folders named by names (mailbox to name). searching: the server's
 * still being asked, for the title. The selection's kept. */
void flagview_load(struct flagview *v, GPtrArray *messages, GHashTable *names,
	bool searching);
void flagview_focus(struct flagview *v);

#endif
