/* gemwrite-cli — drive a document from the shell or a script.
 *
 *   gemwrite-cli [FILE] COMMAND [ARGS]... [COMMAND [ARGS]...]...
 *   gemwrite-cli [FILE] --script SCRIPT     one command per line ("-" = stdin)
 *   gemwrite-cli --list                     every command with its arguments
 *
 * Commands take a fixed number of arguments, so several can follow each
 * other without separators. Nothing is written unless a command saves.
 * Query results are printed as JSON, one value per line, so an agent can
 * read the document, decide, and edit in the same run. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit/commands.h"
#include "edit/editor.h"
#include "layout/layout_pango.h"

/* ---- JSON output --------------------------------------------------------- */

static void json_string(const char *s)
{
  putchar('"');
  for (const char *p = s; *p; p++) {
    switch (*p) {
    case '"':  fputs("\\\"", stdout); break;
    case '\\': fputs("\\\\", stdout); break;
    case '\n': fputs("\\n", stdout); break;
    case '\r': fputs("\\r", stdout); break;
    case '\t': fputs("\\t", stdout); break;
    default:
      if ((unsigned char)*p < 0x20) printf("\\u%04x", *p);
      else putchar(*p);
    }
  }
  putchar('"');
}

static void json_value(GVariant *v)
{
  switch (g_variant_classify(v)) {
  case G_VARIANT_CLASS_BOOLEAN: fputs(g_variant_get_boolean(v) ? "true" : "false", stdout); break;
  case G_VARIANT_CLASS_INT32:   printf("%d", g_variant_get_int32(v)); break;
  case G_VARIANT_CLASS_UINT32:  printf("%u", g_variant_get_uint32(v)); break;
  case G_VARIANT_CLASS_INT64:   printf("%" G_GINT64_FORMAT, g_variant_get_int64(v)); break;
  case G_VARIANT_CLASS_UINT64:  printf("%" G_GUINT64_FORMAT, g_variant_get_uint64(v)); break;
  case G_VARIANT_CLASS_DOUBLE: {
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    fputs(g_ascii_formatd(buf, sizeof buf, "%.15g", g_variant_get_double(v)), stdout);   /* 1.15, not 1.1499999999999999 */
    break;
  }
  case G_VARIANT_CLASS_STRING:  json_string(g_variant_get_string(v, NULL)); break;
  case G_VARIANT_CLASS_VARIANT: {
    GVariant *inner = g_variant_get_variant(v);
    json_value(inner);
    g_variant_unref(inner);
    break;
  }
  case G_VARIANT_CLASS_ARRAY:
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_DICTIONARY)) {
      putchar('{');
      GVariantIter it;
      GVariant *key, *val;
      g_variant_iter_init(&it, v);
      gboolean first = TRUE;
      while (g_variant_iter_loop(&it, "{@?@*}", &key, &val)) {
        if (!first) putchar(',');
        first = FALSE;
        json_value(key);
        putchar(':');
        json_value(val);
      }
      putchar('}');
      break;
    }
    /* fall through: plain arrays and tuples print as lists */
    G_GNUC_FALLTHROUGH;
  case G_VARIANT_CLASS_TUPLE: {
    putchar('[');
    gsize n = g_variant_n_children(v);
    for (gsize i = 0; i < n; i++) {
      if (i) putchar(',');
      GVariant *c = g_variant_get_child_value(v, i);
      json_value(c);
      g_variant_unref(c);
    }
    putchar(']');
    break;
  }
  default:
    json_string(g_variant_print(v, FALSE));
  }
}

/* ---- running ------------------------------------------------------------- */

/* Run the commands in argv[0..argc). Returns the number of words consumed
 * or -1 after printing an error. */
static int run_words(WpEditor *ed, int argc, char **argv)
{
  int i = 0;
  while (i < argc) {
    if (!strcmp(argv[i], "--")) { i++; continue; }
    const WpCommand *cmd = wp_command_find(argv[i]);
    if (!cmd) { fprintf(stderr, "gemwrite-cli: unknown command '%s' (try --list)\n", argv[i]); return -1; }
    int arity = wp_command_arity(cmd);
    if (i + 1 + arity > argc) {
      fprintf(stderr, "gemwrite-cli: %s needs %d argument%s: %s %s\n", cmd->name, arity, arity == 1 ? "" : "s", cmd->name, cmd->usage ? cmd->usage : "");
      return -1;
    }
    char *err = NULL;
    GVariant *arg = NULL, *out = NULL;
    if (!wp_command_parse_args(cmd, argv + i + 1, &arg, &err)) {
      fprintf(stderr, "gemwrite-cli: %s: %s\n", cmd->name, err);
      free(err);
      return -1;
    }
    if (arg) g_variant_ref_sink(arg);
    bool ok = wp_command_run(ed, cmd->name, arg, &out, &err);
    if (arg) g_variant_unref(arg);
    if (!ok) {
      fprintf(stderr, "gemwrite-cli: %s: %s\n", cmd->name, err ? err : "failed");
      free(err);
      return -1;
    }
    if (out) {
      g_variant_ref_sink(out);
      json_value(out);
      putchar('\n');
      g_variant_unref(out);
    }
    i += 1 + arity;
  }
  return i;
}

static int run_script(WpEditor *ed, const char *path)
{
  FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "r");
  if (!f) { fprintf(stderr, "gemwrite-cli: cannot open script %s\n", path); return 1; }
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  int lineno = 0, status = 0;
  while ((n = getline(&line, &cap, f)) >= 0) {
    lineno++;
    char *s = g_strstrip(line);
    if (!*s || *s == '#') continue;
    int argc = 0;
    char **argv = NULL;
    g_autoptr(GError) gerr = NULL;
    if (!g_shell_parse_argv(s, &argc, &argv, &gerr)) {
      fprintf(stderr, "gemwrite-cli: line %d: %s\n", lineno, gerr->message);
      status = 1;
      break;
    }
    int used = run_words(ed, argc, argv);
    g_strfreev(argv);
    if (used < 0) { fprintf(stderr, "gemwrite-cli: stopped at line %d\n", lineno); status = 1; break; }
    fflush(stdout);
  }
  free(line);
  if (f != stdin) fclose(f);
  return status;
}

static void list_commands(void)
{
  size_t n;
  const WpCommand *cmds = wp_commands(&n);
  const char *group = NULL;
  for (size_t i = 0; i < n; i++) {
    if (!group || strcmp(group, cmds[i].group)) {
      group = cmds[i].group;
      printf("%s%s\n", i ? "\n" : "", group);
    }
    char *left = g_strdup_printf("%s %s", cmds[i].name, cmds[i].usage ? cmds[i].usage : "");
    printf("  %-34s %s\n", left, cmds[i].summary);
    g_free(left);
  }
  puts("\nPositions: start, end, caret, anchor, P (paragraph) or P:C (paragraph:character), 0-based.");
  puts("Units for move/extend: char, word, line, line-edge, page, doc; COUNT < 0 moves backwards.");
}

static void usage(FILE *to)
{
  fputs("Usage: gemwrite-cli [FILE] COMMAND [ARGS]... [COMMAND [ARGS]...]...\n"
        "       gemwrite-cli [FILE] --script SCRIPT   (one command per line, - for stdin)\n"
        "       gemwrite-cli --list                   (show every command)\n"
        "Options: --author NAME   sign comments as NAME instead of the settings file's author\n"
        "\n"
        "Loads FILE (.odt or .txt), runs the commands in order, and exits. Nothing is\n"
        "written unless a save command runs. Query results print as JSON.\n"
        "\n"
        "Examples:\n"
        "  gemwrite-cli report.odt info\n"
        "  gemwrite-cli report.odt paragraphs\n"
        "  gemwrite-cli report.odt find lynx insert-text bobcat save\n"
        "  gemwrite-cli report.odt goto end new-paragraph insert-text \"The End\" align center save\n"
        "  gemwrite-cli report.odt select 0 0:7 bold save\n"
        "  gemwrite-cli report.odt find \"very unique\" comment-add \"Drop 'very'.\" save   (comment on just those words)\n"
        "  gemwrite-cli report.odt render page0.png 0\n"
        "  gemwrite-cli style-h1 insert-text Notes save-as notes.odt\n"
        "  gemwrite-cli report.odt --script edits.txt\n"
        "\n"
        "See gemwrite-cli(1) for every command with its arguments and more examples.\n", to);
}

int main(int argc, char **argv)
{
  const char *file = NULL, *script = NULL, *author = NULL;
  int i = 1;
  for (; i < argc; i++) {
    if (!strcmp(argv[i], "--list")) { list_commands(); return 0; }
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
    if (!strcmp(argv[i], "--script")) {
      if (i + 1 >= argc) { usage(stderr); return 2; }
      script = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--author")) {
      if (i + 1 >= argc) { usage(stderr); return 2; }
      author = argv[++i];
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2]) { fprintf(stderr, "gemwrite-cli: unknown option %s\n", argv[i]); return 2; }
    /* The first word that is not a command names the file. */
    if (!file && !wp_command_find(argv[i])) { file = argv[i]; continue; }
    break;
  }
  if (!script && i >= argc) { usage(stderr); return 2; }

  WpEditor *ed = wp_editor_new(wp_layout_pango_new());
  if (author) wp_editor_set_author(ed, author);   /* who comment-add and comment-reply sign as */
  int status = 0;
  if (file) {
    char *err = NULL;
    if (!wp_editor_load(ed, file, &err)) {
      fprintf(stderr, "gemwrite-cli: %s: %s\n", file, err ? err : "could not open");
      free(err);
      wp_editor_free(ed);
      return 1;
    }
  }
  if (i < argc && run_words(ed, argc - i, argv + i) < 0) status = 1;
  if (!status && script) status = run_script(ed, script);
  wp_editor_free(ed);
  return status;
}
