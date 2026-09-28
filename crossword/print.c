/*
 * A crossword on paper, laid out like a newspaper's:
 *
 *   Title                                   (bold)
 *   by Author · Source, date
 *   ------------------------------------------------
 *   +-----------------+  | 12 Clue ... | 40 Clue ...
 *   |      grid       |  | 13 Clue ... | DOWN
 *   |                 |  | ...         | 1 Clue ...
 *   +-----------------+  |             |
 *   ACROSS (cont.) ...  | ...         |
 *
 * The page is four columns. The grid takes the first two (three for big
 * grids); the clues fill the columns beside it first, then the ones under
 * it, then further pages. A clue never splits, and a heading never ends a
 * column. GTK draws it onto whatever the printer (or PDF) needs, with
 * Pango for proper print fonts, since the screen's pixel font isn't one.
 */
#include "print.h"

#define COLUMNS 4
#define GAP 14.0         /* between columns, points */
#define CLUE_FONT "Sans 9"
#define TITLE_FONT "Sans Bold 16"
#define SUBTITLE_FONT "Sans 9"
#define HEADING_FONT "Sans Bold 10"

struct item {
	PangoLayout *layout;
	double height;
	bool heading;
};

/* Where each item goes: page, column slot and position. */
struct placed {
	int item, page;
	double x, y;
};

struct job {
	const struct puzzle *puz;
	char *subtitle;
	bool answers;
	GArray *items;   /* struct item */
	GArray *placed;  /* struct placed */
	int pages;
	double width, height, col_w, header_h, grid_size;
	int grid_cols;
	char *message;   /* for done */
	void (*done)(const char *message, void *data);
	void *data;
};

static void job_free(struct job *job) {
	for (guint i = 0; job->items && i < job->items->len; i++) {
		g_object_unref(g_array_index(job->items, struct item, i).layout);
	}
	g_clear_pointer(&job->items, g_array_unref);
	g_clear_pointer(&job->placed, g_array_unref);
	g_free(job->subtitle);
	g_free(job->message);
	g_free(job);
}

static PangoLayout *text_layout(GtkPrintContext *ctx, const char *font,
		const char *markup, double width) {
	PangoLayout *layout = gtk_print_context_create_pango_layout(ctx);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_width(layout, (int)(width * PANGO_SCALE));
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_markup(layout, markup, -1);
	return layout;
}

static double layout_height(PangoLayout *layout) {
	int w, h;
	pango_layout_get_size(layout, &w, &h);
	return (double)h / PANGO_SCALE;
}

static void add_item(struct job *job, GtkPrintContext *ctx, const char *font,
		const char *markup, bool heading) {
	struct item item = { text_layout(ctx, font, markup, job->col_w), 0, heading };
	item.height = layout_height(item.layout) + (heading ? 4 : 3);
	g_array_append_val(job->items, item);
}

static void add_clues(struct job *job, GtkPrintContext *ctx, const char *name,
		GPtrArray *clues) {
	add_item(job, ctx, HEADING_FONT, name, true);
	for (guint i = 0; i < clues->len; i++) {
		const struct clue *c = clues->pdata[i];
		char *text = g_markup_escape_text(c->text, -1);
		char *markup = g_strdup_printf("<b>%d</b>  %s", c->number, text);
		add_item(job, ctx, CLUE_FONT, markup, false);
		g_free(markup);
		g_free(text);
	}
}

/* Column slot n, left to right as a newspaper reads: on the first page a
 * column under the grid starts below it, one beside it at the top; after
 * that, every column of each further page. */
static void slot(const struct job *job, int n, double grid_h, int *page,
		double *x, double *top) {
	double step = job->col_w + GAP;
	*page = n / COLUMNS;
	*x = (n % COLUMNS) * step;
	if (n >= COLUMNS) {
		*top = 0;
	} else if (n < job->grid_cols) {
		*top = job->header_h + grid_h + GAP;
	} else {
		*top = job->header_h;
	}
}

/* Everything measured, then dealt into column slots, page by page. */
static void begin_print(GtkPrintOperation *op, GtkPrintContext *ctx,
		struct job *job) {
	const struct puzzle *p = job->puz;
	job->width = gtk_print_context_get_width(ctx);
	job->height = gtk_print_context_get_height(ctx);
	job->col_w = (job->width - (COLUMNS - 1) * GAP) / COLUMNS;
	job->grid_cols = MAX(p->width, p->height) > 17 ? 3 : 2;
	job->grid_size = job->grid_cols * job->col_w + (job->grid_cols - 1) * GAP;

	/* The header's height, from its real text. */
	char *title = g_markup_escape_text(p->title, -1);
	PangoLayout *t = text_layout(ctx, TITLE_FONT, title, job->width);
	char *sub = g_markup_escape_text(job->subtitle, -1);
	PangoLayout *s = text_layout(ctx, SUBTITLE_FONT, sub, job->width);
	job->header_h = layout_height(t) + layout_height(s) + 12;
	g_object_unref(t);
	g_object_unref(s);
	g_free(title);
	g_free(sub);

	job->items = g_array_new(FALSE, FALSE, sizeof(struct item));
	add_clues(job, ctx, "ACROSS", p->across);
	add_clues(job, ctx, "DOWN", p->down);

	/* The grid may not fit a page's height either: shrink it to. */
	double room = job->height - job->header_h;
	double cell = MIN(job->grid_size / p->width, room * 0.75 / p->height);
	job->grid_size = cell * MAX(p->width, p->height);
	double grid_h = cell * p->height;

	job->placed = g_array_new(FALSE, FALSE, sizeof(struct placed));
	int page = 0;
	guint i = 0;
	for (int n = 0; i < job->items->len; n++) {
		double x, top, bottom = job->height;
		slot(job, n, grid_h, &page, &x, &top);
		if (bottom - top < 24) {
			continue; /* no room under a tall grid */
		}
		double y = top;
		while (i < job->items->len) {
			struct item *it = &g_array_index(job->items, struct item, i);
			double need = it->height;
			if (it->heading && i + 1 < job->items->len) {
				need += g_array_index(job->items, struct item, i + 1).height;
			}
			if (y + need > bottom && y > top) {
				break; /* on to the next slot */
			}
			struct placed pl = { (int)i, page, x, y };
			g_array_append_val(job->placed, pl);
			y += it->height;
			i++;
		}
	}
	job->pages = page + 1;
	gtk_print_operation_set_n_pages(op, job->pages);
}

static void draw_grid(cairo_t *cr, const struct job *job, double x0, double y0) {
	const struct puzzle *p = job->puz;
	double cell = job->grid_size / MAX(p->width, p->height);
	for (int r = 0; r < p->height; r++) {
		for (int c = 0; c < p->width; c++) {
			if (puz_black(p, r, c)) {
				cairo_rectangle(cr, x0 + c * cell, y0 + r * cell, cell, cell);
			}
		}
	}
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_fill(cr);
	/* Thin lines between squares, a firmer one round the edge. */
	cairo_set_line_width(cr, 0.5);
	for (int r = 0; r <= p->height; r++) {
		cairo_move_to(cr, x0, y0 + r * cell);
		cairo_line_to(cr, x0 + p->width * cell, y0 + r * cell);
	}
	for (int c = 0; c <= p->width; c++) {
		cairo_move_to(cr, x0 + c * cell, y0);
		cairo_line_to(cr, x0 + c * cell, y0 + p->height * cell);
	}
	cairo_stroke(cr);
	cairo_set_line_width(cr, 1.5);
	cairo_rectangle(cr, x0, y0, p->width * cell, p->height * cell);
	cairo_stroke(cr);

	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, cell * 0.3);
	for (int r = 0; r < p->height; r++) {
		for (int c = 0; c < p->width; c++) {
			int n = p->numbers[r * p->width + c];
			if (n > 0) {
				char num[12];
				snprintf(num, sizeof(num), "%d", n);
				cairo_move_to(cr, x0 + c * cell + cell * 0.08,
					y0 + r * cell + cell * 0.3);
				cairo_show_text(cr, num);
			}
		}
	}
	if (!job->answers) {
		return;
	}
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, cell * 0.6);
	for (int r = 0; r < p->height; r++) {
		for (int c = 0; c < p->width; c++) {
			char letter[2] = { p->grid[r * p->width + c], '\0' };
			if (letter[0] == ' ' || letter[0] == '.') {
				continue;
			}
			cairo_text_extents_t te;
			cairo_text_extents(cr, letter, &te);
			cairo_move_to(cr, x0 + c * cell + (cell - te.x_advance) / 2,
				y0 + r * cell + cell * 0.82);
			cairo_show_text(cr, letter);
		}
	}
}

static void draw_page(GtkPrintOperation *op, GtkPrintContext *ctx, int page,
		struct job *job) {
	cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
	cairo_set_source_rgb(cr, 0, 0, 0);
	if (page == 0) {
		char *title = g_markup_escape_text(job->puz->title, -1);
		PangoLayout *t = text_layout(ctx, TITLE_FONT, title, job->width);
		char *sub = g_markup_escape_text(job->subtitle, -1);
		PangoLayout *s = text_layout(ctx, SUBTITLE_FONT, sub, job->width);
		cairo_move_to(cr, 0, 0);
		pango_cairo_show_layout(cr, t);
		cairo_move_to(cr, 0, layout_height(t) + 2);
		pango_cairo_show_layout(cr, s);
		double rule = job->header_h - 6;
		cairo_set_line_width(cr, 0.75);
		cairo_move_to(cr, 0, rule);
		cairo_line_to(cr, job->width, rule);
		cairo_stroke(cr);
		g_object_unref(t);
		g_object_unref(s);
		g_free(title);
		g_free(sub);
		draw_grid(cr, job, 0, job->header_h);
	}
	for (guint i = 0; i < job->placed->len; i++) {
		struct placed *pl = &g_array_index(job->placed, struct placed, i);
		if (pl->page == page) {
			struct item *it = &g_array_index(job->items, struct item, pl->item);
			cairo_move_to(cr, pl->x, pl->y);
			pango_cairo_show_layout(cr, it->layout);
		}
	}
}

/* The print dialog's extra tab: blank grid or the board as it stands. */
static GtkWidget *custom_widget(GtkPrintOperation *op, struct job *job) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_top(box, 12);
	GtkWidget *check = gtk_check_button_new_with_label("Include my answers");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), job->answers);
	gtk_box_append(GTK_BOX(box), check);
	return box;
}

static void custom_apply(GtkPrintOperation *op, GtkWidget *widget,
		struct job *job) {
	GtkWidget *check = gtk_widget_get_first_child(widget);
	job->answers = gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
}

static struct job *job_new(const struct puzzle *p, const char *subtitle,
		bool answers, GtkPrintOperation *op) {
	struct job *job = g_new0(struct job, 1);
	job->puz = p;
	job->subtitle = g_strdup(subtitle ? subtitle : "");
	job->answers = answers;
	gtk_print_operation_set_job_name(op, p->title);
	gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
	gtk_print_operation_set_embed_page_setup(op, TRUE);
	g_signal_connect(op, "begin-print", G_CALLBACK(begin_print), job);
	g_signal_connect(op, "draw-page", G_CALLBACK(draw_page), job);
	return job;
}

/* The dialog remembers its printer and choices for the session. */
static GtkPrintSettings *settings;
static bool with_answers = true;

static void print_done(GtkPrintOperation *op, GtkPrintOperationResult result,
		struct job *job) {
	if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
		g_clear_object(&settings);
		settings = g_object_ref(gtk_print_operation_get_print_settings(op));
		with_answers = job->answers;
		if (job->done) {
			job->done("Sent to the printer.", job->data);
		}
	} else if (result == GTK_PRINT_OPERATION_RESULT_ERROR && job->done) {
		GError *error = NULL;
		gtk_print_operation_get_error(op, &error);
		char *message = g_strdup_printf("Couldn't print: %s",
			error ? error->message : "unknown error");
		job->done(message, job->data);
		g_free(message);
		g_clear_error(&error);
	}
	job_free(job);
	g_object_unref(op);
}

void print_puzzle(GtkWindow *parent, const struct puzzle *p,
		const char *subtitle, void (*done)(const char *message, void *data),
		void *data) {
	GtkPrintOperation *op = gtk_print_operation_new();
	struct job *job = job_new(p, subtitle, with_answers, op);
	job->done = done;
	job->data = data;
	if (settings != NULL) {
		gtk_print_operation_set_print_settings(op, settings);
	}
	gtk_print_operation_set_custom_tab_label(op, "Crossword");
	g_signal_connect(op, "create-custom-widget", G_CALLBACK(custom_widget), job);
	g_signal_connect(op, "custom-widget-apply", G_CALLBACK(custom_apply), job);
	g_signal_connect(op, "done", G_CALLBACK(print_done), job);
	gtk_print_operation_set_allow_async(op, TRUE);
	GError *error = NULL;
	GtkPrintOperationResult result = gtk_print_operation_run(op,
		GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent, &error);
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
		if (done) {
			char *message = g_strdup_printf("Couldn't print: %s", error->message);
			done(message, data);
			g_free(message);
		}
		g_error_free(error);
	}
}

bool print_puzzle_pdf(const struct puzzle *p, const char *subtitle,
		bool answers, const char *path, GError **error) {
	GtkPrintOperation *op = gtk_print_operation_new();
	struct job *job = job_new(p, subtitle, answers, op);
	gtk_print_operation_set_export_filename(op, path);
	GtkPrintOperationResult result = gtk_print_operation_run(op,
		GTK_PRINT_OPERATION_ACTION_EXPORT, NULL, error);
	job_free(job);
	g_object_unref(op);
	return result != GTK_PRINT_OPERATION_RESULT_ERROR;
}
