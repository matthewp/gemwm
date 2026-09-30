/*
 * The page canvas: the document's pages stacked down GEM's grey, drawn by
 * the layout engine with the document's own fonts, and the caret and
 * selection on them. It turns keys, clicks and drags into editor calls;
 * the editor holds the caret, the selection and every edit.
 */
#ifndef GEMWRITE_PAGE_H
#define GEMWRITE_PAGE_H

#include <gtk/gtk.h>
#include "edit/editor.h"

G_BEGIN_DECLS

#define WP_TYPE_PAGE_VIEW (wp_page_view_get_type())
G_DECLARE_FINAL_TYPE(WpPageView, wp_page_view, WP, PAGE_VIEW, GtkWidget)

/* The view drives an editor it doesn't own; keep the editor alive as long
 * as the view. */
WpPageView *wp_page_view_new(WpEditor *editor);
WpEditor *wp_page_view_get_editor(WpPageView *v);

/* Zoom is either set (set_zoom) or fitted to the width (the default): the
 * page shrinks to the window but never grows past 100%. */
void wp_page_view_set_zoom(WpPageView *v, double zoom);
void wp_page_view_set_fit_width(WpPageView *v);
gboolean wp_page_view_get_fit_width(WpPageView *v);
double wp_page_view_get_zoom(WpPageView *v);   /* points to pixels */
/* x of the pages' left edge, in the view's coordinates (for the ruler). */
double wp_page_view_get_page_x(WpPageView *v);

/* The window's part: menu is called for a right click (or the Menu key)
 * at x, y in the view, once the caret or selection is where it applies (a
 * misspelled word under the pointer is selected); comment when the
 * floating Comment button beside a selection is pressed. */
typedef void (*wp_page_menu_fn)(WpPageView *v, double x, double y, void *data);
typedef void (*wp_page_comment_fn)(WpPageView *v, void *data);
void wp_page_view_set_handlers(WpPageView *v, wp_page_menu_fn menu,
	wp_page_comment_fn comment, void *data);
/* While a menu is up over the view, the floating button stays down. */
void wp_page_view_set_menu_up(WpPageView *v, gboolean up);

/* Signals, from the editor, for the window:
 *   "state-changed"    caret, selection, formatting at the caret, zoom
 *   "document-changed" the document was edited */

G_END_DECLS

#endif
