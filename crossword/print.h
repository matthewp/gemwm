/*
 * Printing a crossword: a newspaper-style page, through GTK's print
 * dialog (or straight to a PDF).
 */
#ifndef GEMWM_CROSSWORD_PRINT_H
#define GEMWM_CROSSWORD_PRINT_H

#include <gtk/gtk.h>
#include "puz.h"

/* The print dialog, then the printer. subtitle goes under the title (the
 * source and date, say). done gets a message to show when it's over. */
void print_puzzle(GtkWindow *parent, const struct puzzle *p,
	const char *subtitle, void (*done)(const char *message, void *data),
	void *data);

/* The same pages, into a PDF file. */
bool print_puzzle_pdf(const struct puzzle *p, const char *subtitle,
	bool answers, const char *path, GError **error);

#endif
