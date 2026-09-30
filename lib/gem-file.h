/*
 * GEM's item selector, for opening and saving files: the directory and its
 * pattern on one line (edit it to go elsewhere or see other files), the
 * folder's contents in a list with a close box to go up a folder, and the
 * selection, the file's name, beside it. Double-click a folder to go in,
 * a file to take it. Up and Down move through the list.
 *
 * It holds the window like an alert (see gem-alert.h). Saving over a file
 * that's there asks first.
 */
#ifndef GEMWM_GEM_FILE_H
#define GEMWM_GEM_FILE_H

#include <gtk/gtk.h>
#include <stdbool.h>

/* path is the file chosen, or NULL if it was cancelled. */
typedef void (*gem_file_fn)(const char *path, void *data);

/* folder NULL is the last folder chosen from, else the Documents folder;
 * name is the selection to start with (may be NULL). pattern is a glob
 * (several separated by commas), e.g. "*.odt,*.txt". */
void gem_file_select(GtkOverlay *host, const char *title, bool save,
	const char *folder, const char *name, const char *pattern,
	gem_file_fn done, void *data);

#endif
