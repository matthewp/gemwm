/*
 * Style > Font...: a GEM box with every font family in a list, the size,
 * and a line of the text in them. Typing in the family field goes to the
 * first family that starts with it. OK sets both on the selection (or on
 * what's typed next).
 */
#ifndef GEMWRITE_FONTS_H
#define GEMWRITE_FONTS_H

#include <gtk/gtk.h>
#include "edit/editor.h"

void font_dialog(GtkOverlay *host, WpEditor *ed);

#endif
