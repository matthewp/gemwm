/* editor.h — the editing session: a document, a layout engine, the caret and
 * selection, pending typing attributes, and the file it came from.
 *
 * This is the layer every front end drives. The GTK page view translates
 * keys and clicks into calls here and draws the result; the CLI calls the
 * same functions from a script. Nothing in this file knows about a toolkit,
 * so it links into a headless binary. Cairo reaches it only through the
 * layout engine.
 */
#ifndef WP_EDITOR_H
#define WP_EDITOR_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/document.h"
#include "edit/spell.h"
#include "layout/layout.h"

typedef struct WpEditor WpEditor;

/* Both callbacks may be NULL. state_changed fires when the caret, selection,
 * or attributes-at-caret change; document_changed when text or formatting
 * changed from paragraph first_para onwards. */
typedef struct WpEditorListener {
  void (*state_changed)(WpEditor *ed, void *user);
  void (*document_changed)(WpEditor *ed, size_t first_para, void *user);
  void *user;
} WpEditorListener;

/* Movement units for wp_editor_move. LINE and PAGE need the layout engine;
 * LINE_EDGE is Home/End, DOC is Ctrl+Home/End. */
typedef enum WpMoveUnit {
  WP_MOVE_CHAR,
  WP_MOVE_WORD,
  WP_MOVE_LINE,
  WP_MOVE_LINE_EDGE,
  WP_MOVE_PAGE,
  WP_MOVE_DOC,
} WpMoveUnit;

/* The editor owns both the document it creates and the engine it is given. */
WpEditor *wp_editor_new(WpLayoutEngine *engine);
void      wp_editor_free(WpEditor *ed);
void      wp_editor_set_listener(WpEditor *ed, const WpEditorListener *l);

WpDocument     *wp_editor_document(WpEditor *ed);
WpLayoutEngine *wp_editor_engine(WpEditor *ed);

/* ---- file ---------------------------------------------------------------- */

/* .odt by extension, otherwise UTF-8 plain text. On failure *err is a
 * malloc'd message and the document is left as it was on a read error. */
bool        wp_editor_load(WpEditor *ed, const char *path, char **err);
/* path NULL saves to the current path; fails if there is none. */
bool        wp_editor_save(WpEditor *ed, const char *path, char **err);
/* Re-read the current file as one undo step: the caret stays put (clamped),
 * the history survives, and undo backs the file's version out again. Fails,
 * leaving everything as it was, when there is no file or it cannot be read. */
bool        wp_editor_reload(WpEditor *ed, char **err);
/* Whether the file differs from what the editor last read or wrote (by
 * modification time and size). False when there is no file or it is gone. */
bool        wp_editor_file_changed_on_disk(WpEditor *ed);
const char *wp_editor_path(WpEditor *ed);          /* NULL when untitled */
bool        wp_editor_modified(WpEditor *ed);
void        wp_editor_clear(WpEditor *ed);         /* new untitled document */

/* ---- caret and selection ------------------------------------------------- */

WpPos wp_editor_caret(WpEditor *ed);
WpPos wp_editor_anchor(WpEditor *ed);
bool  wp_editor_has_selection(WpEditor *ed);
void  wp_editor_get_selection(WpEditor *ed, WpPos *a, WpPos *b);   /* ordered */
/* extend keeps the anchor where it is so the selection grows. */
void  wp_editor_set_caret(WpEditor *ed, WpPos pos, bool extend);
void  wp_editor_select(WpEditor *ed, WpPos anchor, WpPos caret);
void  wp_editor_select_all(WpEditor *ed);
/* dir < 0 moves backwards. With a selection and no extend, CHAR collapses
 * to the selection's edge like every text widget. Returns false when the
 * caret could not move. */
bool  wp_editor_move(WpEditor *ed, WpMoveUnit unit, int dir, bool extend);
/* Make needle the search term (see find below) and select the first match
 * after the selection, wrapping once. */
bool  wp_editor_find(WpEditor *ed, const char *needle);

/* ---- editing ------------------------------------------------------------- */

/* Replaces the selection if there is one. len < 0 means NUL-terminated. */
void wp_editor_insert_text(WpEditor *ed, const char *text, long len);
/* Deletes the selection, or one character in direction dir when there is none. */
void wp_editor_delete(WpEditor *ed, int dir);
void wp_editor_delete_selection(WpEditor *ed);

/* ---- clipboard ----------------------------------------------------------- */

/* One clipboard for the whole process, holding formatted paragraphs, shared
 * by every window and by the CLI's commands within a run. copy and cut fill
 * it from the selection; paste inserts it in place of the selection as one
 * undo step. Each returns false when there was nothing to do. A front end
 * keeps it in step with the system clipboard: it publishes wp_clipboard_text
 * after a copy, and before a paste loads any text that came from elsewhere
 * with wp_clipboard_set_text. */
bool        wp_editor_copy(WpEditor *ed);
bool        wp_editor_cut(WpEditor *ed);
bool        wp_editor_paste(WpEditor *ed);
const char *wp_clipboard_text(void);                            /* NULL when empty */
void        wp_clipboard_set_text(const char *text, long len);  /* len < 0: NUL-terminated */

/* ---- undo ---------------------------------------------------------------- */

/* Every edit above is one undo step, except that a run of typed characters
 * (or of backspaces, or of forward deletes) at the same spot merges into
 * one. Loading or clearing the document empties the history. */
bool wp_editor_can_undo(WpEditor *ed);
bool wp_editor_can_redo(WpEditor *ed);
bool wp_editor_undo(WpEditor *ed);
bool wp_editor_redo(WpEditor *ed);

/* ---- formatting ---------------------------------------------------------- */

/* Character formatting applies to the selection, or to the next insertion
 * when there is none (the "typing attributes"). */
void        wp_editor_toggle_attr(WpEditor *ed, uint32_t flag);
void        wp_editor_set_family(WpEditor *ed, const char *family);
void        wp_editor_set_size(WpEditor *ed, float size_pt);
/* Attributes the next typed character would get (or those at the start of
 * the selection); family/size may be 0/NULL meaning the paragraph default. */
WpTextAttrs wp_editor_current_attrs(WpEditor *ed);
/* Same, resolved against the paragraph style so family and size are set. */
WpTextAttrs wp_editor_effective_attrs(WpEditor *ed);

/* Paragraph formatting applies to every paragraph the selection touches. */
void        wp_editor_set_align(WpEditor *ed, WpAlign align);
WpAlign     wp_editor_current_align(WpEditor *ed);
/* Line spacing as a multiple of the font's line height: 1.0 single, 2.0 double. */
void        wp_editor_set_line_spacing(WpEditor *ed, float factor);
float       wp_editor_current_line_spacing(WpEditor *ed);
void        wp_editor_set_style_name(WpEditor *ed, const char *name);
const char *wp_editor_current_style_name(WpEditor *ed);

/* Lists. set_list makes every paragraph the selection touches an item of
 * that kind (the List Bullet or List Number style, keeping the level of
 * any that already are items), unless they all are already: then they
 * return to body text, so the same command toggles. Enter on an empty item
 * and Backspace at the start of one outdent it (see indent) rather than
 * adding or joining paragraphs. */
void        wp_editor_set_list(WpEditor *ed, WpListKind kind);
/* Indent (dir > 0) or outdent every paragraph the selection touches: a
 * list item moves one level deeper or shallower (outdenting a top-level
 * item makes it body text), any other paragraph's left edge moves in or
 * out by WP_INDENT_STEP_PT, never past the margin or so far that less than
 * WP_INDENT_MIN_TEXT_PT of the line is left. Returns false when nothing
 * could move. */
bool        wp_editor_indent(WpEditor *ed, int dir);
/* Set one indent of every paragraph the selection touches, in points from
 * the margin (first: from the left indent, negative hanging). Values are
 * clamped so the text keeps WP_INDENT_MIN_TEXT_PT of the line. */
typedef enum WpIndent { WP_INDENT_LEFT, WP_INDENT_RIGHT, WP_INDENT_FIRST } WpIndent;
void        wp_editor_set_indent(WpEditor *ed, WpIndent which, float pt);
/* Tab stops of the paragraphs the selection touches; positions are from
 * the left indent. remove_tab is false when no touched paragraph had one. */
void        wp_editor_add_tab(WpEditor *ed, float pos_pt, WpTabKind kind);
bool        wp_editor_remove_tab(WpEditor *ed, float pos_pt);
void        wp_editor_move_tab(WpEditor *ed, float from_pt, float to_pt);   /* keeps the stop's kind; one step */
void        wp_editor_clear_tabs(WpEditor *ed);
WpListKind  wp_editor_current_list_kind(WpEditor *ed);
int         wp_editor_current_list_level(WpEditor *ed);

/* ---- comments ------------------------------------------------------------ */

/* The author name new comments carry; NULL until set, and the command layer
 * then falls back to the user's settings. The string is copied. */
void        wp_editor_set_author(WpEditor *ed, const char *author);
const char *wp_editor_author(WpEditor *ed);
/* Comment on the selection. Returns the new id, 0 when nothing is selected.
 * date NULL means now. Each of these is one undo step. */
uint32_t    wp_editor_add_comment(WpEditor *ed, const char *author, const char *date, const char *text);
/* Reply to a comment (or to its thread, if id is itself a reply); 0 if unknown. */
uint32_t    wp_editor_add_reply(WpEditor *ed, uint32_t id, const char *author, const char *date, const char *text);
bool        wp_editor_set_comment_text(WpEditor *ed, uint32_t id, const char *text);
bool        wp_editor_remove_comment(WpEditor *ed, uint32_t id);
/* Select the commented text; false for an unknown or orphaned comment. */
bool        wp_editor_select_comment(WpEditor *ed, uint32_t id);
/* The comment on the text at the caret (or at the start of the selection), 0 if none. */
uint32_t    wp_editor_comment_at_caret(WpEditor *ed);

/* ---- spelling ------------------------------------------------------------ */

/* A byte range within one paragraph. */
typedef struct WpSpan { size_t start, end; } WpSpan;

/* The editor owns its checker; NULL turns checking off. Misspellings are
 * found per paragraph the first time they are asked for and remembered
 * until the paragraph's text changes, so asking on every redraw is cheap. */
void          wp_editor_set_spell(WpEditor *ed, WpSpell *sp);
WpSpell      *wp_editor_spell(WpEditor *ed);
/* Misspelled words of a paragraph in order; NULL with *n = 0 when off. */
const WpSpan *wp_editor_misspellings(WpEditor *ed, size_t para, size_t *n);
/* The misspelled word touching pos (pos may be at either end of it). */
bool          wp_editor_misspelled_at(WpEditor *ed, WpPos pos, WpPos *a, WpPos *b);
/* Act on the misspelled word at the caret (or the start of the selection):
 * add it to the personal word list, or ignore it for this session. False
 * when checking is off or the caret is not on a misspelled word. */
bool          wp_editor_spell_add(WpEditor *ed);
bool          wp_editor_spell_ignore(WpEditor *ed);

/* ---- find and replace ---------------------------------------------------- */

/* The editor holds one search term. Its matches are found per paragraph
 * the first time they are asked for and remembered until the paragraph's
 * text changes, like misspellings, so a view can ask on every redraw. A
 * match never spans paragraphs. Matching ignores case unless
 * WP_FIND_MATCH_CASE is set. Setting the term (NULL or empty clears it)
 * reports a state change so views repaint. */
enum { WP_FIND_MATCH_CASE = 1 };
void          wp_editor_set_search(WpEditor *ed, const char *needle, unsigned flags);
const char   *wp_editor_search_term(WpEditor *ed);    /* NULL when none */
unsigned      wp_editor_search_flags(WpEditor *ed);
/* Matches within a paragraph in order; NULL with *n = 0 when there is no term. */
const WpSpan *wp_editor_search_matches(WpEditor *ed, size_t para, size_t *n);
/* Every match in the document; *current (may be NULL) gets the 1-based
 * index of the match the selection covers exactly, or 0. */
size_t        wp_editor_search_count(WpEditor *ed, size_t *current);
/* Select the next match after the selection (dir > 0) or the previous one
 * before it (dir < 0), wrapping once. False when there is no term or no match. */
bool          wp_editor_find_next(WpEditor *ed, int dir);
/* Select the match at or after the start of the selection, wrapping once:
 * what an incremental search does as the term grows. */
bool          wp_editor_find_current(WpEditor *ed);
/* If the selection is exactly a match, replace it with `with` as one undo
 * step and move to the next match; false when it is not. The new text takes
 * the formatting of the match's first character. */
bool          wp_editor_replace(WpEditor *ed, const char *with);
/* Replace every match as one undo step; returns how many. */
size_t        wp_editor_replace_all(WpEditor *ed, const char *with);

/* ---- queries ------------------------------------------------------------- */

size_t wp_editor_word_count(WpEditor *ed, WpPos a, WpPos b);
size_t wp_editor_page_count(WpEditor *ed);

#endif
