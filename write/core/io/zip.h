/* zip.h — the little bit of zip we need for OpenDocument packages. */
#ifndef WP_ZIP_H
#define WP_ZIP_H

#include <stdbool.h>
#include <stddef.h>

/* Read one named entry from a zip file into a malloc'd buffer (NUL-terminated
 * for convenience). Returns false with *err set (malloc'd) on failure or when
 * the entry is absent. */
bool wp_zip_read_entry(const char *path, const char *name, char **data, size_t *len, char **err);

/* Sequential zip writer. ODF requires "mimetype" to be the first entry,
 * stored uncompressed, with no extra fields — so we write the container
 * ourselves rather than through a general-purpose library. */
typedef struct WpZipWriter WpZipWriter;
WpZipWriter *wp_zip_writer_open(const char *path, char **err);
bool         wp_zip_writer_add(WpZipWriter *z, const char *name, const void *data, size_t len, bool store);
bool         wp_zip_writer_close(WpZipWriter *z, char **err);   /* frees z */

#endif
