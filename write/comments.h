/*
 * The comments panel at the right of the page: a card per comment, in
 * the document's order, its replies under it. Click a card to select its
 * text, click its words to edit them, Reply and Delete on its header. A
 * new comment is written in a draft card at the top. Drag the panel's
 * left edge to widen it.
 *
 * Every change goes through the command table, by way of run, so the
 * window reports failures the same way as for everything else.
 */
#ifndef GEMWRITE_COMMENTS_H
#define GEMWRITE_COMMENTS_H

#include <gtk/gtk.h>
#include "page.h"

struct comments;

typedef void (*comments_run_fn)(const char *command, GVariant *arg,
	void *data);
/* shown_changed: the panel was shown or hidden (for the View menu). */
struct comments *comments_new(WpPageView *view, comments_run_fn run,
	void (*shown_changed)(void *data), void *data);
GtkWidget *comments_widget(struct comments *c);

/* The document changed: rebuild the cards, soon. */
void comments_refresh(struct comments *c);
/* The caret moved: mark the card its text is in. */
void comments_caret_moved(struct comments *c);
/* A draft for the selection. */
void comments_add(struct comments *c);
bool comments_shown(struct comments *c);
void comments_show(struct comments *c, bool shown);

#endif
