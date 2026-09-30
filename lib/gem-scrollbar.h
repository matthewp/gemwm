/*
 * GEM's scroll bar for a GtkAdjustment: arrow boxes at the ends, a dithered
 * track, and a white slider as long as the part shown. The arrows step
 * (and repeat while held), the track pages, the slider drags. Put it
 * beside a GtkScrolledWindow whose own bars are GTK_POLICY_EXTERNAL, on
 * that window's adjustment.
 */
#ifndef GEMWM_GEM_SCROLLBAR_H
#define GEMWM_GEM_SCROLLBAR_H

#include <gtk/gtk.h>

GtkWidget *gem_scrollbar_new(GtkOrientation orientation, GtkAdjustment *adj);

#endif
