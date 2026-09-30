/* editor.c — see editor.h. GLib is used for file I/O and Unicode
 * classification only; no GTK. */
#include "edit/editor.h"

#include <glib.h>
#include <math.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "io/odt.h"

/* One undo step: the other version of a paragraph range. Undo and redo are
 * the same swap, so an entry moves between the two stacks unchanged apart
 * from which state is in the document and which is stored. */
typedef enum { COALESCE_NONE, COALESCE_TYPING, COALESCE_BACKSPACE, COALESCE_DELETE } CoalesceKind;

typedef struct WpUndoEntry {
  size_t       first;      /* first paragraph of the range */
  WpParagraph *paras;      /* stored version of the range */
  size_t       n_stored;   /* paragraphs in `paras` */
  size_t       n_live;     /* paragraphs the range spans in the document now */
  WpComment   *comments;   /* stored version of the comment table */
  size_t       ncomments;
  WpPos        caret_before, anchor_before, caret_after, anchor_after;
  CoalesceKind kind;
} WpUndoEntry;

typedef struct WpUndoStack {
  WpUndoEntry *items;
  size_t n, cap;
} WpUndoStack;

#define UNDO_LIMIT 500

typedef struct SpanEntry { uint64_t hash; size_t len; bool valid; WpSpan *spans; size_t n, cap; } SpanEntry;
typedef struct SpanCache { SpanEntry *items; size_t n; } SpanCache;   /* one entry per paragraph */

struct WpEditor {
  WpDocument     *doc;
  WpLayoutEngine *engine;

  WpUndoStack undo, redo;
  size_t      save_depth;  /* undo.n when the document last matched the file; SIZE_MAX = unreachable */

  /* A change in progress: the range snapshotted by begin_change. */
  struct {
    bool         active;
    size_t       first, count, nparas_before;
    WpParagraph *paras;
    WpComment   *comments;
    size_t       ncomments;
    WpPos        caret, anchor;
  } pending;

  WpPos  caret, anchor;
  double x_goal;             /* < 0 when unset; page x for repeated Up/Down */

  WpTextAttrs typing;        /* attributes for the next insertion when typing_override */
  bool        typing_override;

  char *path;
  bool  modified;
  char *author;
  /* The file as last read or written, to tell our own writes from others'. */
  struct { bool valid; gint64 sec, nsec, size; } stamp;

  WpEditorListener listener;

  /* Spans found per paragraph (misspellings, search matches): an entry is
   * valid while the paragraph's text still has the length and hash it was
   * scanned with, so the caches never go stale and need no invalidation
   * from the editing paths. */
  WpSpell    *spell;
  SpanCache   spell_cache;

  char       *search;         /* the search term, NULL for none */
  char       *search_folded;  /* the term lower-cased, for case-insensitive matching */
  unsigned    search_flags;
  SpanCache   search_cache;
};

static void caret_changed(WpEditor *ed);

/* ---- span caches --------------------------------------------------------- */

static void span_cache_clear(SpanCache *c)
{
  for (size_t i = 0; i < c->n; i++) free(c->items[i].spans);
  free(c->items);
  c->items = NULL;
  c->n = 0;
}

static void span_cache_invalidate(SpanCache *c)
{
  for (size_t i = 0; i < c->n; i++) c->items[i].valid = false;
}

static uint64_t fnv1a(const char *s, size_t len)
{
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 0x100000001b3ull; }
  return h;
}

/* The entry for a paragraph. *fresh is true when it still describes the
 * paragraph's current text; otherwise it has been emptied and marked as
 * describing the current text, and the caller fills it. */
static SpanEntry *span_cache_entry(SpanCache *c, const WpDocument *doc, size_t para, bool *fresh)
{
  size_t n = wp_document_para_count(doc);
  if (c->n != n) {
    for (size_t i = n; i < c->n; i++) free(c->items[i].spans);
    c->items = realloc(c->items, (n ? n : 1) * sizeof *c->items);
    if (n > c->n) memset(c->items + c->n, 0, (n - c->n) * sizeof *c->items);
    c->n = n;
  }
  SpanEntry *e = &c->items[para];
  const WpParagraph *p = wp_document_para(doc, para);
  uint64_t h = fnv1a(p->text, p->len);
  *fresh = e->valid && e->len == p->len && e->hash == h;
  if (!*fresh) { e->n = 0; e->valid = true; e->len = p->len; e->hash = h; }
  return e;
}

static void span_entry_add(SpanEntry *e, size_t start, size_t end)
{
  if (e->n == e->cap) {
    e->cap = e->cap ? e->cap * 2 : 8;
    e->spans = realloc(e->spans, e->cap * sizeof *e->spans);
  }
  e->spans[e->n++] = (WpSpan){ start, end };
}

/* ---- notifications ------------------------------------------------------- */

static void notify_state(WpEditor *ed)
{
  if (ed->listener.state_changed) ed->listener.state_changed(ed, ed->listener.user);
}

static void on_doc_changed(WpDocument *doc, size_t first_para, void *user)
{
  WpEditor *ed = user;
  wp_layout_invalidate(ed->engine, first_para);
  ed->modified = true;
  if (ed->listener.document_changed) ed->listener.document_changed(ed, first_para, ed->listener.user);
}

/* ---- undo ---------------------------------------------------------------- */

static void entry_free(WpUndoEntry *e)
{
  wp_paragraphs_free(e->paras, e->n_stored);
  wp_comments_free(e->comments, e->ncomments);
}

static void stack_clear(WpUndoStack *st)
{
  for (size_t i = 0; i < st->n; i++) entry_free(&st->items[i]);
  st->n = 0;
}

static void stack_push(WpUndoStack *st, WpUndoEntry e)
{
  if (st->n == st->cap) {
    st->cap = st->cap ? st->cap * 2 : 16;
    st->items = realloc(st->items, st->cap * sizeof *st->items);
  }
  st->items[st->n++] = e;
}

static void history_clear(WpEditor *ed)
{
  stack_clear(&ed->undo);
  stack_clear(&ed->redo);
  ed->save_depth = 0;
}

/* Snapshot paragraphs [p0, p1] before an edit touches them. */
static void begin_change(WpEditor *ed, size_t p0, size_t p1)
{
  if (p0 > p1) { size_t t = p0; p0 = p1; p1 = t; }
  ed->pending.active = true;
  ed->pending.first = p0;
  ed->pending.count = p1 - p0 + 1;
  ed->pending.nparas_before = wp_document_para_count(ed->doc);
  ed->pending.paras = wp_document_copy_paras(ed->doc, p0, ed->pending.count);
  ed->pending.comments = wp_document_copy_comments(ed->doc, &ed->pending.ncomments);
  ed->pending.caret = ed->caret;
  ed->pending.anchor = ed->anchor;
}

/* Turn the snapshot into an undo entry, or fold it into the previous one
 * when this is more of the same typing. */
static void end_change(WpEditor *ed, CoalesceKind kind)
{
  if (!ed->pending.active) return;
  ed->pending.active = false;
  size_t nparas = wp_document_para_count(ed->doc);
  size_t n_live = ed->pending.count + nparas - ed->pending.nparas_before;

  /* Editing after an undo discards the redo branch; if the save point was
   * on that branch it can no longer be reached. */
  if (ed->redo.n) {
    stack_clear(&ed->redo);
    if (ed->save_depth > ed->undo.n) ed->save_depth = SIZE_MAX;
  }

  WpUndoEntry *top = ed->undo.n ? &ed->undo.items[ed->undo.n - 1] : NULL;
  if (kind != COALESCE_NONE && top && top->kind == kind &&
      wp_pos_eq(top->caret_after, ed->pending.caret) && wp_pos_eq(top->anchor_after, ed->pending.anchor) &&
      ed->pending.first == top->first && ed->pending.count == top->n_live &&
      ed->undo.n != ed->save_depth) {
    wp_paragraphs_free(ed->pending.paras, ed->pending.count);
    wp_comments_free(ed->pending.comments, ed->pending.ncomments);
    top->n_live = n_live;
    top->caret_after = ed->caret;
    top->anchor_after = ed->anchor;
    notify_state(ed);
    return;
  }

  WpUndoEntry e = {
    .first = ed->pending.first, .paras = ed->pending.paras, .n_stored = ed->pending.count, .n_live = n_live,
    .comments = ed->pending.comments, .ncomments = ed->pending.ncomments,
    .caret_before = ed->pending.caret, .anchor_before = ed->pending.anchor,
    .caret_after = ed->caret, .anchor_after = ed->anchor, .kind = kind,
  };
  if (ed->undo.n == UNDO_LIMIT) {
    entry_free(&ed->undo.items[0]);
    memmove(ed->undo.items, ed->undo.items + 1, (ed->undo.n - 1) * sizeof *ed->undo.items);
    ed->undo.n--;
    if (ed->save_depth != SIZE_MAX) ed->save_depth = ed->save_depth ? ed->save_depth - 1 : SIZE_MAX;
  }
  stack_push(&ed->undo, e);
  /* Listeners heard about the edit itself before the entry existed; tell
   * them again so undo/redo availability is current. */
  notify_state(ed);
}

/* Swap the stored range with the live one and move the entry across. */
static void swap_entry(WpEditor *ed, WpUndoStack *from, WpUndoStack *to, bool undoing)
{
  WpUndoEntry e = from->items[--from->n];
  WpParagraph *live = wp_document_copy_paras(ed->doc, e.first, e.n_live);
  size_t nlive_comments;
  WpComment *live_comments = wp_document_copy_comments(ed->doc, &nlive_comments);
  wp_document_set_comments(ed->doc, e.comments, e.ncomments);
  wp_document_replace_paras(ed->doc, e.first, e.n_live, e.paras, e.n_stored);
  wp_paragraphs_free(e.paras, e.n_stored);
  wp_comments_free(e.comments, e.ncomments);
  e.paras = live;
  e.comments = live_comments;
  e.ncomments = nlive_comments;
  size_t t = e.n_stored; e.n_stored = e.n_live; e.n_live = t;
  stack_push(to, e);

  ed->caret = undoing ? e.caret_before : e.caret_after;
  ed->anchor = undoing ? e.anchor_before : e.anchor_after;
  ed->x_goal = -1;
  ed->typing_override = false;
  ed->modified = ed->undo.n != ed->save_depth;
  caret_changed(ed);
}

bool wp_editor_can_undo(WpEditor *ed) { return ed->undo.n > 0; }
bool wp_editor_can_redo(WpEditor *ed) { return ed->redo.n > 0; }

bool wp_editor_undo(WpEditor *ed)
{
  if (!ed->undo.n) return false;
  swap_entry(ed, &ed->undo, &ed->redo, true);
  return true;
}

bool wp_editor_redo(WpEditor *ed)
{
  if (!ed->redo.n) return false;
  swap_entry(ed, &ed->redo, &ed->undo, false);
  return true;
}

/* ---- lifecycle ----------------------------------------------------------- */

WpEditor *wp_editor_new(WpLayoutEngine *engine)
{
  WpEditor *ed = calloc(1, sizeof *ed);
  ed->doc = wp_document_new();
  ed->engine = engine;
  ed->x_goal = -1;
  ed->doc->on_change = on_doc_changed;
  ed->doc->on_change_user = ed;
  wp_layout_set_document(engine, ed->doc);
  return ed;
}

void wp_editor_free(WpEditor *ed)
{
  if (!ed) return;
  history_clear(ed);
  free(ed->undo.items);
  free(ed->redo.items);
  if (ed->pending.active) {
    wp_paragraphs_free(ed->pending.paras, ed->pending.count);
    wp_comments_free(ed->pending.comments, ed->pending.ncomments);
  }
  wp_layout_destroy(ed->engine);
  wp_document_free(ed->doc);
  free(ed->path);
  free(ed->author);
  wp_spell_free(ed->spell);   /* not set_spell: no listener should hear from a dying editor */
  span_cache_clear(&ed->spell_cache);
  span_cache_clear(&ed->search_cache);
  free(ed->search);
  free(ed->search_folded);
  free(ed);
}

void wp_editor_set_listener(WpEditor *ed, const WpEditorListener *l)
{
  ed->listener = l ? *l : (WpEditorListener){ 0 };
}

WpDocument     *wp_editor_document(WpEditor *ed) { return ed->doc; }
WpLayoutEngine *wp_editor_engine(WpEditor *ed)   { return ed->engine; }

/* ---- caret --------------------------------------------------------------- */

static void caret_changed(WpEditor *ed)
{
  ed->caret = wp_document_clamp(ed->doc, ed->caret);
  ed->anchor = wp_document_clamp(ed->doc, ed->anchor);
  notify_state(ed);
}

static void set_caret(WpEditor *ed, WpPos pos, bool extend, bool keep_goal)
{
  ed->caret = pos;
  if (!extend) ed->anchor = pos;
  if (!keep_goal) ed->x_goal = -1;
  ed->typing_override = false;
  caret_changed(ed);
}

WpPos wp_editor_caret(WpEditor *ed)  { return ed->caret; }
WpPos wp_editor_anchor(WpEditor *ed) { return ed->anchor; }
bool  wp_editor_has_selection(WpEditor *ed) { return !wp_pos_eq(ed->caret, ed->anchor); }

void wp_editor_get_selection(WpEditor *ed, WpPos *a, WpPos *b)
{
  *a = wp_pos_min(ed->caret, ed->anchor);
  *b = wp_pos_max(ed->caret, ed->anchor);
}

void wp_editor_set_caret(WpEditor *ed, WpPos pos, bool extend) { set_caret(ed, pos, extend, false); }

void wp_editor_select(WpEditor *ed, WpPos anchor, WpPos caret)
{
  ed->anchor = anchor;
  set_caret(ed, caret, true, false);
}

void wp_editor_select_all(WpEditor *ed)
{
  wp_editor_select(ed, (WpPos){ 0, 0 }, wp_document_end(ed->doc));
}

bool wp_editor_move(WpEditor *ed, WpMoveUnit unit, int dir, bool extend)
{
  WpPos p = ed->caret, s, e;
  switch (unit) {
  case WP_MOVE_CHAR:
    if (!extend && wp_editor_has_selection(ed)) {
      set_caret(ed, dir < 0 ? wp_pos_min(ed->caret, ed->anchor) : wp_pos_max(ed->caret, ed->anchor), false, false);
      return true;
    }
    if (!(dir < 0 ? wp_document_pos_backward(ed->doc, &p) : wp_document_pos_forward(ed->doc, &p))) return false;
    set_caret(ed, p, extend, false);
    return true;
  case WP_MOVE_WORD:
    if (dir < 0) {
      if (!wp_document_pos_backward(ed->doc, &p)) return false;
      wp_document_word_bounds(ed->doc, p, &s, &e);
      p = s;
    } else {
      if (!wp_document_pos_forward(ed->doc, &p)) return false;
      wp_document_word_bounds(ed->doc, p, &s, &e);
      p = e;
    }
    set_caret(ed, p, extend, false);
    return true;
  case WP_MOVE_LINE: {
    WpPos out;
    if (wp_layout_move_vertical(ed->engine, ed->caret, dir, &ed->x_goal, &out)) {
      set_caret(ed, out, extend, true);
      return true;
    }
    /* No line in that direction: go to the document edge, like GtkTextView. */
    WpPos edge = dir < 0 ? (WpPos){ 0, 0 } : wp_document_end(ed->doc);
    if (wp_pos_eq(edge, ed->caret)) return false;
    set_caret(ed, edge, extend, false);
    return true;
  }
  case WP_MOVE_LINE_EDGE:
    if (!wp_layout_line_bounds(ed->engine, ed->caret, &s, &e)) return false;
    set_caret(ed, dir < 0 ? s : e, extend, false);
    return true;
  case WP_MOVE_PAGE: {
    /* Walk lines until roughly one page of text height has been covered. */
    WpPos cur = ed->caret, out;
    size_t page0; WpRect r0;
    if (!wp_layout_caret_rect(ed->engine, cur, &page0, &r0)) return false;
    double travelled = 0;
    double limit = ed->doc->page.height - ed->doc->page.margin_top - ed->doc->page.margin_bottom;
    while (travelled < limit && wp_layout_move_vertical(ed->engine, cur, dir, &ed->x_goal, &out)) {
      size_t pg; WpRect r;
      cur = out;
      if (wp_layout_caret_rect(ed->engine, cur, &pg, &r)) travelled += r.h;
    }
    if (wp_pos_eq(cur, ed->caret)) return false;
    set_caret(ed, cur, extend, true);
    return true;
  }
  case WP_MOVE_DOC:
    set_caret(ed, dir < 0 ? (WpPos){ 0, 0 } : wp_document_end(ed->doc), extend, false);
    return true;
  }
  return false;
}

bool wp_editor_find(WpEditor *ed, const char *needle)
{
  if (!needle || !*needle) return false;
  wp_editor_set_search(ed, needle, ed->search_flags);
  return wp_editor_find_next(ed, 1);
}

/* ---- editing ------------------------------------------------------------- */

void wp_editor_delete_selection(WpEditor *ed)
{
  if (!wp_editor_has_selection(ed)) return;
  WpPos a, b;
  wp_editor_get_selection(ed, &a, &b);
  begin_change(ed, a.para, b.para);
  wp_document_delete_range(ed->doc, a, b);
  set_caret(ed, a, false, false);
  end_change(ed, COALESCE_NONE);
}

static void list_step_out(WpEditor *ed);

void wp_editor_insert_text(WpEditor *ed, const char *text, long len)
{
  if (len < 0) len = (long)strlen(text);
  bool had_sel = wp_editor_has_selection(ed);
  if (!had_sel && len == 1 && text[0] == '\n' &&
      ed->doc->paras[ed->caret.para].len == 0 && ed->doc->paras[ed->caret.para].style.list_kind != WP_LIST_NONE) {
    list_step_out(ed);
    return;
  }
  WpPos a = ed->caret, b = ed->caret;
  if (had_sel) wp_editor_get_selection(ed, &a, &b);
  begin_change(ed, a.para, b.para);
  if (had_sel) {
    wp_document_delete_range(ed->doc, a, b);
    ed->caret = ed->anchor = a;
  }
  const WpTextAttrs *attrs = ed->typing_override ? &ed->typing : NULL;
  WpPos end = wp_document_insert_text(ed->doc, ed->caret, text, (size_t)len, attrs);
  ed->caret = ed->anchor = end;
  ed->x_goal = -1;
  ed->typing_override = false;
  caret_changed(ed);
  /* Plain typing merges into one step; a paragraph break or a replacement
   * of selected text stands on its own. */
  bool plain = !had_sel && !memchr(text, '\n', (size_t)len);
  end_change(ed, plain ? COALESCE_TYPING : COALESCE_NONE);
}

void wp_editor_delete(WpEditor *ed, int dir)
{
  if (wp_editor_has_selection(ed)) { wp_editor_delete_selection(ed); return; }
  if (dir < 0 && ed->caret.offset == 0 && ed->doc->paras[ed->caret.para].style.list_kind != WP_LIST_NONE) {
    list_step_out(ed);
    return;
  }
  WpPos p = ed->caret;
  bool ok = dir < 0 ? wp_document_pos_backward(ed->doc, &p) : wp_document_pos_forward(ed->doc, &p);
  if (!ok) return;
  WpPos a = wp_pos_min(p, ed->caret), b = wp_pos_max(p, ed->caret);
  begin_change(ed, a.para, b.para);
  wp_document_delete_range(ed->doc, a, b);
  set_caret(ed, a, false, false);
  /* Deleting a paragraph break is its own step, like inserting one. */
  bool same_para = a.para == b.para;
  end_change(ed, !same_para ? COALESCE_NONE : dir < 0 ? COALESCE_BACKSPACE : COALESCE_DELETE);
}

/* ---- clipboard ----------------------------------------------------------- */

/* Process-wide; see editor.h. Owned here so the CLI and every window of the
 * app share the one fragment. */
static struct {
  WpParagraph *paras;
  size_t       n;
  char        *text;   /* the fragment as plain text, paragraphs joined by '\n' */
} clipboard;

static void clipboard_set(WpParagraph *paras, size_t n)
{
  wp_paragraphs_free(clipboard.paras, clipboard.n);
  free(clipboard.text);
  clipboard.paras = paras;
  clipboard.n = n;
  size_t total = 0;
  for (size_t i = 0; i < n; i++) total += paras[i].len + 1;
  char *w = clipboard.text = malloc(total);
  for (size_t i = 0; i < n; i++) {
    memcpy(w, paras[i].text, paras[i].len);
    w += paras[i].len;
    *w++ = i + 1 < n ? '\n' : 0;
  }
}

const char *wp_clipboard_text(void) { return clipboard.n ? clipboard.text : NULL; }

void wp_clipboard_set_text(const char *text, long len)
{
  if (len < 0) len = (long)strlen(text);
  size_t n;
  WpParagraph *paras = wp_paragraphs_from_text(text, (size_t)len, &n);
  clipboard_set(paras, n);
}

bool wp_editor_copy(WpEditor *ed)
{
  if (!wp_editor_has_selection(ed)) return false;
  WpPos a, b;
  wp_editor_get_selection(ed, &a, &b);
  size_t n;
  WpParagraph *paras = wp_document_copy_range(ed->doc, a, b, &n);
  clipboard_set(paras, n);
  return true;
}

bool wp_editor_cut(WpEditor *ed)
{
  if (!wp_editor_copy(ed)) return false;
  wp_editor_delete_selection(ed);
  return true;
}

bool wp_editor_paste(WpEditor *ed)
{
  if (!clipboard.n) return false;
  bool had_sel = wp_editor_has_selection(ed);
  WpPos a = ed->caret, b = ed->caret;
  if (had_sel) wp_editor_get_selection(ed, &a, &b);
  begin_change(ed, a.para, b.para);
  if (had_sel) {
    wp_document_delete_range(ed->doc, a, b);
    ed->caret = ed->anchor = a;
  }
  WpPos end = wp_document_insert_paras(ed->doc, ed->caret, clipboard.paras, clipboard.n);
  ed->caret = ed->anchor = end;
  ed->x_goal = -1;
  ed->typing_override = false;
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return true;
}

/* ---- formatting ---------------------------------------------------------- */

static WpPos attr_probe_pos(WpEditor *ed)
{
  return wp_editor_has_selection(ed) ? wp_pos_min(ed->caret, ed->anchor) : ed->caret;
}

WpTextAttrs wp_editor_current_attrs(WpEditor *ed)
{
  if (ed->typing_override) return ed->typing;
  return wp_document_typing_attrs(ed->doc, attr_probe_pos(ed));
}

WpTextAttrs wp_editor_effective_attrs(WpEditor *ed)
{
  WpTextAttrs run = wp_editor_current_attrs(ed);
  WpTextAttrs r = wp_document_para_base_attrs(ed->doc, attr_probe_pos(ed).para);
  r.flags |= run.flags;
  if (run.size_pt > 0) r.size_pt = run.size_pt;
  if (run.family) r.family = run.family;
  return r;
}

void wp_editor_toggle_attr(WpEditor *ed, uint32_t flag)
{
  if (wp_editor_has_selection(ed)) {
    WpPos a, b;
    wp_editor_get_selection(ed, &a, &b);
    /* attrs_at looks at the character *before* a position, so probe just after a. */
    WpPos probe = a;
    wp_document_pos_forward(ed->doc, &probe);
    bool set = !(wp_document_attrs_at(ed->doc, probe).flags & flag);
    begin_change(ed, a.para, b.para);
    wp_document_set_flags(ed->doc, a, b, flag, set);
    end_change(ed, COALESCE_NONE);
  } else {
    if (!ed->typing_override) ed->typing = wp_document_typing_attrs(ed->doc, ed->caret);
    ed->typing.flags ^= flag;
    ed->typing_override = true;
  }
  notify_state(ed);
}

static void apply_attr_change(WpEditor *ed, WpAttrsFn fn, void *user)
{
  if (wp_editor_has_selection(ed)) {
    WpPos a, b;
    wp_editor_get_selection(ed, &a, &b);
    begin_change(ed, a.para, b.para);
    wp_document_apply_attrs(ed->doc, a, b, fn, user);
    end_change(ed, COALESCE_NONE);
  } else {
    if (!ed->typing_override) ed->typing = wp_document_typing_attrs(ed->doc, ed->caret);
    fn(&ed->typing, user);
    ed->typing_override = true;
  }
  notify_state(ed);
}

static void family_fn(WpTextAttrs *a, void *user) { a->family = user; }
static void size_fn(WpTextAttrs *a, void *user)   { a->size_pt = *(float *)user; }
static void align_fn(WpParaStyle *st, void *user) { st->align = *(WpAlign *)user; }
static void line_spacing_fn(WpParaStyle *st, void *user) { st->line_spacing = *(float *)user; }

void wp_editor_set_family(WpEditor *ed, const char *family)
{
  apply_attr_change(ed, family_fn, (void *)wp_intern(family));
}

void wp_editor_set_size(WpEditor *ed, float size_pt)
{
  if (size_pt <= 0) return;
  apply_attr_change(ed, size_fn, &size_pt);
}

void wp_editor_set_align(WpEditor *ed, WpAlign align)
{
  begin_change(ed, ed->anchor.para, ed->caret.para);
  wp_document_apply_para_style(ed->doc, ed->anchor, ed->caret, align_fn, &align);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

WpAlign wp_editor_current_align(WpEditor *ed)
{
  return wp_document_para(ed->doc, ed->caret.para)->style.align;
}

void wp_editor_set_line_spacing(WpEditor *ed, float factor)
{
  if (factor <= 0) return;
  begin_change(ed, ed->anchor.para, ed->caret.para);
  wp_document_apply_para_style(ed->doc, ed->anchor, ed->caret, line_spacing_fn, &factor);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

float wp_editor_current_line_spacing(WpEditor *ed)
{
  float f = wp_document_para(ed->doc, ed->caret.para)->style.line_spacing;
  return f > 0 ? f : 1.0f;
}

void wp_editor_set_style_name(WpEditor *ed, const char *name)
{
  begin_change(ed, ed->anchor.para, ed->caret.para);
  wp_document_set_style_name(ed->doc, ed->anchor, ed->caret, name);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

const char *wp_editor_current_style_name(WpEditor *ed)
{
  return wp_document_style_of(ed->doc, ed->caret.para)->name;
}

/* ---- lists --------------------------------------------------------------- */

/* Make paragraph pi an item of `kind` at `level` (kind NONE: body text),
 * with the indent that level normally has. Adding or removing a list is
 * formatting, not a change of style: the paragraph keeps its alignment,
 * line spacing, and space before and after. */
static void set_para_list(WpDocument *doc, size_t pi, WpListKind kind, int level)
{
  WpPos at = { pi, 0 };
  WpParaStyle was = doc->paras[pi].style;
  if (level < 0) level = 0;
  if (level > WP_LIST_MAX_LEVEL) level = WP_LIST_MAX_LEVEL;
  wp_document_set_style_name(doc, at, at, kind == WP_LIST_NONE ? WP_STYLE_STANDARD :
                             kind == WP_LIST_BULLET ? WP_STYLE_LIST_BULLET : WP_STYLE_LIST_NUMBER);
  WpParaStyle *st = &doc->paras[pi].style;
  st->align = was.align;
  st->line_spacing = was.line_spacing;
  st->space_before_pt = was.space_before_pt;
  st->space_after_pt = was.space_after_pt;
  if (kind != WP_LIST_NONE) {
    st->list_kind = kind;
    st->list_level = level;
    st->indent_left_pt = wp_list_indent(level);
  }
  wp_document_changed(doc, pi);
}

void wp_editor_set_list(WpEditor *ed, WpListKind kind)
{
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  bool all = kind != WP_LIST_NONE;
  for (size_t pi = p0; pi <= p1 && all; pi++) all = ed->doc->paras[pi].style.list_kind == kind;
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) {
    const WpParaStyle *st = &ed->doc->paras[pi].style;
    if (all) set_para_list(ed->doc, pi, WP_LIST_NONE, 0);
    else set_para_list(ed->doc, pi, kind, st->list_kind != WP_LIST_NONE ? st->list_level : 0);
  }
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

/* One level out for paragraph pi: shallower, or body text from the top. */
static void outdent_para(WpDocument *doc, size_t pi)
{
  const WpParaStyle *st = &doc->paras[pi].style;
  if (st->list_level > 0) set_para_list(doc, pi, st->list_kind, st->list_level - 1);
  else set_para_list(doc, pi, WP_LIST_NONE, 0);
}

bool wp_editor_indent(WpEditor *ed, int dir)
{
  if (dir == 0) return false;
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  const WpPageSetup *pg = &ed->doc->page;
  bool moved = false;
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) {
    WpParaStyle *st = &ed->doc->paras[pi].style;
    if (st->list_kind != WP_LIST_NONE) {
      if (dir > 0 && st->list_level >= WP_LIST_MAX_LEVEL) continue;
      if (dir > 0) set_para_list(ed->doc, pi, st->list_kind, st->list_level + 1);
      else outdent_para(ed->doc, pi);
      moved = true;
      continue;
    }
    /* an ordinary paragraph: step its left edge, kept within the margins */
    float most = (float)(pg->width - pg->margin_left - pg->margin_right) - st->indent_right_pt - WP_INDENT_MIN_TEXT_PT;
    float want = st->indent_left_pt + (dir > 0 ? WP_INDENT_STEP_PT : -WP_INDENT_STEP_PT);
    if (want < 0) want = 0;
    if (want > most) want = most;
    if (want == st->indent_left_pt) continue;
    st->indent_left_pt = want;
    wp_document_changed(ed->doc, pi);
    moved = true;
  }
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return moved;
}

/* The widest the left and right indents together may be. */
static float indent_room(WpEditor *ed)
{
  const WpPageSetup *pg = &ed->doc->page;
  return (float)(pg->width - pg->margin_left - pg->margin_right) - WP_INDENT_MIN_TEXT_PT;
}

void wp_editor_set_indent(WpEditor *ed, WpIndent which, float pt)
{
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  float room = indent_room(ed);
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) {
    WpParaStyle *st = &ed->doc->paras[pi].style;
    float v = pt;
    switch (which) {
    case WP_INDENT_LEFT:
      v = CLAMP(v, 0, room - st->indent_right_pt);
      if (st->list_kind != WP_LIST_NONE && v < WP_LIST_HANG_PT) v = WP_LIST_HANG_PT;
      st->indent_left_pt = v;
      break;
    case WP_INDENT_RIGHT:
      st->indent_right_pt = CLAMP(v, 0, room - st->indent_left_pt);
      break;
    case WP_INDENT_FIRST:
      if (st->list_kind != WP_LIST_NONE) continue;
      v = CLAMP(v, -st->indent_left_pt, room - st->indent_left_pt - st->indent_right_pt);
      st->indent_first_pt = v == 0 ? 0 : v;   /* clamping to -0 would print as -0 */
      break;
    }
  }
  wp_document_changed(ed->doc, p0);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

void wp_editor_add_tab(WpEditor *ed, float pos_pt, WpTabKind kind)
{
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) wp_para_style_add_tab(&ed->doc->paras[pi].style, pos_pt, kind);
  wp_document_changed(ed->doc, p0);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

bool wp_editor_remove_tab(WpEditor *ed, float pos_pt)
{
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  bool any = false;
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) any |= wp_para_style_remove_tab(&ed->doc->paras[pi].style, pos_pt);
  wp_document_changed(ed->doc, p0);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return any;
}

void wp_editor_move_tab(WpEditor *ed, float from_pt, float to_pt)
{
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) {
    WpParaStyle *st = &ed->doc->paras[pi].style;
    WpTabKind kind = WP_TAB_LEFT;
    for (int i = 0; i < st->ntabs; i++) if (fabsf(st->tabs[i].pos_pt - from_pt) < 0.5f) kind = st->tabs[i].kind;
    if (wp_para_style_remove_tab(st, from_pt)) wp_para_style_add_tab(st, to_pt, kind);
  }
  wp_document_changed(ed->doc, p0);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

void wp_editor_clear_tabs(WpEditor *ed)
{
  size_t p0 = MIN(ed->anchor.para, ed->caret.para), p1 = MAX(ed->anchor.para, ed->caret.para);
  begin_change(ed, p0, p1);
  for (size_t pi = p0; pi <= p1; pi++) {
    ed->doc->paras[pi].style.ntabs = 0;
    memset(ed->doc->paras[pi].style.tabs, 0, sizeof ed->doc->paras[pi].style.tabs);
  }
  wp_document_changed(ed->doc, p0);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

WpListKind wp_editor_current_list_kind(WpEditor *ed)
{
  return wp_document_para(ed->doc, ed->caret.para)->style.list_kind;
}

int wp_editor_current_list_level(WpEditor *ed)
{
  const WpParaStyle *st = &wp_document_para(ed->doc, ed->caret.para)->style;
  return st->list_kind == WP_LIST_NONE ? 0 : st->list_level;
}

/* Enter on an empty item, or Backspace at the start of one, steps it out
 * of the list instead: one level shallower, or to body text from the top. */
static void list_step_out(WpEditor *ed)
{
  size_t pi = ed->caret.para;
  begin_change(ed, pi, pi);
  outdent_para(ed->doc, pi);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
}

/* ---- comments ------------------------------------------------------------ */

void wp_editor_set_author(WpEditor *ed, const char *author)
{
  char *dup = author && *author ? strdup(author) : NULL;
  free(ed->author);
  ed->author = dup;
}

const char *wp_editor_author(WpEditor *ed) { return ed->author; }

static char *now_iso8601(void)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  return g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");
}

uint32_t wp_editor_add_comment(WpEditor *ed, const char *author, const char *date, const char *text)
{
  if (!wp_editor_has_selection(ed)) return 0;
  WpPos a, b;
  wp_editor_get_selection(ed, &a, &b);
  g_autofree char *now = date ? NULL : now_iso8601();
  begin_change(ed, a.para, b.para);
  uint32_t id = wp_document_add_comment(ed->doc, a, b, author ? author : ed->author, date ? date : now, text);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return id;
}

uint32_t wp_editor_add_reply(WpEditor *ed, uint32_t id, const char *author, const char *date, const char *text)
{
  uint32_t root = wp_document_comment_root(ed->doc, id);
  if (!root) return 0;
  WpPos a, b;
  size_t p0 = ed->caret.para, p1 = ed->caret.para;
  if (wp_document_comment_range(ed->doc, root, &a, &b)) { p0 = a.para; p1 = b.para; }
  g_autofree char *now = date ? NULL : now_iso8601();
  begin_change(ed, p0, p1);
  uint32_t rid = wp_document_add_reply(ed->doc, root, author ? author : ed->author, date ? date : now, text);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return rid;
}

/* The paragraphs a comment's undo entry snapshots: its text, or just the
 * caret's paragraph for an orphan (nothing in the text changes then). */
static void comment_paras(WpEditor *ed, uint32_t id, size_t *p0, size_t *p1)
{
  WpPos a, b;
  if (wp_document_comment_range(ed->doc, id, &a, &b)) { *p0 = a.para; *p1 = b.para; }
  else *p0 = *p1 = ed->caret.para;
}

bool wp_editor_set_comment_text(WpEditor *ed, uint32_t id, const char *text)
{
  if (!wp_document_comment(ed->doc, id)) return false;
  size_t p0, p1;
  comment_paras(ed, id, &p0, &p1);
  begin_change(ed, p0, p1);
  wp_document_set_comment_text(ed->doc, id, text);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return true;
}

bool wp_editor_remove_comment(WpEditor *ed, uint32_t id)
{
  if (!wp_document_comment(ed->doc, id)) return false;
  size_t p0, p1;
  comment_paras(ed, id, &p0, &p1);
  begin_change(ed, p0, p1);
  wp_document_remove_comment(ed->doc, id);
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return true;
}

bool wp_editor_select_comment(WpEditor *ed, uint32_t id)
{
  WpPos a, b;
  if (!wp_document_comment_range(ed->doc, id, &a, &b)) return false;
  wp_editor_select(ed, a, b);
  return true;
}

uint32_t wp_editor_comment_at_caret(WpEditor *ed)
{
  return wp_document_comment_at(ed->doc, attr_probe_pos(ed));
}

/* ---- file ---------------------------------------------------------------- */

static bool is_odt(const char *path)
{
  size_t n = strlen(path);
  return n >= 4 && g_ascii_strcasecmp(path + n - 4, ".odt") == 0;
}

static void set_path(WpEditor *ed, const char *path)
{
  char *dup = path ? strdup(path) : NULL;
  free(ed->path);
  ed->path = dup;
  ed->stamp.valid = false;
}

static void take_stamp(WpEditor *ed)
{
  GStatBuf st;
  ed->stamp.valid = ed->path && g_stat(ed->path, &st) == 0;
  if (!ed->stamp.valid) return;
  ed->stamp.sec = st.st_mtim.tv_sec;
  ed->stamp.nsec = st.st_mtim.tv_nsec;
  ed->stamp.size = st.st_size;
}

bool wp_editor_file_changed_on_disk(WpEditor *ed)
{
  GStatBuf st;
  if (!ed->stamp.valid || !ed->path || g_stat(ed->path, &st) != 0) return false;
  return st.st_mtim.tv_sec != ed->stamp.sec || st.st_mtim.tv_nsec != ed->stamp.nsec || st.st_size != ed->stamp.size;
}

/* Read path into the document; on failure the document is untouched. */
static bool read_file(WpEditor *ed, const char *path, char **err)
{
  if (is_odt(path)) return wp_odt_read(ed->doc, path, err);
  g_autoptr(GError) gerr = NULL;
  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &contents, &len, &gerr)) {
    if (err) *err = strdup(gerr->message);
    return false;
  }
  if (!g_utf8_validate(contents, (gssize)len, NULL)) {
    if (err) *err = strdup("The file is not valid UTF-8 text");
    return false;
  }
  wp_document_load_text(ed->doc, contents, len);
  return true;
}

const char *wp_editor_path(WpEditor *ed)   { return ed->path; }
bool        wp_editor_modified(WpEditor *ed) { return ed->modified; }

void wp_editor_clear(WpEditor *ed)
{
  wp_document_clear(ed->doc);
  history_clear(ed);
  set_path(ed, NULL);
  ed->modified = false;
  set_caret(ed, (WpPos){ 0, 0 }, false, false);
}

bool wp_editor_load(WpEditor *ed, const char *path, char **err)
{
  if (!read_file(ed, path, err)) return false;
  history_clear(ed);
  set_path(ed, path);
  take_stamp(ed);
  ed->modified = false;
  set_caret(ed, (WpPos){ 0, 0 }, false, false);
  return true;
}

bool wp_editor_reload(WpEditor *ed, char **err)
{
  if (!ed->path) {
    if (err) *err = strdup("The document has no file to reload from");
    return false;
  }
  begin_change(ed, 0, wp_document_para_count(ed->doc) - 1);
  if (!read_file(ed, ed->path, err)) {
    wp_paragraphs_free(ed->pending.paras, ed->pending.count);
    wp_comments_free(ed->pending.comments, ed->pending.ncomments);
    ed->pending.active = false;
    return false;
  }
  ed->x_goal = -1;
  ed->typing_override = false;
  caret_changed(ed);   /* clamps caret and anchor to the new text */
  end_change(ed, COALESCE_NONE);
  take_stamp(ed);
  ed->modified = false;
  ed->save_depth = ed->undo.n;
  notify_state(ed);
  return true;
}

bool wp_editor_save(WpEditor *ed, const char *path, char **err)
{
  if (!path) path = ed->path;
  if (!path) {
    if (err) *err = strdup("The document has no file name yet; use save-as");
    return false;
  }
  if (is_odt(path)) {
    if (!wp_odt_write(ed->doc, path, err)) return false;
  } else {
    size_t len = 0;
    char *text = wp_document_get_all_text(ed->doc, &len);
    g_autoptr(GError) gerr = NULL;
    bool ok = g_file_set_contents(path, text, (gssize)len, &gerr);
    free(text);
    if (!ok) {
      if (err) *err = strdup(gerr->message);
      return false;
    }
  }
  if (path != ed->path) set_path(ed, path);
  take_stamp(ed);
  ed->modified = false;
  ed->save_depth = ed->undo.n;
  notify_state(ed);
  return true;
}

/* ---- spelling ------------------------------------------------------------ */

void wp_editor_set_spell(WpEditor *ed, WpSpell *sp)
{
  if (sp != ed->spell) wp_spell_free(ed->spell);
  ed->spell = sp;
  span_cache_clear(&ed->spell_cache);
  notify_state(ed);
}

WpSpell *wp_editor_spell(WpEditor *ed) { return ed->spell; }

/* Paragraphs are checked whole: a change anywhere in one rechecks it, which
 * for hunspell is well under a millisecond for a screenful of words. */
const WpSpan *wp_editor_misspellings(WpEditor *ed, size_t para, size_t *n)
{
  if (!ed->spell || para >= wp_document_para_count(ed->doc)) { *n = 0; return NULL; }
  bool fresh;
  SpanEntry *e = span_cache_entry(&ed->spell_cache, ed->doc, para, &fresh);
  if (!fresh) {
    const WpParagraph *p = wp_document_para(ed->doc, para);
    size_t pos = 0, s, en;
    const char *extra = wp_spell_extra_chars(ed->spell);
    while (wp_spell_next_word(p->text, p->len, extra, &pos, &s, &en))
      if (!wp_spell_check(ed->spell, p->text + s, en - s)) span_entry_add(e, s, en);
  }
  *n = e->n;
  return e->spans;
}

bool wp_editor_misspelled_at(WpEditor *ed, WpPos pos, WpPos *a, WpPos *b)
{
  size_t n;
  const WpSpan *spans = wp_editor_misspellings(ed, pos.para, &n);
  for (size_t i = 0; i < n; i++) {
    if (pos.offset < spans[i].start) break;
    if (pos.offset <= spans[i].end) {
      *a = (WpPos){ pos.para, spans[i].start };
      *b = (WpPos){ pos.para, spans[i].end };
      return true;
    }
  }
  return false;
}

/* The word at the caret, or at the start of the selection. */
static bool spell_word_at_caret(WpEditor *ed, WpPos *a, WpPos *b)
{
  return wp_editor_misspelled_at(ed, wp_pos_min(ed->caret, ed->anchor), a, b);
}

static bool spell_word_action(WpEditor *ed, bool add)
{
  WpPos a, b;
  if (!spell_word_at_caret(ed, &a, &b)) return false;
  const WpParagraph *p = wp_document_para(ed->doc, a.para);
  if (add) wp_spell_add(ed->spell, p->text + a.offset, b.offset - a.offset);
  else     wp_spell_ignore(ed->spell, p->text + a.offset, b.offset - a.offset);
  span_cache_invalidate(&ed->spell_cache);   /* every paragraph may have carried the word */
  notify_state(ed);
  return true;
}

bool wp_editor_spell_add(WpEditor *ed)    { return spell_word_action(ed, true); }
bool wp_editor_spell_ignore(WpEditor *ed) { return spell_word_action(ed, false); }

/* ---- find and replace ---------------------------------------------------- */

/* Lower-case text one character at a time into out (room for 4 bytes per
 * input byte plus a terminator), so every output byte comes from one input
 * character; map (len * 4 + 1 entries, may be NULL) then records for each
 * output byte the offset of that character, with map[out_len] = len. This
 * is what makes case-insensitive matching report positions in the original
 * text. Returns the output length. */
static size_t fold_text(const char *text, size_t len, char *out, size_t *map)
{
  size_t o = 0;
  for (size_t i = 0; i < len; ) {
    size_t next = wp_utf8_next(text, len, i);
    gunichar c = g_utf8_get_char_validated(text + i, (gssize)(next - i));
    int w;
    if (c == (gunichar)-1 || c == (gunichar)-2) { memcpy(out + o, text + i, next - i); w = (int)(next - i); }
    else w = g_unichar_to_utf8(g_unichar_tolower(c), out + o);
    if (map) for (int k = 0; k < w; k++) map[o + k] = i;
    o += (size_t)w;
    i = next;
  }
  if (map) map[o] = len;
  out[o] = 0;
  return o;
}

static char *fold_dup(const char *s)
{
  size_t len = strlen(s);
  char *out = malloc(len * 4 + 1);
  fold_text(s, len, out, NULL);
  return out;
}

/* Fill a paragraph's entry with the non-overlapping matches of the term. */
static void scan_matches(WpEditor *ed, SpanEntry *e, const WpParagraph *p)
{
  if (ed->search_flags & WP_FIND_MATCH_CASE) {
    size_t nlen = strlen(ed->search);
    for (size_t at = 0; at + nlen <= p->len; ) {
      const char *hit = g_strstr_len(p->text + at, (gssize)(p->len - at), ed->search);
      if (!hit) break;
      size_t s = (size_t)(hit - p->text);
      span_entry_add(e, s, s + nlen);
      at = s + nlen;
    }
    return;
  }
  /* Case-insensitive: search the folded paragraph and map hits back. A hit
   * counts only when it starts and ends on character boundaries of the
   * folded text, so a term cannot match half of one character's folding. */
  char *folded = malloc(p->len * 4 + 1);
  size_t *map = malloc((p->len * 4 + 1) * sizeof *map);
  size_t flen = fold_text(p->text, p->len, folded, map);
  size_t nlen = strlen(ed->search_folded);
  for (size_t at = 0; at + nlen <= flen; ) {
    const char *hit = g_strstr_len(folded + at, (gssize)(flen - at), ed->search_folded);
    if (!hit) break;
    size_t fs = (size_t)(hit - folded), fe = fs + nlen;
    bool starts = fs == 0 || map[fs] != map[fs - 1];
    bool ends = fe == flen || map[fe] != map[fe - 1];
    if (starts && ends) { span_entry_add(e, map[fs], map[fe]); at = fe; }
    else at = fs + 1;
  }
  free(folded);
  free(map);
}

void wp_editor_set_search(WpEditor *ed, const char *needle, unsigned flags)
{
  if (needle && !*needle) needle = NULL;
  bool same_term = needle ? ed->search && !strcmp(needle, ed->search) : !ed->search;
  if (same_term && flags == ed->search_flags) return;
  free(ed->search);
  free(ed->search_folded);
  ed->search = needle ? strdup(needle) : NULL;
  ed->search_folded = needle ? fold_dup(needle) : NULL;
  ed->search_flags = flags;
  span_cache_invalidate(&ed->search_cache);
  notify_state(ed);
}

const char *wp_editor_search_term(WpEditor *ed)  { return ed->search; }
unsigned    wp_editor_search_flags(WpEditor *ed) { return ed->search_flags; }

const WpSpan *wp_editor_search_matches(WpEditor *ed, size_t para, size_t *n)
{
  if (!ed->search || para >= wp_document_para_count(ed->doc)) { *n = 0; return NULL; }
  bool fresh;
  SpanEntry *e = span_cache_entry(&ed->search_cache, ed->doc, para, &fresh);
  if (!fresh) scan_matches(ed, e, wp_document_para(ed->doc, para));
  *n = e->n;
  return e->spans;
}

/* Whether the selection is exactly one match; *index gets its position in the paragraph's list. */
static bool selection_is_match(WpEditor *ed, size_t *index)
{
  if (!ed->search || !wp_editor_has_selection(ed)) return false;
  WpPos a, b;
  wp_editor_get_selection(ed, &a, &b);
  if (a.para != b.para) return false;
  size_t n;
  const WpSpan *spans = wp_editor_search_matches(ed, a.para, &n);
  for (size_t i = 0; i < n; i++) {
    if (spans[i].start > a.offset) break;
    if (spans[i].start == a.offset && spans[i].end == b.offset) { if (index) *index = i; return true; }
  }
  return false;
}

size_t wp_editor_search_count(WpEditor *ed, size_t *current)
{
  size_t total = 0, index = 0, before = 0;
  bool selected = selection_is_match(ed, &index);
  WpPos a = wp_pos_min(ed->caret, ed->anchor);
  for (size_t i = 0; i < wp_document_para_count(ed->doc); i++) {
    size_t n;
    wp_editor_search_matches(ed, i, &n);
    if (selected && i == a.para) before = total + index;
    total += n;
  }
  if (current) *current = selected ? before + 1 : 0;
  return total;
}

/* The first match starting at or after `from` (dir > 0), or the last one
 * starting before it (dir < 0). */
static bool match_from(WpEditor *ed, WpPos from, int dir, WpPos *a, WpPos *b)
{
  size_t n = wp_document_para_count(ed->doc);
  if (dir > 0) {
    for (size_t i = from.para; i < n; i++) {
      size_t ns;
      const WpSpan *spans = wp_editor_search_matches(ed, i, &ns);
      for (size_t k = 0; k < ns; k++)
        if (i > from.para || spans[k].start >= from.offset) {
          *a = (WpPos){ i, spans[k].start }; *b = (WpPos){ i, spans[k].end };
          return true;
        }
    }
  } else {
    for (size_t i = from.para + 1; i-- > 0; ) {
      size_t ns;
      const WpSpan *spans = wp_editor_search_matches(ed, i, &ns);
      for (size_t k = ns; k-- > 0; )
        if (i < from.para || spans[k].start < from.offset) {
          *a = (WpPos){ i, spans[k].start }; *b = (WpPos){ i, spans[k].end };
          return true;
        }
    }
  }
  return false;
}

static bool find_from(WpEditor *ed, WpPos from, int dir)
{
  if (!ed->search) return false;
  WpPos a, b;
  WpPos wrap = dir > 0 ? (WpPos){ 0, 0 } : (WpPos){ wp_document_para_count(ed->doc) - 1, SIZE_MAX };
  if (!match_from(ed, from, dir, &a, &b) && !match_from(ed, wrap, dir, &a, &b)) return false;
  wp_editor_select(ed, a, b);
  return true;
}

bool wp_editor_find_next(WpEditor *ed, int dir)
{
  return find_from(ed, dir > 0 ? wp_pos_max(ed->caret, ed->anchor) : wp_pos_min(ed->caret, ed->anchor), dir);
}

bool wp_editor_find_current(WpEditor *ed)
{
  return find_from(ed, wp_pos_min(ed->caret, ed->anchor), 1);
}

/* Replace [a, b) within one paragraph; the new text takes the formatting
 * (comment included) of the range's first character. Returns the end of
 * the new text. The caller brackets the change. */
static WpPos replace_span(WpEditor *ed, WpPos a, WpPos b, const char *with, size_t wlen)
{
  WpPos after_first = a;
  wp_document_pos_forward(ed->doc, &after_first);
  WpTextAttrs attrs = wp_document_attrs_at(ed->doc, after_first);
  wp_document_delete_range(ed->doc, a, b);
  return wlen ? wp_document_insert_text(ed->doc, a, with, wlen, &attrs) : a;
}

bool wp_editor_replace(WpEditor *ed, const char *with)
{
  if (!selection_is_match(ed, NULL)) return false;
  WpPos a, b;
  wp_editor_get_selection(ed, &a, &b);
  begin_change(ed, a.para, a.para);
  WpPos end = replace_span(ed, a, b, with, strlen(with));
  ed->caret = ed->anchor = end;
  ed->x_goal = -1;
  ed->typing_override = false;
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  wp_editor_find_next(ed, 1);
  return true;
}

size_t wp_editor_replace_all(WpEditor *ed, const char *with)
{
  if (!ed->search) return 0;
  /* Collect every match first: editing reshapes the paragraphs, and the
   * cache would rescan the new text (finding the replacements, perhaps). */
  struct Hit { size_t para, start, end; } *hits = NULL;
  size_t nh = 0, cap = 0, n = wp_document_para_count(ed->doc);
  for (size_t i = 0; i < n; i++) {
    size_t ns;
    const WpSpan *spans = wp_editor_search_matches(ed, i, &ns);
    for (size_t k = 0; k < ns; k++) {
      if (nh == cap) { cap = cap ? cap * 2 : 32; hits = realloc(hits, cap * sizeof *hits); }
      hits[nh++] = (struct Hit){ i, spans[k].start, spans[k].end };
    }
  }
  if (!nh) return 0;
  begin_change(ed, 0, n - 1);
  size_t wlen = strlen(with);
  WpPos end = { 0, 0 };
  /* Back to front, so each replacement leaves the offsets of the ones
   * still to do untouched. The last done is the first in the document. */
  for (size_t k = nh; k-- > 0; )
    end = replace_span(ed, (WpPos){ hits[k].para, hits[k].start }, (WpPos){ hits[k].para, hits[k].end }, with, wlen);
  free(hits);
  ed->caret = ed->anchor = end;
  ed->x_goal = -1;
  ed->typing_override = false;
  caret_changed(ed);
  end_change(ed, COALESCE_NONE);
  return nh;
}

/* ---- queries ------------------------------------------------------------- */

/* Words are runs of non-space characters containing at least one letter or
 * digit, so stray punctuation and dashes are not counted. */
static size_t count_words(const char *text, size_t len)
{
  size_t words = 0;
  bool in_word = false, has_alnum = false;
  const char *p = text, *end = text + len;
  while (p < end) {
    gunichar c = g_utf8_get_char_validated(p, end - p);
    if (c == (gunichar)-1 || c == (gunichar)-2) break;
    if (g_unichar_isspace(c)) {
      if (in_word && has_alnum) words++;
      in_word = has_alnum = false;
    } else {
      in_word = true;
      if (g_unichar_isalnum(c)) has_alnum = true;
    }
    p = g_utf8_next_char(p);
  }
  if (in_word && has_alnum) words++;
  return words;
}

size_t wp_editor_word_count(WpEditor *ed, WpPos a, WpPos b)
{
  if (wp_pos_cmp(a, b) > 0) { WpPos t = a; a = b; b = t; }
  size_t total = 0;
  for (size_t i = a.para; i <= b.para && i < wp_document_para_count(ed->doc); i++) {
    const WpParagraph *para = wp_document_para(ed->doc, i);
    size_t s = i == a.para ? a.offset : 0;
    size_t e = i == b.para ? b.offset : para->len;
    if (s < e) total += count_words(para->text + s, e - s);
  }
  return total;
}

size_t wp_editor_page_count(WpEditor *ed) { return wp_layout_page_count(ed->engine); }
