/* commands.h — the one table of things a user can do to a document.
 *
 * Every entry names an operation on a WpEditor with a GVariant parameter
 * type. The GTK window turns each entry into a GAction (win.<name>), binds
 * its accelerator, and lists it in the shortcuts dialog; the CLI parses
 * argv against the same parameter type and calls the same function. Add a
 * row here and every front end gains the command.
 *
 * Queries are commands that return a value in *out instead of editing.
 */
#ifndef WP_COMMANDS_H
#define WP_COMMANDS_H

#include <glib.h>
#include <stdbool.h>

#include "edit/editor.h"

typedef struct WpCommand {
  const char *name;      /* "insert-text"; also the GAction name */
  const char *params;    /* GVariant type string, NULL for none */
  const char *usage;     /* argument names for help output, e.g. "TEXT" */
  const char *summary;   /* short title: "Bold", "Save As" */
  const char *group;     /* "File", "Editing", "Formatting", ... */
  const char *accel;     /* default GTK accelerator, NULL if unbound */
  /* Returns false and sets *err (malloc'd) on failure. *out may be left
   * NULL; when set the caller owns the reference. */
  bool (*run)(WpEditor *ed, GVariant *arg, GVariant **out, char **err);
  const char *alt_accel; /* second accelerator, NULL for none */
} WpCommand;

const WpCommand *wp_commands(size_t *count);
const WpCommand *wp_command_find(const char *name);
bool wp_command_run(WpEditor *ed, const char *name, GVariant *arg, GVariant **out, char **err);

/* Number of argv words the command consumes: one per leaf of its type. */
int  wp_command_arity(const WpCommand *cmd);
/* Build the parameter from argv words (exactly wp_command_arity of them). */
bool wp_command_parse_args(const WpCommand *cmd, char **argv, GVariant **out, char **err);

/* Position syntax shared by every front end:
 *   start | end | caret | anchor | P | P:C
 * where P is a 0-based paragraph index and C a 0-based character offset. */
bool      wp_editor_parse_pos(WpEditor *ed, const char *spec, WpPos *out, char **err);

/* The name comments are signed with when the editor has none: the "author"
 * key in ~/.config/gemwrite/settings.ini, else the account's full name, else
 * the login name. Returned string is interned. */
const char *wp_default_author(void);
/* The "spell-language" key in the settings file, else NULL for the locale's language. */
const char *wp_default_spell_language(void);
/* Give the editor a checker if it has none, for the default language.
 * False with *err (malloc'd) when no dictionary is available. */
bool        wp_editor_ensure_spell(WpEditor *ed, char **err);
GVariant *wp_editor_pos_variant(WpEditor *ed, WpPos p);   /* a{sv} with para and offset (chars) */

#endif
