/*
 * The ruler above the page, in inches or centimetres (as the locale
 * measures), lined up with the page: the caret paragraph's indents and tab
 * stops on it. Drag the bottom-left marker for the left indent, the
 * top-left one for the first line, the right one for the right indent;
 * drag a tab stop to move it or off the ruler to remove it; click the
 * scale between the indents to add one. Each change is one undo step,
 * for every paragraph the selection touches.
 */
#ifndef GEMWRITE_RULER_H
#define GEMWRITE_RULER_H

#include <gtk/gtk.h>
#include "page.h"

GtkWidget *ruler_new(WpPageView *view);

#endif
