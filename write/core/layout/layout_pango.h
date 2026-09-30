#ifndef WP_LAYOUT_PANGO_H
#define WP_LAYOUT_PANGO_H

#include "layout/layout.h"

/* The first engine: Pango does shaping and paragraph line-breaking; this
 * module does pagination and geometry queries on top. */
WpLayoutEngine *wp_layout_pango_new(void);

#endif
