/*
 * File > Print...: the document's pages, as the page view shows them,
 * through GemWM's print dialog (to a printer or a PDF file).
 */
#ifndef GEMWRITE_PRINT_H
#define GEMWRITE_PRINT_H

#include <gtk/gtk.h>
#include "edit/editor.h"

/* done gets a line for the info line when it's over (NULL if cancelled). */
void print_document(GtkWindow *parent, WpEditor *ed, const char *name,
	void (*done)(const char *message, void *data), void *data);

#endif
