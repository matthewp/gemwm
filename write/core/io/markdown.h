/* markdown.h — Markdown export for the document model.
 *
 * One line per paragraph, a blank line between them. Title and headings
 * become ATX headings (Title and Heading 1 are "#", Heading 2 "##", Heading 3
 * "###"); Quote paragraphs become a block quote and Note paragraphs a
 * "> [!NOTE]" callout, with consecutive ones joined into one block. Bold and
 * italic use asterisks, underline uses <u>. Alignment, spacing, fonts and
 * comments have no Markdown form and are dropped. Text that would otherwise
 * read as markup is backslash-escaped. There is no reader: this is a one-way
 * export. */
#ifndef WP_MARKDOWN_H
#define WP_MARKDOWN_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/document.h"

/* The document as Markdown, malloc'd and NUL-terminated (empty for an empty
 * document). *out_len may be NULL. */
char *wp_markdown_format(const WpDocument *doc, size_t *out_len);
/* Write it to path. Returns false and sets *err (malloc'd) on failure. */
bool  wp_markdown_write(const WpDocument *doc, const char *path, char **err);

#endif
