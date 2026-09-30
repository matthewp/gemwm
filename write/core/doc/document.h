/* document.h — the word processor's document model.
 *
 * Deliberately free of any toolkit or layout-engine types so that the
 * layout engine and the UI can both be replaced independently.
 *
 * A document is an ordered list of paragraphs. A paragraph is UTF-8 text
 * plus a list of runs that partition that text into spans of identical
 * character attributes, plus a paragraph style. Positions are
 * (paragraph index, byte offset) pairs.
 *
 * Invariants:
 *   - paragraph text is NUL-terminated and never NULL.
 *   - runs cover the text exactly: sum(run.len) == para.len.
 *   - a paragraph always has >= 1 run; a zero-length run is permitted only
 *     when it is the sole run (it carries the attributes of an empty
 *     paragraph so typing into it inherits them).
 *   - adjacent runs never have identical attributes.
 */
#ifndef WP_DOCUMENT_H
#define WP_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  WP_ATTR_BOLD      = 1u << 0,
  WP_ATTR_ITALIC    = 1u << 1,
  WP_ATTR_UNDERLINE = 1u << 2,
};

typedef struct WpTextAttrs {
  uint32_t    flags;    /* WP_ATTR_* */
  float       size_pt;  /* 0 = document default */
  const char *family;   /* interned via wp_intern(); NULL = document default */
  uint32_t    comment;  /* id of the comment anchored on this text, 0 = none; see WpComment */
} WpTextAttrs;

/* A comment on a range of text. The range is not stored here: it is the
 * text whose runs carry the comment's id, so anchors move with the text
 * through every edit and undo for free. A comment whose id no run carries
 * any more (its text was deleted) is an orphan: it is kept, so that undo can
 * bring it back, but hidden from queries and never written to a file. */
typedef struct WpComment {
  uint32_t id;
  uint32_t parent;   /* id of the comment this replies to, 0 for a top-level comment */
  char    *author;
  char    *date;     /* ISO 8601, as written to the file */
  char    *text;     /* paragraphs separated by '\n' */
} WpComment;

typedef enum WpAlign {
  WP_ALIGN_LEFT,
  WP_ALIGN_CENTER,
  WP_ALIGN_RIGHT,
  WP_ALIGN_JUSTIFY,
} WpAlign;

/* Colours are 0x01RRGGBB; 0 means none. */
#define WP_COLOR(r, g, b) (0x01000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
static inline bool wp_color_set(uint32_t c) { return (c & 0x01000000u) != 0; }
static inline double wp_color_r(uint32_t c) { return ((c >> 16) & 0xff) / 255.0; }
static inline double wp_color_g(uint32_t c) { return ((c >> 8) & 0xff) / 255.0; }
static inline double wp_color_b(uint32_t c) { return (c & 0xff) / 255.0; }

/* Lists. A paragraph in a list carries its kind and level; the label it is
 * drawn with (a bullet, or a number counted from the items before it) is
 * computed, never stored, so inserting an item renumbers the rest for free.
 * indent_left_pt is the text's left edge as for any paragraph; the label
 * hangs WP_LIST_HANG_PT to its left, and each level of nesting normally
 * adds WP_LIST_INDENT_PT. */
typedef enum WpListKind {
  WP_LIST_NONE,
  WP_LIST_BULLET,
  WP_LIST_NUMBER,
} WpListKind;

#define WP_LIST_INDENT_PT 36.0f   /* indent step per level (0.5 in) */
#define WP_INDENT_STEP_PT WP_LIST_INDENT_PT   /* what Increase Indent moves an ordinary paragraph by */
#define WP_INDENT_MIN_TEXT_PT 72.0f           /* indenting stops when a line would have less text than this */
#define WP_LIST_HANG_PT   18.0f   /* label gutter to the left of the text */
#define WP_LIST_MAX_LEVEL 9       /* ODF has ten levels */

/* A tab stop: where a tab in the text advances to, measured from the
 * paragraph's left indent, and how the text after it sits on the stop.
 * Beyond a paragraph's last stop, tabs fall every WP_TAB_DEFAULT_PT. */
typedef enum WpTabKind { WP_TAB_LEFT, WP_TAB_CENTER, WP_TAB_RIGHT, WP_TAB_DECIMAL } WpTabKind;
typedef struct WpTabStop { float pos_pt; WpTabKind kind; } WpTabStop;
#define WP_MAX_TABS 16
#define WP_TAB_DEFAULT_PT 36.0f

typedef struct WpParaStyle {
  WpAlign  align;
  float    space_before_pt;
  float    space_after_pt;
  float    line_spacing;     /* multiplier; 1.0 = single */
  float    indent_left_pt;   /* text inset from the page margins */
  float    indent_right_pt;
  float    padding_pt;       /* space between the box edge and the text, when boxed */
  float    border_left_pt;   /* width of a rule down the left edge; 0 = none */
  uint32_t border_color;
  uint32_t background;       /* fill behind the paragraph; 0 = none */
  WpListKind list_kind;      /* WP_LIST_NONE for an ordinary paragraph */
  int      list_level;       /* 0 = outermost; meaningful only in a list */
  float    indent_first_pt;  /* the first line's extra inset from indent_left; negative hangs it. Ignored for a list item, whose label hangs instead */
  int      ntabs;            /* tab stops, sorted by position; unused slots are zero so styles compare with memcmp */
  WpTabStop tabs[WP_MAX_TABS];
} WpParaStyle;

/* Add a stop (replacing one within half a point of it; the table is kept
 * sorted, and a full table drops the farthest stop) or remove the stop at
 * a position. remove returns false when there was none. */
void wp_para_style_add_tab(WpParaStyle *st, float pos_pt, WpTabKind kind);
bool wp_para_style_remove_tab(WpParaStyle *st, float pos_pt);

/* The indent a list item at `level` normally has. */
static inline float wp_list_indent(int level) { return (float)(level + 1) * WP_LIST_INDENT_PT; }

/* A paragraph with a background or a border is drawn as a box. */
static inline bool wp_para_style_boxed(const WpParaStyle *st) { return wp_color_set(st->background) || st->border_left_pt > 0; }

typedef struct WpPageSetup {
  double width, height;                      /* points */
  double margin_top, margin_bottom, margin_left, margin_right;
} WpPageSetup;

static inline WpPageSetup wp_page_setup_letter(void) { return (WpPageSetup){ 612, 792, 72, 72, 72, 72 }; }
static inline WpPageSetup wp_page_setup_a4(void)     { return (WpPageSetup){ 595.28, 841.89, 72, 72, 72, 72 }; }

typedef struct WpRun {
  size_t      len;   /* bytes */
  WpTextAttrs attrs;
} WpRun;

/* A named paragraph style ("Standard", "Heading 1", ...). Paragraphs refer to
 * one by name; the engine draws paragraph text with the style's base
 * attributes and run attributes layered on top. */
#define WP_STYLE_STANDARD "Standard"
typedef struct WpNamedStyle {
  const char *name;            /* interned */
  int         outline_level;   /* 0 = not a heading */
  bool        keep_with_next;  /* pagination hint */
  const char *next;            /* style for the paragraph Enter creates at the end; NULL = same */
  WpParaStyle para;
  WpTextAttrs text;            /* base character attributes; 0/NULL = document default */
} WpNamedStyle;

typedef struct WpParagraph {
  char       *text;
  size_t      len, cap;
  WpRun      *runs;
  size_t      nruns, runs_cap;
  const char *style_name;   /* interned; see WpNamedStyle */
  WpParaStyle style;        /* effective paragraph properties (copied from the named style, then edited) */
} WpParagraph;

typedef struct WpPos {
  size_t para;
  size_t offset;   /* byte offset within paragraph, 0..len inclusive */
} WpPos;

typedef struct WpDocument WpDocument;
typedef void (*WpDocumentChangedFn)(WpDocument *doc, size_t first_para, void *user);

struct WpDocument {
  WpParagraph *paras;
  size_t       nparas, paras_cap;
  WpTextAttrs  default_attrs;   /* family and size_pt are always set */
  WpParaStyle  default_style;
  WpPageSetup  page;
  WpNamedStyle *styles;
  size_t        nstyles, styles_cap;
  WpComment    *comments;
  size_t        ncomments, comments_cap;
  uint32_t      next_comment_id;
  WpDocumentChangedFn on_change;
  void        *on_change_user;
};

/* String interning so attribute families compare by pointer. */
const char *wp_intern(const char *s);

/* UTF-8 stepping helpers (safe at boundaries). */
size_t wp_utf8_next(const char *text, size_t len, size_t off);
size_t wp_utf8_prev(const char *text, size_t off);

bool wp_attrs_equal(const WpTextAttrs *a, const WpTextAttrs *b);
int  wp_pos_cmp(WpPos a, WpPos b);
static inline WpPos wp_pos_min(WpPos a, WpPos b) { return wp_pos_cmp(a, b) <= 0 ? a : b; }
static inline WpPos wp_pos_max(WpPos a, WpPos b) { return wp_pos_cmp(a, b) >= 0 ? a : b; }
static inline bool  wp_pos_eq(WpPos a, WpPos b)  { return a.para == b.para && a.offset == b.offset; }

WpDocument *wp_document_new(void);
void        wp_document_free(WpDocument *doc);
void        wp_document_clear(WpDocument *doc);   /* leaves one empty paragraph */

size_t             wp_document_para_count(const WpDocument *doc);
const WpParagraph *wp_document_para(const WpDocument *doc, size_t i);
WpPos              wp_document_end(const WpDocument *doc);
WpPos              wp_document_clamp(const WpDocument *doc, WpPos p);

/* Move one character. Return false at document boundaries. */
bool wp_document_pos_forward(const WpDocument *doc, WpPos *p);
bool wp_document_pos_backward(const WpDocument *doc, WpPos *p);
/* Word boundaries within the paragraph (used for double-click / ctrl+arrows). */
void wp_document_word_bounds(const WpDocument *doc, WpPos p, WpPos *start, WpPos *end);

/* Insert UTF-8 text. '\n' splits paragraphs. If attrs is NULL the text
 * inherits the attributes of the character before the insertion point.
 * Returns the position just after the inserted text. */
WpPos wp_document_insert_text(WpDocument *doc, WpPos pos, const char *text, size_t len,
                              const WpTextAttrs *attrs);
void  wp_document_delete_range(WpDocument *doc, WpPos a, WpPos b);

/* Attributes of the character before pos (or of the paragraph if empty). */
WpTextAttrs wp_document_attrs_at(const WpDocument *doc, WpPos pos);
/* The same, as text typed at pos should get: the comment id is kept only
 * strictly inside a commented range, which is what insert_text does when
 * attrs is NULL. Front ends that build explicit typing attributes start here. */
WpTextAttrs wp_document_typing_attrs(const WpDocument *doc, WpPos pos);
/* Fill in defaults for size/family. */
WpTextAttrs wp_document_resolve_attrs(const WpDocument *doc, const WpTextAttrs *a);

typedef void (*WpAttrsFn)(WpTextAttrs *attrs, void *user);
typedef void (*WpParaStyleFn)(WpParaStyle *style, void *user);
void wp_document_apply_attrs(WpDocument *doc, WpPos a, WpPos b, WpAttrsFn fn, void *user);
void wp_document_apply_para_style(WpDocument *doc, WpPos a, WpPos b, WpParaStyleFn fn, void *user);
void wp_document_set_flags(WpDocument *doc, WpPos a, WpPos b, uint32_t mask, bool set);

/* Named styles. The built-ins are Standard, Title, Heading 1..3, Quote
 * (indented, italic: a block quotation), Note (a tinted box with a rule
 * down its left edge: an aside to the reader), and List Bullet and List
 * Number (items of a list; see WpListKind). */
#define WP_STYLE_LIST_BULLET "List Bullet"
#define WP_STYLE_LIST_NUMBER "List Number"
const WpNamedStyle *wp_document_find_style(const WpDocument *doc, const char *name);
const WpNamedStyle *wp_document_style_of(const WpDocument *doc, size_t para);   /* never NULL */
void wp_document_add_style(WpDocument *doc, const WpNamedStyle *style);        /* insert or replace by name */
void wp_document_reset_styles(WpDocument *doc);                                /* back to the built-ins */
void wp_document_set_style_name(WpDocument *doc, WpPos a, WpPos b, const char *name);
/* Resolved base attributes for a paragraph: document defaults + its style. */
WpTextAttrs wp_document_para_base_attrs(const WpDocument *doc, size_t para);

/* Lists. list_numbers fills out[i] with the number of paragraph i in its
 * list (1-based), 0 for a bullet or an ordinary paragraph. Numbering runs
 * along consecutive items at one level: an ordinary paragraph ends every
 * list, a bullet ends the numbering at its level and deeper, a deeper item
 * leaves the outer count where it was. list_label writes the label an item
 * shows (bullets and number styles cycle by level: "• ◦ ▪" and "1. a. i.");
 * buf needs 16 bytes. */
void wp_document_list_numbers(const WpDocument *doc, int *out);
void wp_list_label(WpListKind kind, int level, int number, char *buf);

/* Comments. add_comment marks the runs in [a, b) with a fresh id and
 * records the comment; new_comment_record only records (the reader marks
 * runs itself). Strings are copied. remove_comment clears the runs and
 * drops the record. comment_range finds the text carrying the id; false for
 * an orphan. comment_at is the id on the character after pos, else the one
 * before, else 0. Typing at the edge of a commented range does not extend
 * it, only typing inside does.
 *
 * A reply is a record with a parent and no runs of its own: it shares its
 * parent's text, is an orphan when the parent is, and goes when the parent
 * is removed. Replies to a reply attach to the top-level comment. */
uint32_t         wp_document_add_comment(WpDocument *doc, WpPos a, WpPos b, const char *author, const char *date, const char *text);
uint32_t         wp_document_add_reply(WpDocument *doc, uint32_t parent, const char *author, const char *date, const char *text);   /* 0 if parent unknown */
uint32_t         wp_document_new_comment_record(WpDocument *doc, uint32_t parent, const char *author, const char *date, const char *text);
const WpComment *wp_document_comment(const WpDocument *doc, uint32_t id);      /* NULL if unknown */
uint32_t         wp_document_comment_root(const WpDocument *doc, uint32_t id); /* the top-level comment of id's thread */
void             wp_document_set_comment_text(WpDocument *doc, uint32_t id, const char *text);
void             wp_document_remove_comment(WpDocument *doc, uint32_t id);
bool             wp_document_comment_range(const WpDocument *doc, uint32_t id, WpPos *a, WpPos *b);
uint32_t         wp_document_comment_at(const WpDocument *doc, WpPos pos);
/* Snapshots of the comment table for undo; set_comments replaces it with deep copies. */
WpComment       *wp_document_copy_comments(const WpDocument *doc, size_t *count);
void             wp_comments_free(WpComment *comments, size_t count);
void             wp_document_set_comments(WpDocument *doc, const WpComment *src, size_t nsrc);

/* Plain text in and out. Returned buffer is malloc'd and NUL-terminated. */
char *wp_document_get_text(const WpDocument *doc, WpPos a, WpPos b, size_t *out_len);
char *wp_document_get_all_text(const WpDocument *doc, size_t *out_len);
void  wp_document_load_text(WpDocument *doc, const char *text, size_t len);

/* Paragraph-range snapshots, the primitive the editor's undo is built on.
 * copy_paras returns deep copies of [first, first + count); replace_paras
 * swaps [first, first + count) for deep copies of src and notifies. */
WpParagraph *wp_document_copy_paras(const WpDocument *doc, size_t first, size_t count);
void         wp_paragraphs_free(WpParagraph *paras, size_t count);
void         wp_document_replace_paras(WpDocument *doc, size_t first, size_t count,
                                       const WpParagraph *src, size_t nsrc);

/* Paragraph fragments: a range of a document with its formatting, as the
 * clipboard holds it. copy_range returns deep copies of the paragraphs a..b
 * trimmed to the range (always >= 1). insert_paras puts a fragment at pos:
 * the first paragraph's text joins the paragraph there, paragraphs after it
 * become new paragraphs, and the text after pos ends up after the last one.
 * A paragraph in the fragment whose style_name is NULL (see
 * wp_paragraphs_from_text) takes the style of the paragraph at pos; one with
 * a style applies it only where it does not merge with existing text, i.e.
 * to the paragraph at pos when nothing precedes pos in it, and to the
 * remainder when nothing follows pos. Comment ids in the fragment survive
 * only when the document knows the comment and no text carries the id any
 * more (a cut being pasted back); a copy of commented text pastes plain.
 * Returns the position just after the inserted content. */
WpParagraph *wp_document_copy_range(const WpDocument *doc, WpPos a, WpPos b, size_t *count);
WpPos        wp_document_insert_paras(WpDocument *doc, WpPos pos, const WpParagraph *src, size_t nsrc);
/* Plain text as an unstyled fragment (style_name NULL); '\n' and "\r\n" separate paragraphs. */
WpParagraph *wp_paragraphs_from_text(const char *text, size_t len, size_t *count);

/* Emit the change notification for paragraphs >= first_para. */
void wp_document_changed(WpDocument *doc, size_t first_para);

#endif
