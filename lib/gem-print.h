/*
 * GemWM's print dialog, for its own GTK applications: in place of GTK's,
 * a GEM one, drawn like the rest of the app, that picks the printer, the
 * copies and the pages, then prints without GTK's dialog. Printers come
 * from GTK (so from CUPS); "PDF file" saves to the Documents folder.
 */
#ifndef GEMWM_GEM_PRINT_H
#define GEMWM_GEM_PRINT_H

#include <gtk/gtk.h>
#include <stdbool.h>

/* GTK prints through xdg-desktop-portal when there is one, and the portal
 * puts up its own print dialog whatever the app asks. Call this first
 * thing in main, before GTK starts, so the app's printing goes straight to
 * CUPS instead. (It's GTK's no-portals switch, so the app's other portals
 * go too; GemWM's apps, drawn by hand, don't use them.) */
void gem_print_setup(void);

/* An application's own option, as a check box: value is shown, and set
 * when Print is pressed (before any page is drawn). */
struct gem_print_choice {
	const char *label;
	bool *value;
};

/* Asks where and how to print, then runs op, which the caller has set up
 * (job name, begin-print, draw-page). done gets a line for the app's status
 * when it's over, or NULL if the dialog was cancelled; op is unreffed after.
 * The printer and choices are remembered for next time, while the app
 * runs. */
void gem_print_dialog(GtkWindow *parent, GtkPrintOperation *op,
	const struct gem_print_choice *choices, int n_choices,
	void (*done)(const char *message, void *data), void *data);

/* For printing that isn't a GtkPrintOperation (WebKit's): the same
 * dialog, but on Print, run gets the settings chosen (the printer, copies
 * and pages; for a PDF, GTK's "Print to File" printer and the file) and
 * prints with them, then calls gem_print_job_done, with an error line if
 * it failed. name is the job's, and the PDF's. */
struct gem_print_job;
typedef void (*gem_print_run_fn)(GtkPrintSettings *settings,
	struct gem_print_job *job, void *data);
void gem_print_dialog_run(GtkWindow *parent, const char *name,
	gem_print_run_fn run,
	void (*done)(const char *message, void *data), void *data);
void gem_print_job_done(struct gem_print_job *job, const char *error);

#endif
