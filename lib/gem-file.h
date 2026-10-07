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

/* path is the file (or folder) chosen, or NULL if it was cancelled. */
typedef void (*gem_file_fn)(const char *path, void *data);

/* Choosing a file to open, a name to save as, or a folder (the list
 * shows only folders, and OK takes the one selected, or the one it's in). */
enum gem_file_mode { GEM_FILE_OPEN, GEM_FILE_SAVE, GEM_FILE_FOLDER };

/* folder NULL is the last folder chosen from, else the Documents folder;
 * name is the selection to start with (may be NULL). pattern is a glob
 * (several separated by commas), e.g. "*.odt,*.txt"; NULL is "*". ok is
 * the OK button's label (NULL: "OK"). */
void gem_file_choose(GtkOverlay *host, const char *title,
	enum gem_file_mode mode, const char *folder, const char *name,
	const char *pattern, const char *ok, gem_file_fn done, void *data);

/* How big the selector's box is: for a window that's only that. */
void gem_file_size(int *w, int *h);

/* gem_file_choose to open (save false) or save, with an OK button. */
void gem_file_select(GtkOverlay *host, const char *title, bool save,
	const char *folder, const char *name, const char *pattern,
	gem_file_fn done, void *data);

/* MIME types (image/png, text/plain, or a whole kind like image/ and a
 * star) as the globs their files
 * have, from the system's MIME database, for a pattern: "*.png,*.jpg".
 * NULL if none of them has any. */
char *gem_file_pattern_for_types(const char *const *types);

#endif
