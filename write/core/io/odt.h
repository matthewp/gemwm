/* odt.h — OpenDocument Text import/export for the document model.
 *
 * Supported: paragraphs and headings, character formatting (bold, italic,
 * underline, size, font family), paragraph alignment, spacing, line height,
 * page size and margins, named and automatic styles with inheritance.
 * Lists and tables are read as plain paragraphs; images, notes and tracked
 * changes are skipped. */
#ifndef WP_ODT_H
#define WP_ODT_H

#include <stdbool.h>

#include "doc/document.h"

/* Both return false and set *err (malloc'd) on failure. */
bool wp_odt_read(WpDocument *doc, const char *path, char **err);
bool wp_odt_write(const WpDocument *doc, const char *path, char **err);

#endif
