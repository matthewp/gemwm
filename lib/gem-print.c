/*
 * The print dialog (see gem-print.h). A small GEM form:
 *
 *   Printer:
 *   +------------------------------------+
 *   | Kyocera                      Ready |   (the chosen one inverted)
 *   | PDF file                 Documents |
 *   +------------------------------------+
 *   Copies:  [-] 1 [+]
 *   Pages:   [All][Range]  [-] 1 [+] to [-] 1 [+]
 *   [x] Include my answers                    (the app's choices)
 *   --------------------------------------
 *                            [Cancel] [Print]
 *
 * Printers turn up as GTK finds them. Print sets the operation's settings
 * and runs it with no dialog of GTK's; "PDF file" exports instead.
 */
#include <gtk/gtkunixprint.h>
#include <string.h>
#include "gem-draw.h"
#include "gem-print.h"

#define PAD 8
#define ROW_H 24
#define BAR_H 34      /* the bottom row: status and buttons */
#define BUTTON_H 18
#define WIDTH 400
#define LABEL_W 80    /* "Copies:", "Pages:" */
#define STEP_W 20     /* a stepper's - and + */
#define VALUE_W 32    /* and its number */
#define MAX_SHOWN 8   /* printers listed */
#define MAX_COPIES 99
#define MAX_PAGE 999

enum action {
	ACT_NONE,
	ACT_PRINTER,   /* index: the printer, or printers->len for PDF */
	ACT_COPIES_DOWN,
	ACT_COPIES_UP,
	ACT_ALL,
	ACT_RANGE,
	ACT_FROM_DOWN,
	ACT_FROM_UP,
	ACT_TO_DOWN,
	ACT_TO_UP,
	ACT_CHOICE,    /* index: which */
	ACT_CANCEL,
	ACT_PRINT,
};

struct hit {
	int x, y, w, h;
	enum action action;
	int index;
};

/* How good a reason a printer has to be the one chosen for you. */
enum rank { RANK_NONE, RANK_FIRST, RANK_DEFAULT, RANK_LAST_USED };

struct dialog {
	int refs;              /* while open or printing, and while listing */
	GtkWidget *window, *area;
	GtkPrintOperation *op;
	GPtrArray *printers;   /* GtkPrinter, as found */
	bool listing;
	char *printer;         /* the chosen one's name */
	bool pdf;              /* or PDF file */
	enum rank rank;
	bool picked;           /* chosen by hand: leave it be */
	int copies, from, to;
	bool range;
	struct gem_print_choice *choices;
	bool *values;          /* the check boxes, until Print */
	int n_choices;
	GArray *hits;
	enum action pressed;
	int pressed_index;
	int height;
	char *pdf_path;
	bool finished;
	void (*done)(const char *message, void *data);
	void *data;
};

/* Remembered for next time, while the app runs. */
static char *last_printer;
static bool last_pdf;
static bool *last_values;

static void dialog_unref(struct dialog *d) {
	if (--d->refs > 0) {
		return;
	}
	for (guint i = 0; i < d->printers->len; i++) {
		g_signal_handlers_disconnect_by_data(d->printers->pdata[i], d);
	}
	g_ptr_array_unref(d->printers);
	g_array_unref(d->hits);
	g_free(d->printer);
	g_free(d->pdf_path);
	g_free(d->choices);
	g_free(d->values);
	g_free(d);
}

static void redraw(struct dialog *d) {
	if (d->area != NULL) {
		gtk_widget_queue_draw(d->area);
	}
}

static void close_window(struct dialog *d) {
	if (d->window != NULL) {
		GtkWidget *w = d->window;
		d->window = d->area = NULL;
		gtk_window_destroy(GTK_WINDOW(w));
	}
}

/* ---- Printers ----------------------------------------------------------- */

static void printer_changed(GObject *printer, GParamSpec *pspec,
		struct dialog *d) {
	redraw(d);
}

static void choose(struct dialog *d, const char *name, bool pdf, enum rank rank) {
	g_free(d->printer);
	d->printer = g_strdup(name);
	d->pdf = pdf;
	d->rank = rank;
}

static gboolean found(GtkPrinter *printer, void *data) {
	struct dialog *d = data;
	if (d->window == NULL) {
		return TRUE; /* gone: stop looking */
	}
	const char *name = gtk_printer_get_name(printer);
	if (gtk_printer_is_virtual(printer)) {
		return FALSE; /* GTK's Print to File: PDF file does that */
	}
	for (guint i = 0; i < d->printers->len; i++) {
		if (strcmp(gtk_printer_get_name(d->printers->pdata[i]), name) == 0) {
			return FALSE;
		}
	}
	g_ptr_array_add(d->printers, g_object_ref(printer));
	g_signal_connect(printer, "notify", G_CALLBACK(printer_changed), d);
	if (!d->picked) {
		enum rank rank = g_strcmp0(name, last_printer) == 0 && !last_pdf ?
			RANK_LAST_USED : gtk_printer_is_default(printer) ? RANK_DEFAULT :
			RANK_FIRST;
		if (rank > d->rank) {
			choose(d, name, false, rank);
		}
	}
	redraw(d);
	return FALSE;
}

static void listed(void *data) {
	struct dialog *d = data;
	d->listing = false;
	redraw(d);
	dialog_unref(d);
}

static const char *printer_state(GtkPrinter *p) {
	const char *message = gtk_printer_get_state_message(p);
	return gtk_printer_is_paused(p) ? "Paused" :
		!gtk_printer_is_accepting_jobs(p) ? "Not accepting jobs" :
		message != NULL && message[0] != '\0' ? message : "Ready";
}

/* ---- Printing ----------------------------------------------------------- */

static char *job_name(GtkPrintOperation *op) {
	char *name = NULL;
	g_object_get(op, "job-name", &name, NULL);
	return name;
}

/* Documents/Name.pdf, or Name (2).pdf and so on if that's taken. */
static char *pdf_path(const char *name) {
	const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
	if (dir == NULL) {
		dir = g_get_home_dir();
	}
	g_mkdir_with_parents(dir, 0755);
	char *base = g_strdelimit(g_strdup(name != NULL && name[0] ? name :
		"Untitled"), "/", '-');
	char *path = NULL;
	for (int n = 1; path == NULL; n++) {
		char *file = n == 1 ? g_strdup_printf("%s.pdf", base) :
			g_strdup_printf("%s (%d).pdf", base, n);
		path = g_build_filename(dir, file, NULL);
		g_free(file);
		if (g_file_test(path, G_FILE_TEST_EXISTS)) {
			g_clear_pointer(&path, g_free);
		}
	}
	g_free(base);
	return path;
}

static void finish(struct dialog *d, GtkPrintOperationResult result) {
	if (d->finished) {
		return;
	}
	d->finished = true;
	char *message = NULL;
	if (result == GTK_PRINT_OPERATION_RESULT_APPLY && d->pdf) {
		/* Short: the folder and the file, not the whole path. */
		char *dir = g_path_get_dirname(d->pdf_path);
		char *folder = g_path_get_basename(dir);
		char *file = g_path_get_basename(d->pdf_path);
		message = g_strdup_printf("Saved %s/%s.", folder, file);
		g_free(dir);
		g_free(folder);
		g_free(file);
	} else if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
		message = g_strdup_printf("Sent to %s.", d->printer);
	} else if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
		GError *error = NULL;
		gtk_print_operation_get_error(d->op, &error);
		message = g_strdup_printf("Couldn't print: %s",
			error != NULL ? error->message : "unknown error");
		g_clear_error(&error);
	} else {
		message = g_strdup("Printing cancelled.");
	}
	if (d->done != NULL) {
		d->done(message, d->data);
	}
	g_free(message);
	g_object_unref(d->op);
	dialog_unref(d); /* the dialog's own ref */
}

static void op_done(GtkPrintOperation *op, GtkPrintOperationResult result,
		struct dialog *d) {
	finish(d, result);
}

static void print(struct dialog *d) {
	for (int i = 0; i < d->n_choices; i++) {
		*d->choices[i].value = d->values[i];
	}
	g_free(last_values);
	last_values = g_memdup2(d->values, sizeof(bool) * d->n_choices);
	g_free(last_printer);
	last_printer = g_strdup(d->printer);
	last_pdf = d->pdf;
	close_window(d);

	GtkPrintSettings *s = gtk_print_operation_get_print_settings(d->op);
	s = s != NULL ? gtk_print_settings_copy(s) : gtk_print_settings_new();
	GtkPrintOperationAction action = GTK_PRINT_OPERATION_ACTION_PRINT;
	if (d->pdf) {
		char *name = job_name(d->op);
		d->pdf_path = pdf_path(name);
		g_free(name);
		gtk_print_operation_set_export_filename(d->op, d->pdf_path);
		action = GTK_PRINT_OPERATION_ACTION_EXPORT;
	} else {
		gtk_print_settings_set_printer(s, d->printer);
		gtk_print_settings_set_n_copies(s, d->copies);
		if (d->range) {
			GtkPageRange r = { d->from - 1, d->to - 1 };
			gtk_print_settings_set_print_pages(s, GTK_PRINT_PAGES_RANGES);
			gtk_print_settings_set_page_ranges(s, &r, 1);
		} else {
			gtk_print_settings_set_print_pages(s, GTK_PRINT_PAGES_ALL);
		}
	}
	gtk_print_operation_set_print_settings(d->op, s);
	g_object_unref(s);
	gtk_print_operation_set_allow_async(d->op, TRUE);
	g_signal_connect(d->op, "done", G_CALLBACK(op_done), d);

	d->refs++; /* finish may come while running */
	GtkPrintOperationResult result = gtk_print_operation_run(d->op, action,
		NULL, NULL);
	if (result != GTK_PRINT_OPERATION_RESULT_IN_PROGRESS) {
		finish(d, result);
	}
	dialog_unref(d);
}

static void cancel(struct dialog *d) {
	close_window(d);
	if (d->done != NULL) {
		d->done(NULL, d->data);
	}
	g_object_unref(d->op);
	dialog_unref(d);
}

static bool can_print(const struct dialog *d) {
	return d->pdf || d->printer != NULL;
}

/* ---- Acting ------------------------------------------------------------- */

static void act(struct dialog *d, enum action action, int index) {
	switch (action) {
	case ACT_PRINTER:
		if (index < (int)d->printers->len) {
			choose(d, gtk_printer_get_name(d->printers->pdata[index]), false,
				RANK_LAST_USED);
		} else {
			choose(d, NULL, true, RANK_LAST_USED);
		}
		d->picked = true;
		break;
	case ACT_COPIES_DOWN:
		d->copies = MAX(1, d->copies - 1);
		break;
	case ACT_COPIES_UP:
		d->copies = MIN(MAX_COPIES, d->copies + 1);
		break;
	case ACT_ALL:
	case ACT_RANGE:
		d->range = action == ACT_RANGE;
		break;
	case ACT_FROM_DOWN:
		d->from = MAX(1, d->from - 1);
		break;
	case ACT_FROM_UP:
		d->from = MIN(MAX_PAGE, d->from + 1);
		d->to = MAX(d->to, d->from);
		break;
	case ACT_TO_DOWN:
		d->to = MAX(1, d->to - 1);
		d->from = MIN(d->from, d->to);
		break;
	case ACT_TO_UP:
		d->to = MIN(MAX_PAGE, d->to + 1);
		break;
	case ACT_CHOICE:
		d->values[index] = !d->values[index];
		break;
	case ACT_CANCEL:
		cancel(d);
		return;
	case ACT_PRINT:
		if (can_print(d)) {
			print(d);
		}
		return;
	default:
		break;
	}
	redraw(d);
}

/* ---- Drawing ------------------------------------------------------------ */

static void add_hit(struct dialog *d, int x, int y, int w, int h,
		enum action action, int index) {
	struct hit hit = { x, y, w, h, action, index };
	g_array_append_val(d->hits, hit);
}

static int button_width(cairo_t *cr, const char *label) {
	return (int)gem_text_width(cr, label) + 2 * PAD;
}

static void button(struct dialog *d, cairo_t *cr, int x, int y, int w,
		const char *label, enum action action, int index, int border,
		bool enabled) {
	bool pressed = enabled && d->pressed == action && d->pressed_index == index;
	gem_black(cr);
	if (pressed) {
		gem_fill(cr, x, y, w, BUTTON_H);
		gem_white(cr);
	} else {
		gem_frame(cr, x, y, w, BUTTON_H, border);
	}
	gem_text(cr, label, x + (w - gem_text_width(cr, label)) / 2, y, BUTTON_H);
	if (enabled) {
		add_hit(d, x, y, w, BUTTON_H, action, index);
	} else {
		gem_grey_out(cr, x, y, w, BUTTON_H);
	}
}

/* A GEM radio button: one of a row, the chosen one filled. */
static void radio(struct dialog *d, cairo_t *cr, int x, int y, int w,
		const char *label, enum action action, bool selected, bool enabled) {
	gem_black(cr);
	if (selected) {
		gem_fill(cr, x, y, w, BUTTON_H);
		gem_white(cr);
	} else {
		gem_frame(cr, x, y, w, BUTTON_H, 1);
	}
	gem_text(cr, label, x + (w - gem_text_width(cr, label)) / 2, y, BUTTON_H);
	if (!enabled) {
		gem_grey_out(cr, x, y, w, BUTTON_H);
	} else if (!selected) {
		add_hit(d, x, y, w, BUTTON_H, action, 0);
	}
}

/* [-] n [+]; returns where it ends. */
static int stepper(struct dialog *d, cairo_t *cr, int x, int y, int value,
		enum action down, enum action up, int min, int max, bool enabled) {
	button(d, cr, x, y, STEP_W, "-", down, 0, 1, enabled && value > min);
	char num[12];
	snprintf(num, sizeof(num), "%d", value);
	gem_black(cr);
	gem_text(cr, num, x + STEP_W + (VALUE_W - gem_text_width(cr, num)) / 2, y,
		BUTTON_H);
	if (!enabled) {
		gem_grey_out(cr, x + STEP_W, y, VALUE_W, BUTTON_H);
	}
	button(d, cr, x + STEP_W + VALUE_W, y, STEP_W, "+", up, 0, 1,
		enabled && value < max);
	return x + 2 * STEP_W + VALUE_W;
}

static void check_box(cairo_t *cr, int x, int y, bool checked) {
	gem_black(cr);
	gem_frame(cr, x, y, 12, 12, 1);
	if (checked) {
		for (int i = 2; i < 10; i++) {
			gem_fill(cr, x + i, y + i, 1, 1);
			gem_fill(cr, x + 11 - i, y + i, 1, 1);
		}
	}
}

/* A row of the printer list: its name, and how it is at the right. */
static void list_row(struct dialog *d, cairo_t *cr, int x, int y, int w,
		const char *name, const char *state, bool selected, int index) {
	int sw = (int)gem_text_width(cr, state);
	gem_black(cr);
	if (selected) {
		gem_fill(cr, x, y, w, ROW_H);
		gem_white(cr);
	}
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w - sw - 3 * PAD, ROW_H);
	cairo_clip(cr);
	gem_text(cr, name, x + PAD, y, ROW_H);
	cairo_restore(cr);
	gem_text(cr, state, x + w - PAD - sw, y, ROW_H);
	if (!selected) {
		gem_grey_out(cr, x + w - PAD - sw, y, sw, ROW_H);
	}
	if (index >= 0) {
		add_hit(d, x, y, w, ROW_H, ACT_PRINTER, index);
	}
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	struct dialog *d = data;
	g_array_set_size(d->hits, 0);
	int by = (ROW_H - BUTTON_H) / 2;

	/* The printers. */
	int y = PAD;
	gem_black(cr);
	gem_text(cr, "Printer:", PAD, y, ROW_H);
	y += ROW_H;
	int lx = PAD, lw = w - 2 * PAD, top = y;
	y += 1;
	int shown = MIN((int)d->printers->len, MAX_SHOWN);
	for (int i = 0; i < shown; i++) {
		GtkPrinter *p = d->printers->pdata[i];
		list_row(d, cr, lx + 1, y, lw - 2, gtk_printer_get_name(p),
			printer_state(p), !d->pdf && g_strcmp0(d->printer,
				gtk_printer_get_name(p)) == 0, i);
		y += ROW_H;
	}
	if (d->listing && shown == 0) {
		list_row(d, cr, lx + 1, y, lw - 2, "Looking for printers...", "", false,
			-1);
		gem_grey_out(cr, lx + 1, y, lw - 2, ROW_H);
		y += ROW_H;
	}
	list_row(d, cr, lx + 1, y, lw - 2, "PDF file", "Documents", d->pdf,
		(int)d->printers->len);
	y += ROW_H + 1;
	gem_black(cr);
	gem_frame(cr, lx, top, lw, y - top, 1);
	y += PAD;

	/* Copies and pages: a PDF is one of everything. */
	bool paper = !d->pdf;
	int cx = PAD + LABEL_W;
	gem_black(cr);
	gem_text(cr, "Copies:", PAD, y, ROW_H);
	stepper(d, cr, cx, y + by, d->copies, ACT_COPIES_DOWN, ACT_COPIES_UP, 1,
		MAX_COPIES, paper);
	if (!paper) {
		gem_grey_out(cr, PAD, y, LABEL_W, ROW_H);
	}
	y += ROW_H;
	gem_black(cr);
	gem_text(cr, "Pages:", PAD, y, ROW_H);
	int rw = MAX(button_width(cr, "All"), button_width(cr, "Range"));
	radio(d, cr, cx, y + by, rw, "All", ACT_ALL, !d->range, paper);
	radio(d, cr, cx + rw - 1, y + by, rw, "Range", ACT_RANGE, d->range, paper);
	bool ranged = paper && d->range;
	int x = stepper(d, cr, cx + 2 * rw + PAD, y + by, d->from, ACT_FROM_DOWN,
		ACT_FROM_UP, 1, MAX_PAGE, ranged);
	gem_black(cr);
	gem_text(cr, "to", x + PAD / 2, y, ROW_H);
	int tw = (int)gem_text_width(cr, "to");
	if (!ranged) {
		gem_grey_out(cr, x + PAD / 2, y, tw, ROW_H);
	}
	stepper(d, cr, x + PAD + tw, y + by, d->to, ACT_TO_DOWN, ACT_TO_UP, 1,
		MAX_PAGE, ranged);
	if (!paper) {
		gem_grey_out(cr, PAD, y, LABEL_W, ROW_H);
	}
	y += ROW_H;

	/* The app's own. */
	if (d->n_choices > 0) {
		y += PAD / 2;
	}
	for (int i = 0; i < d->n_choices; i++) {
		check_box(cr, PAD, y + (ROW_H - 12) / 2, d->values[i]);
		gem_text(cr, d->choices[i].label, PAD + 12 + PAD, y, ROW_H);
		add_hit(d, PAD, y, 12 + 2 * PAD + (int)gem_text_width(cr,
			d->choices[i].label), ROW_H, ACT_CHOICE, i);
		y += ROW_H;
	}
	y += PAD;

	/* Cancel and Print. */
	gem_black(cr);
	gem_fill(cr, 0, y, w, 1);
	int bw = MAX(button_width(cr, "Cancel"), button_width(cr, "Print"));
	int fy = y + (BAR_H - BUTTON_H) / 2;
	button(d, cr, w - PAD - bw, fy, bw, d->pdf ? "Save" : "Print", ACT_PRINT, 0,
		2, can_print(d));
	button(d, cr, w - 2 * PAD - 2 * bw, fy, bw, "Cancel", ACT_CANCEL, 0, 1,
		true);

	int need = y + BAR_H;
	if (need != d->height) {
		d->height = need;
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(d->area), need);
	}
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	gem_draw_pixelated(cr, w, h, paint, data);
}

/* ---- Input -------------------------------------------------------------- */

static struct hit *hit_at(struct dialog *d, double x, double y) {
	for (guint i = 0; i < d->hits->len; i++) {
		struct hit *hit = &g_array_index(d->hits, struct hit, i);
		if (x >= hit->x && x < hit->x + hit->w && y >= hit->y &&
				y < hit->y + hit->h) {
			return hit;
		}
	}
	return NULL;
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y,
		struct dialog *d) {
	struct hit *hit = hit_at(d, x, y);
	d->pressed = hit != NULL ? hit->action : ACT_NONE;
	d->pressed_index = hit != NULL ? hit->index : 0;
	/* The list and the check boxes act at once; buttons when let go. */
	if (d->pressed == ACT_PRINTER || d->pressed == ACT_CHOICE) {
		d->pressed = ACT_NONE;
		act(d, hit->action, hit->index);
		return;
	}
	redraw(d);
}

static void released(GtkGestureClick *gesture, int n, double x, double y,
		struct dialog *d) {
	struct hit *hit = hit_at(d, x, y);
	enum action action = d->pressed;
	int index = d->pressed_index;
	d->pressed = ACT_NONE;
	if (hit != NULL && action != ACT_NONE && hit->action == action &&
			hit->index == index) {
		act(d, action, index); /* may close the dialog */
		return;
	}
	redraw(d);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
		guint code, GdkModifierType mods, struct dialog *d) {
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		act(d, ACT_PRINT, 0);
	} else if (keyval == GDK_KEY_Escape) {
		act(d, ACT_CANCEL, 0);
	} else if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
		/* Through the list: the printers, then PDF file. */
		int n = MIN((int)d->printers->len, MAX_SHOWN), at = n;
		for (int i = 0; i < n && !d->pdf; i++) {
			if (g_strcmp0(d->printer,
					gtk_printer_get_name(d->printers->pdata[i])) == 0) {
				at = i;
			}
		}
		at += keyval == GDK_KEY_Up ? -1 : 1;
		if (at >= 0 && at <= n) {
			act(d, ACT_PRINTER, at == n ? (int)d->printers->len : at);
		}
	} else {
		return FALSE;
	}
	return TRUE;
}

static gboolean close_request(GtkWindow *window, struct dialog *d) {
	cancel(d);
	return TRUE;
}

void gem_print_setup(void) {
	const char *flags = g_getenv("GDK_DEBUG");
	if (flags == NULL || flags[0] == '\0') {
		g_setenv("GDK_DEBUG", "no-portals", TRUE);
	} else if (strstr(flags, "no-portals") == NULL) {
		char *more = g_strconcat(flags, ",no-portals", NULL);
		g_setenv("GDK_DEBUG", more, TRUE);
		g_free(more);
	}
}

void gem_print_dialog(GtkWindow *parent, GtkPrintOperation *op,
		const struct gem_print_choice *choices, int n_choices,
		void (*done)(const char *message, void *data), void *data) {
	struct dialog *d = g_new0(struct dialog, 1);
	d->refs = 2; /* the dialog, and the printer listing */
	d->op = op;
	d->done = done;
	d->data = data;
	d->printers = g_ptr_array_new_with_free_func(g_object_unref);
	d->hits = g_array_new(FALSE, TRUE, sizeof(struct hit));
	d->listing = true;
	d->copies = d->from = d->to = 1;
	d->pdf = last_pdf;
	d->rank = last_pdf ? RANK_LAST_USED : RANK_NONE;
	d->n_choices = n_choices;
	d->choices = g_memdup2(choices, sizeof(*choices) * n_choices);
	d->values = g_new0(bool, MAX(n_choices, 1));
	for (int i = 0; i < n_choices; i++) {
		d->values[i] = *choices[i].value;
	}
	if (last_values != NULL) {
		memcpy(d->values, last_values, sizeof(bool) * n_choices);
	}

	d->window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(d->window), "Print");
	gtk_window_set_transient_for(GTK_WINDOW(d->window), parent);
	/* The app's, so the menu bar names it. */
	gtk_window_set_application(GTK_WINDOW(d->window),
		gtk_window_get_application(parent));
	gtk_window_set_modal(GTK_WINDOW(d->window), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(d->window), FALSE);
	d->area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(d->area), WIDTH);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(d->area), 240);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->area), draw, d, NULL);
	gtk_window_set_child(GTK_WINDOW(d->window), d->area);

	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), d);
	g_signal_connect(click, "released", G_CALLBACK(released), d);
	gtk_widget_add_controller(d->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), d);
	gtk_widget_add_controller(d->window, keys);
	g_signal_connect(d->window, "close-request", G_CALLBACK(close_request), d);

	gtk_enumerate_printers(found, d, listed, FALSE);
	gtk_window_present(GTK_WINDOW(d->window));
}
