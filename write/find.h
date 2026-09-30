/*
 * Find, and Find and Replace: a GEM box over the page's top-right corner
 * that stays while you work. While it's up the editor has its term, so
 * every match is marked; the term stays afterwards for Find Next.
 */
#ifndef GEMWRITE_FIND_H
#define GEMWRITE_FIND_H

#include <gtk/gtk.h>
#include "page.h"

struct find;

struct find *find_new(GtkOverlay *host, WpPageView *view);
void find_show(struct find *f, bool replace);
void find_hide(struct find *f);
bool find_shown(struct find *f);
/* Find Next (dir 1) and Previous (-1): the box comes back if it was
 * closed, but the focus stays in the document. */
void find_step(struct find *f, int dir);
/* The selection changed: "3 of 12" follows. */
void find_update(struct find *f);

#endif
