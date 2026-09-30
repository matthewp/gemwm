#include "gem-print.h"
#include "layout/layout.h"
#include "print.h"

struct job {
	WpEditor *ed;
	void (*done)(const char *message, void *data);
	void *data;
};

/* Comments are marked on the screen, not on paper: off while printing. */
static void begin_print(GtkPrintOperation *op, GtkPrintContext *ctx,
		gpointer data) {
	struct job *job = data;
	wp_layout_set_comment_marks(wp_editor_engine(job->ed), false);
	gtk_print_operation_set_n_pages(op,
		(int)wp_layout_page_count(wp_editor_engine(job->ed)));
}

/* A page as the engine lays it out, at its own size: the operation's page
 * is the document's, and the whole of it. */
static void draw_page(GtkPrintOperation *op, GtkPrintContext *ctx, int page,
		gpointer data) {
	struct job *job = data;
	cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
	cairo_set_source_rgb(cr, 0, 0, 0);
	wp_layout_render_page(wp_editor_engine(job->ed), page, cr);
}

static void end_print(GtkPrintOperation *op, GtkPrintContext *ctx,
		gpointer data) {
	struct job *job = data;
	wp_layout_set_comment_marks(wp_editor_engine(job->ed), true);
}

static void printed(const char *message, void *data) {
	struct job *job = data;
	if (job->done != NULL) {
		job->done(message, job->data);
	}
	g_free(job);
}

void print_document(GtkWindow *parent, WpEditor *ed, const char *name,
		void (*done)(const char *message, void *data), void *data) {
	struct job *job = g_new0(struct job, 1);
	job->ed = ed;
	job->done = done;
	job->data = data;

	const WpPageSetup *pg = &wp_editor_document(ed)->page;
	GtkPaperSize *paper = gtk_paper_size_new_custom("gemwrite", "Document",
		pg->width, pg->height, GTK_UNIT_POINTS);
	GtkPageSetup *setup = gtk_page_setup_new();
	gtk_page_setup_set_paper_size(setup, paper);
	gtk_page_setup_set_top_margin(setup, 0, GTK_UNIT_POINTS);
	gtk_page_setup_set_bottom_margin(setup, 0, GTK_UNIT_POINTS);
	gtk_page_setup_set_left_margin(setup, 0, GTK_UNIT_POINTS);
	gtk_page_setup_set_right_margin(setup, 0, GTK_UNIT_POINTS);
	GtkPrintOperation *op = gtk_print_operation_new();
	gtk_print_operation_set_default_page_setup(op, setup);
	gtk_print_operation_set_use_full_page(op, TRUE);
	gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
	gtk_print_operation_set_job_name(op, name);
	g_signal_connect(op, "begin-print", G_CALLBACK(begin_print), job);
	g_signal_connect(op, "draw-page", G_CALLBACK(draw_page), job);
	g_signal_connect(op, "end-print", G_CALLBACK(end_print), job);
	g_object_unref(setup);
	gtk_paper_size_free(paper);
	gem_print_dialog(parent, op, NULL, 0, printed, job);
}
