#include "zip.h"

#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static char *errf(const char *fmt, const char *a)
{
  size_t n = strlen(fmt) + (a ? strlen(a) : 0) + 1;
  char *s = malloc(n);
  snprintf(s, n, fmt, a ? a : "");
  return s;
}

/* ---- reading (libarchive) ------------------------------------------------ */

bool wp_zip_read_entry(const char *path, const char *name, char **data, size_t *len, char **err)
{
  *data = NULL; *len = 0;
  struct archive *a = archive_read_new();
  archive_read_support_format_zip(a);
  if (archive_read_open_filename(a, path, 64 * 1024) != ARCHIVE_OK) {
    if (err) *err = errf("cannot open archive: %s", archive_error_string(a));
    archive_read_free(a);
    return false;
  }

  bool found = false;
  struct archive_entry *e;
  while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
    if (strcmp(archive_entry_pathname(e), name) != 0) continue;
    size_t cap = 64 * 1024, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
      if (n == cap) { cap *= 2; buf = realloc(buf, cap + 1); }
      la_ssize_t r = archive_read_data(a, buf + n, cap - n);
      if (r < 0) { free(buf); buf = NULL; break; }
      if (r == 0) break;
      n += (size_t)r;
    }
    if (!buf) { if (err) *err = errf("error reading %s", name); break; }
    buf[n] = 0;
    *data = buf; *len = n;
    found = true;
    break;
  }
  if (!found && err && !*err) *err = errf("archive has no %s", name);
  archive_read_free(a);
  return found;
}

/* ---- writing -------------------------------------------------------------- */

typedef struct CentralEntry {
  char    *name;
  uint32_t crc, csize, usize, offset;
  uint16_t method;
} CentralEntry;

struct WpZipWriter {
  FILE *f;
  CentralEntry *entries;
  size_t n, cap;
  uint16_t dos_time, dos_date;
  bool failed;
};

static void put16(FILE *f, uint32_t v) { unsigned char b[2] = { v & 0xff, (v >> 8) & 0xff }; fwrite(b, 1, 2, f); }
static void put32(FILE *f, uint32_t v) { unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff }; fwrite(b, 1, 4, f); }

WpZipWriter *wp_zip_writer_open(const char *path, char **err)
{
  FILE *f = fopen(path, "wb");
  if (!f) { if (err) *err = errf("cannot write %s", path); return NULL; }
  WpZipWriter *z = calloc(1, sizeof *z);
  z->f = f;
  time_t now = time(NULL);
  struct tm tm; localtime_r(&now, &tm);
  z->dos_time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  z->dos_date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  return z;
}

bool wp_zip_writer_add(WpZipWriter *z, const char *name, const void *data, size_t len, bool store)
{
  if (z->failed) return false;
  unsigned char *comp = NULL;
  size_t clen = len;
  uint16_t method = 0;

  if (!store && len > 0) {
    z_stream st = { 0 };
    if (deflateInit2(&st, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { z->failed = true; return false; }
    size_t bound = deflateBound(&st, (uLong)len);
    comp = malloc(bound);
    st.next_in = (Bytef *)data; st.avail_in = (uInt)len;
    st.next_out = comp; st.avail_out = (uInt)bound;
    int rc = deflate(&st, Z_FINISH);
    clen = st.total_out;
    deflateEnd(&st);
    if (rc != Z_STREAM_END) { free(comp); z->failed = true; return false; }
    method = 8;
  }

  uint32_t crc = (uint32_t)crc32(0, (const Bytef *)data, (uInt)len);
  long offset = ftell(z->f);
  size_t nlen = strlen(name);

  put32(z->f, 0x04034b50);          /* local file header */
  put16(z->f, method ? 20 : 10);    /* version needed */
  put16(z->f, 0x0800);              /* flags: UTF-8 names */
  put16(z->f, method);
  put16(z->f, z->dos_time);
  put16(z->f, z->dos_date);
  put32(z->f, crc);
  put32(z->f, (uint32_t)clen);
  put32(z->f, (uint32_t)len);
  put16(z->f, (uint16_t)nlen);
  put16(z->f, 0);                   /* extra length */
  fwrite(name, 1, nlen, z->f);
  fwrite(method ? (const void *)comp : data, 1, clen, z->f);
  free(comp);

  if (z->n == z->cap) {
    z->cap = z->cap ? z->cap * 2 : 8;
    z->entries = realloc(z->entries, z->cap * sizeof *z->entries);
  }
  z->entries[z->n++] = (CentralEntry){ strdup(name), crc, (uint32_t)clen, (uint32_t)len, (uint32_t)offset, method };
  return !ferror(z->f);
}

bool wp_zip_writer_close(WpZipWriter *z, char **err)
{
  bool ok = !z->failed;
  long cd_start = ftell(z->f);
  for (size_t i = 0; i < z->n; i++) {
    CentralEntry *e = &z->entries[i];
    size_t nlen = strlen(e->name);
    put32(z->f, 0x02014b50);
    put16(z->f, 0x031e);            /* made by: unix, 3.0 */
    put16(z->f, e->method ? 20 : 10);
    put16(z->f, 0x0800);
    put16(z->f, e->method);
    put16(z->f, z->dos_time);
    put16(z->f, z->dos_date);
    put32(z->f, e->crc);
    put32(z->f, e->csize);
    put32(z->f, e->usize);
    put16(z->f, (uint16_t)nlen);
    put16(z->f, 0);                 /* extra */
    put16(z->f, 0);                 /* comment */
    put16(z->f, 0);                 /* disk */
    put16(z->f, 0);                 /* internal attrs */
    put32(z->f, 0100644u << 16);    /* external attrs: -rw-r--r-- */
    put32(z->f, e->offset);
    fwrite(e->name, 1, nlen, z->f);
    free(e->name);
  }
  long cd_end = ftell(z->f);
  put32(z->f, 0x06054b50);
  put16(z->f, 0); put16(z->f, 0);
  put16(z->f, (uint16_t)z->n); put16(z->f, (uint16_t)z->n);
  put32(z->f, (uint32_t)(cd_end - cd_start));
  put32(z->f, (uint32_t)cd_start);
  put16(z->f, 0);
  if (ferror(z->f)) ok = false;
  if (fclose(z->f) != 0) ok = false;
  if (!ok && err) *err = errf("error writing zip%s", "");
  free(z->entries);
  free(z);
  return ok;
}
