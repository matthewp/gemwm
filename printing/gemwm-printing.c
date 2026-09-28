/*
 * gemwm-printing: what's printing, GEM style.
 *
 *   gemwm-printing              the window: what's printing (with Cancel),
 *                               what finished lately, and how the printers
 *                               are
 *   gemwm-printing --menu-app   the menu bar item (see menu/menu.c): a
 *                               printer while something's printing, with
 *                               what's wrong in inverse if the printer
 *                               needs you (out of paper, paused); it stays
 *                               half a minute after the last job finishes,
 *                               then goes. A click opens the window.
 *
 * It all comes from CUPS, so it's every app's printing, not just GemWM's.
 * CUPS says on the system bus when a job is queued or a printer changes;
 * while anything is printing it's asked every couple of seconds as well,
 * and otherwise not at all.
 */
#include <cups/cups.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app-menu.h"
#include "gem-draw.h"

#define PAD 8
#define BAR_H 30      /* the bottom row: what happened */
#define ROW_H 24
#define BUTTON_H 18
#define WIDTH 520
#define RECENT 5      /* finished jobs listed */
#define POLL 2        /* seconds between askings while printing */
#define LINGER 30     /* seconds the item stays after the last job */
#define SETTLE 300    /* ms to let a burst of CUPS's signals pass */

/* ---- CUPS --------------------------------------------------------------- */

struct job {
	int id;
	char *title, *printer;
	ipp_jstate_t state;
	time_t created, completed;
	int pages;             /* printed so far, if the printer says */
	bool held;
};

struct printer {
	char *name;
	ipp_pstate_t state;
	char *message;         /* printer-state-message */
	char **reasons;        /* printer-state-reasons */
	bool accepting;
};

static void job_free(void *data) {
	struct job *j = data;
	g_free(j->title);
	g_free(j->printer);
	g_free(j);
}

static void printer_free(void *data) {
	struct printer *p = data;
	g_free(p->name);
	g_free(p->message);
	g_strfreev(p->reasons);
	g_free(p);
}

static struct {
	GPtrArray *active;     /* struct job, oldest first */
	GPtrArray *recent;     /* struct job, newest first */
	GPtrArray *printers;   /* struct printer */
	bool reached;          /* CUPS answered, last time */
	guint poll, settle;
	void (*changed)(void);
} model;

static const char *const job_attrs[] = {
	"job-id", "job-name", "job-state", "job-printer-uri", "time-at-creation",
	"time-at-completed", "job-impressions-completed", "job-state-reasons",
};

static const char *const printer_attrs[] = {
	"printer-name", "printer-state", "printer-state-message",
	"printer-state-reasons", "printer-is-accepting-jobs",
};

static http_t *cups_connect(void) {
	return httpConnect2(cupsServer(), ippPort(), NULL, AF_UNSPEC,
		cupsEncryption(), 1, 5000, NULL);
}

static char **strings(ipp_attribute_t *a) {
	int n = ippGetCount(a);
	char **v = g_new0(char *, n + 1);
	for (int i = 0; i < n; i++) {
		v[i] = g_strdup(ippGetString(a, i, NULL));
	}
	return v;
}

/* Your jobs, not-completed or completed. */
static GPtrArray *get_jobs(http_t *http, const char *which) {
	ipp_t *req = ippNewRequest(IPP_OP_GET_JOBS);
	ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL,
		"ipp://localhost/");
	ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name",
		NULL, cupsUser());
	ippAddBoolean(req, IPP_TAG_OPERATION, "my-jobs", 1);
	ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", NULL,
		which);
	ippAddStrings(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		"requested-attributes", G_N_ELEMENTS(job_attrs), NULL, job_attrs);
	ipp_t *resp = cupsDoRequest(http, req, "/");
	GPtrArray *jobs = g_ptr_array_new_with_free_func(job_free);
	if (resp == NULL) {
		return jobs;
	}
	ipp_attribute_t *a = ippFirstAttribute(resp);
	while (a != NULL) {
		while (a != NULL && ippGetGroupTag(a) != IPP_TAG_JOB) {
			a = ippNextAttribute(resp);
		}
		if (a == NULL) {
			break;
		}
		struct job *j = g_new0(struct job, 1);
		for (; a != NULL && ippGetGroupTag(a) == IPP_TAG_JOB;
				a = ippNextAttribute(resp)) {
			const char *name = ippGetName(a);
			bool integer = ippGetValueTag(a) == IPP_TAG_INTEGER ||
				ippGetValueTag(a) == IPP_TAG_ENUM;
			if (name == NULL) {
				continue;
			} else if (strcmp(name, "job-id") == 0 && integer) {
				j->id = ippGetInteger(a, 0);
			} else if (strcmp(name, "job-name") == 0) {
				j->title = g_strdup(ippGetString(a, 0, NULL));
			} else if (strcmp(name, "job-state") == 0 && integer) {
				j->state = (ipp_jstate_t)ippGetInteger(a, 0);
			} else if (strcmp(name, "job-printer-uri") == 0) {
				const char *uri = ippGetString(a, 0, NULL);
				const char *slash = uri != NULL ? strrchr(uri, '/') : NULL;
				j->printer = g_strdup(slash != NULL ? slash + 1 : uri);
			} else if (strcmp(name, "time-at-creation") == 0 && integer) {
				j->created = ippGetInteger(a, 0);
			} else if (strcmp(name, "time-at-completed") == 0 && integer) {
				j->completed = ippGetInteger(a, 0);
			} else if (strcmp(name, "job-impressions-completed") == 0 &&
					integer) {
				j->pages = ippGetInteger(a, 0);
			} else if (strcmp(name, "job-state-reasons") == 0) {
				for (int i = 0; i < ippGetCount(a); i++) {
					const char *r = ippGetString(a, i, NULL);
					j->held |= r != NULL && strstr(r, "hold") != NULL;
				}
			}
		}
		if (j->title == NULL || j->title[0] == '\0') {
			g_free(j->title);
			j->title = g_strdup("Untitled");
		}
		if (j->printer == NULL) {
			j->printer = g_strdup("?");
		}
		g_ptr_array_add(jobs, j);
	}
	ippDelete(resp);
	return jobs;
}

static GPtrArray *get_printers(http_t *http) {
	ipp_t *req = ippNewRequest(IPP_OP_CUPS_GET_PRINTERS);
	ippAddString(req, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name",
		NULL, cupsUser());
	ippAddStrings(req, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		"requested-attributes", G_N_ELEMENTS(printer_attrs), NULL,
		printer_attrs);
	ipp_t *resp = cupsDoRequest(http, req, "/");
	GPtrArray *printers = g_ptr_array_new_with_free_func(printer_free);
	if (resp == NULL) {
		return printers;
	}
	ipp_attribute_t *a = ippFirstAttribute(resp);
	while (a != NULL) {
		while (a != NULL && ippGetGroupTag(a) != IPP_TAG_PRINTER) {
			a = ippNextAttribute(resp);
		}
		if (a == NULL) {
			break;
		}
		struct printer *p = g_new0(struct printer, 1);
		p->accepting = true;
		for (; a != NULL && ippGetGroupTag(a) == IPP_TAG_PRINTER;
				a = ippNextAttribute(resp)) {
			const char *name = ippGetName(a);
			if (name == NULL) {
				continue;
			} else if (strcmp(name, "printer-name") == 0) {
				p->name = g_strdup(ippGetString(a, 0, NULL));
			} else if (strcmp(name, "printer-state") == 0) {
				p->state = (ipp_pstate_t)ippGetInteger(a, 0);
			} else if (strcmp(name, "printer-state-message") == 0) {
				p->message = g_strdup(ippGetString(a, 0, NULL));
			} else if (strcmp(name, "printer-state-reasons") == 0) {
				p->reasons = strings(a);
			} else if (strcmp(name, "printer-is-accepting-jobs") == 0) {
				p->accepting = ippGetBoolean(a, 0);
			}
		}
		if (p->name != NULL) {
			g_ptr_array_add(printers, p);
		} else {
			printer_free(p);
		}
	}
	ippDelete(resp);
	return printers;
}

static int newest_first(const void *a, const void *b) {
	const struct job *x = *(struct job *const *)a, *y = *(struct job *const *)b;
	time_t tx = x->completed ? x->completed : x->created;
	time_t ty = y->completed ? y->completed : y->created;
	return tx < ty ? 1 : tx > ty ? -1 : y->id - x->id;
}

static void schedule(void);

static void refresh(void) {
	GPtrArray *active = NULL, *recent = NULL, *printers = NULL;
	http_t *http = cups_connect();
	model.reached = http != NULL;
	if (http != NULL) {
		active = get_jobs(http, "not-completed");
		recent = get_jobs(http, "completed");
		printers = get_printers(http);
		httpClose(http);
		g_ptr_array_sort(recent, newest_first);
		if (recent->len > RECENT) {
			g_ptr_array_set_size(recent, RECENT);
		}
	}
	g_clear_pointer(&model.active, g_ptr_array_unref);
	g_clear_pointer(&model.recent, g_ptr_array_unref);
	g_clear_pointer(&model.printers, g_ptr_array_unref);
	model.active = active ? active : g_ptr_array_new_with_free_func(job_free);
	model.recent = recent ? recent : g_ptr_array_new_with_free_func(job_free);
	model.printers = printers ? printers :
		g_ptr_array_new_with_free_func(printer_free);
	schedule();
	model.changed();
}

static gboolean poll_tick(void *data) {
	refresh();
	return G_SOURCE_CONTINUE;
}

/* While anything's printing, ask again every few seconds: CUPS doesn't say
 * when a page is done, or always when a job is. */
static void schedule(void) {
	bool printing = model.active->len > 0;
	if (printing && model.poll == 0) {
		model.poll = g_timeout_add_seconds(POLL, poll_tick, NULL);
	} else if (!printing && model.poll != 0) {
		g_source_remove(model.poll);
		model.poll = 0;
	}
}

static gboolean settled(void *data) {
	model.settle = 0;
	refresh();
	return G_SOURCE_REMOVE;
}

/* JobQueuedLocal, QueueChanged and the like: CUPS's own, on the system
 * bus. They come in bursts, so they're answered once, shortly after. */
static void cups_signal(GDBusConnection *bus, const char *sender,
		const char *path, const char *iface, const char *name,
		GVariant *params, void *data) {
	if (model.settle == 0) {
		model.settle = g_timeout_add(SETTLE, settled, NULL);
	}
}

static void cups_watch(void (*changed)(void)) {
	model.changed = changed;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (bus != NULL) {
		static const char *const ifaces[] = { "com.redhat.PrinterSpooler",
			"org.cups.cupsd.Notifier" };
		for (size_t i = 0; i < G_N_ELEMENTS(ifaces); i++) {
			g_dbus_connection_signal_subscribe(bus, NULL, ifaces[i], NULL, NULL,
				NULL, G_DBUS_SIGNAL_FLAGS_NONE, cups_signal, NULL, NULL);
		}
	}
	refresh();
}

static struct printer *printer_named(const char *name) {
	for (guint i = 0; model.printers != NULL && i < model.printers->len; i++) {
		struct printer *p = model.printers->pdata[i];
		if (g_strcmp0(p->name, name) == 0) {
			return p;
		}
	}
	return NULL;
}

/* What a printer's reasons say needs doing, in a few words; NULL if it's
 * fine. -report reasons are news, not trouble; -warning ones (toner low)
 * can wait. */
static const char *trouble(const struct printer *p) {
	if (p == NULL) {
		return NULL;
	}
	static const struct {
		const char *reason, *words;
	} known[] = {
		{ "media-empty", "Out of paper" },
		{ "media-needed", "Out of paper" },
		{ "media-jam", "Paper jam" },
		{ "toner-empty", "Out of toner" },
		{ "marker-supply-empty", "Out of ink" },
		{ "door-open", "Door open" },
		{ "cover-open", "Cover open" },
		{ "input-tray-missing", "Tray missing" },
		{ "offline", "Offline" },
		{ "shutdown", "Offline" },
	};
	for (int i = 0; p->reasons != NULL && p->reasons[i] != NULL; i++) {
		const char *r = p->reasons[i];
		if (g_str_has_suffix(r, "-report") && !g_str_has_prefix(r, "offline")) {
			continue;
		}
		if (g_str_has_suffix(r, "-warning")) {
			continue;
		}
		for (size_t k = 0; k < G_N_ELEMENTS(known); k++) {
			if (g_str_has_prefix(r, known[k].reason)) {
				return known[k].words;
			}
		}
		if (g_str_has_suffix(r, "-error")) {
			return "Needs attention";
		}
	}
	if (p->state == IPP_PSTATE_STOPPED) {
		return "Paused";
	}
	return NULL;
}

/* A job's state, in words. */
static char *job_state(const struct job *j) {
	switch (j->state) {
	case IPP_JSTATE_PENDING:
		return g_strdup("Waiting");
	case IPP_JSTATE_HELD:
		return g_strdup("Held");
	case IPP_JSTATE_PROCESSING:
		return j->pages > 0 ? g_strdup_printf("Printing page %d", j->pages + 1) :
			g_strdup("Printing");
	case IPP_JSTATE_STOPPED:
		return g_strdup("Stopped");
	case IPP_JSTATE_CANCELED:
		return g_strdup("Cancelled");
	case IPP_JSTATE_ABORTED:
		return g_strdup("Failed");
	case IPP_JSTATE_COMPLETED:
		return g_strdup("Printed");
	}
	return g_strdup("?");
}

/* The time, as the clock would say it: today's as the time, this week's
 * with the day, older ones as the date. */
static char *when(time_t t) {
	GDateTime *dt = g_date_time_new_from_unix_local(t);
	GDateTime *now = g_date_time_new_now_local();
	char *ampm = g_date_time_format(dt, "%p");
	const char *time = ampm != NULL && ampm[0] ? "%-l:%M %p" : "%H:%M";
	char *format;
	GTimeSpan age = g_date_time_difference(now, dt);
	if (g_date_time_get_year(dt) == g_date_time_get_year(now) &&
			g_date_time_get_day_of_year(dt) == g_date_time_get_day_of_year(now)) {
		format = g_strdup(time);
	} else if (age < 6 * G_TIME_SPAN_DAY) {
		format = g_strdup_printf("%%a %s", time);
	} else {
		format = g_strdup("%-d %b");
	}
	char *s = g_date_time_format(dt, format);
	g_free(format);
	g_free(ampm);
	g_date_time_unref(dt);
	g_date_time_unref(now);
	return s;
}

/* ---- The icon ----------------------------------------------------------- */

/* A printer: paper going in at the top, coming out at the bottom. */
static const char *const icon_rows[] = {
	"...#######...",
	"...#.....#...",
	".###########.",
	"#...........#",
	"#........##.#",
	"#...........#",
	"#############",
	"..#.......#..",
	"..#.......#..",
	"..#########..",
};
enum { ICON_W = 13, ICON_H = G_N_ELEMENTS(icon_rows) };

static void icon_bitmap(GString *out) {
	g_string_append_printf(out, "bitmap:%dx%d:", ICON_W, ICON_H);
	for (int y = 0; y < ICON_H; y++) {
		for (int x = 0; x < ICON_W; x += 8) {
			unsigned byte = 0;
			for (int b = 0; b < 8 && x + b < ICON_W; b++) {
				byte |= icon_rows[y][x + b] == '#' ? 0x80u >> b : 0;
			}
			g_string_append_printf(out, "%02x", byte);
		}
	}
}

/* ---- The menu bar item -------------------------------------------------- */

static char *self;

/* The last job to finish while we watched, for the half minute after. */
static struct {
	GArray *watching;      /* ids of the jobs printing last time */
	char *title;
	ipp_jstate_t state;
	gint64 until;          /* monotonic, µs */
	guint timer;
} done;

static void status_print(void);

static gboolean linger_over(void *data) {
	done.timer = 0;
	status_print();
	return G_SOURCE_REMOVE;
}

/* Jobs that were printing and aren't now have finished: the newest one
 * (as CUPS tells it) is what the item says for a while. */
static void notice_finished(void) {
	bool gone = false;
	for (guint i = 0; i < done.watching->len; i++) {
		int id = g_array_index(done.watching, int, i);
		bool still = false;
		for (guint k = 0; k < model.active->len && !still; k++) {
			still = ((struct job *)model.active->pdata[k])->id == id;
		}
		if (still) {
			continue;
		}
		for (guint k = 0; k < model.recent->len; k++) {
			struct job *j = model.recent->pdata[k];
			if (j->id == id) {
				g_free(done.title);
				done.title = g_strdup(j->title);
				done.state = j->state;
				gone = true;
				break;
			}
		}
	}
	g_array_set_size(done.watching, 0);
	for (guint k = 0; k < model.active->len; k++) {
		g_array_append_val(done.watching,
			((struct job *)model.active->pdata[k])->id);
	}
	if (gone && model.active->len == 0) {
		done.until = g_get_monotonic_time() + LINGER * G_TIME_SPAN_SECOND;
		if (done.timer != 0) {
			g_source_remove(done.timer);
		}
		done.timer = g_timeout_add_seconds(LINGER, linger_over, NULL);
	}
}

static void status_print(void) {
	static char *last;
	GString *line = g_string_new(NULL);
	if (model.active->len > 0) {
		/* What's wrong with a printer being printed to, if anything. */
		const char *wrong = NULL;
		const char *where = NULL;
		for (guint i = 0; i < model.active->len && wrong == NULL; i++) {
			struct job *j = model.active->pdata[i];
			wrong = trouble(printer_named(j->printer));
			where = j->printer;
		}
		struct job *first = model.active->pdata[0];
		char *tip;
		if (wrong != NULL) {
			tip = g_strdup_printf("%s: %s", where, wrong);
		} else if (model.active->len > 1) {
			tip = g_strdup_printf("Printing %u jobs", model.active->len);
		} else {
			tip = g_strdup_printf("%s %s", first->state ==
				IPP_JSTATE_PROCESSING ? "Printing" : "Waiting to print",
				first->title);
		}
		icon_bitmap(line);
		g_string_append_printf(line, "\t%s\t%s\t%s", wrong ? wrong : "",
			wrong ? "inverse" : "", g_strdelimit(tip, "\t\n", ' '));
		g_free(tip);
	} else if (done.title != NULL && g_get_monotonic_time() < done.until) {
		bool failed = done.state == IPP_JSTATE_ABORTED;
		char *tip = g_strdup_printf("%s %s", failed ? "Couldn't print" :
			done.state == IPP_JSTATE_CANCELED ? "Cancelled" : "Printed",
			done.title);
		icon_bitmap(line);
		g_string_append_printf(line, "\t%s\t%s\t%s", failed ? "Failed" : "",
			failed ? "inverse" : "", g_strdelimit(tip, "\t\n", ' '));
		g_free(tip);
	}
	/* Nothing: an empty line, and the item goes. */
	if (g_strcmp0(line->str, last) != 0) {
		printf("%s\n", line->str);
		fflush(stdout);
		g_free(last);
		last = g_strdup(line->str);
	}
	g_string_free(line, TRUE);
}

static void menu_app_changed(void) {
	notice_finished();
	status_print();
}

static void read_bar(GObject *source, GAsyncResult *result, void *data) {
	GMainLoop *loop = data;
	char *line = g_data_input_stream_read_line_finish(
		G_DATA_INPUT_STREAM(source), result, NULL, NULL);
	if (line == NULL) {
		g_main_loop_quit(loop); /* the bar has gone */
		return;
	}
	if (strcmp(line, "click 1") == 0) {
		/* Single instance: a second click brings the window forward. */
		char *argv[] = { self, NULL };
		g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			NULL);
	}
	g_free(line);
	g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(source),
		G_PRIORITY_DEFAULT, NULL, read_bar, loop);
}

static int menu_app(void) {
	done.watching = g_array_new(FALSE, FALSE, sizeof(int));
	/* Jobs already done before we started aren't news. */
	cups_watch(status_print);
	for (guint k = 0; k < model.active->len; k++) {
		g_array_append_val(done.watching,
			((struct job *)model.active->pdata[k])->id);
	}
	model.changed = menu_app_changed;
	status_print();
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GInputStream *in = g_unix_input_stream_new(0, FALSE);
	GDataInputStream *lines = g_data_input_stream_new(in);
	g_data_input_stream_read_line_async(lines, G_PRIORITY_DEFAULT, NULL,
		read_bar, loop);
	g_main_loop_run(loop);
	return 0;
}

/* ---- The window --------------------------------------------------------- */

enum action {
	ACT_NONE,
	ACT_CANCEL_JOB,
	ACT_CLOSE,
};

struct hit {
	int x, y, w, h;
	enum action action;
	int job;
};

static struct {
	GtkWidget *window, *area;
	struct app_menu *menu;
	GArray *hits;
	enum action pressed;
	int pressed_job;
	char *message;
	int height;
} ui;

static void set_message(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void set_message(const char *format, ...) {
	g_free(ui.message);
	va_list args;
	va_start(args, format);
	ui.message = g_strdup_vprintf(format, args);
	va_end(args);
	gtk_widget_queue_draw(ui.area);
}

static struct job *active_job(int id) {
	for (guint i = 0; i < model.active->len; i++) {
		struct job *j = model.active->pdata[i];
		if (j->id == id) {
			return j;
		}
	}
	return NULL;
}

static void cancel_job(int id) {
	struct job *j = active_job(id);
	if (j == NULL) {
		return;
	}
	http_t *http = cups_connect();
	ipp_status_t status = http != NULL ?
		cupsCancelJob2(http, j->printer, id, 0) : IPP_STATUS_ERROR_SERVICE_UNAVAILABLE;
	if (status >= IPP_STATUS_ERROR_BAD_REQUEST) {
		set_message("Couldn't cancel %s: %s", j->title, cupsLastErrorString());
	} else {
		set_message("Cancelled %s.", j->title);
	}
	if (http != NULL) {
		httpClose(http);
	}
	refresh();
}

static void act(enum action action, int job) {
	switch (action) {
	case ACT_CANCEL_JOB:
		cancel_job(job);
		break;
	case ACT_CLOSE:
		gtk_window_close(GTK_WINDOW(ui.window));
		break;
	default:
		break;
	}
	gtk_widget_queue_draw(ui.area);
}

static void add_hit(int x, int y, int w, int h, enum action action, int job) {
	struct hit hit = { x, y, w, h, action, job };
	g_array_append_val(ui.hits, hit);
}

static int button_width(cairo_t *cr, const char *label) {
	return (int)gem_text_width(cr, label) + 2 * PAD;
}

static void button(cairo_t *cr, int x, int y, int w, const char *label,
		enum action action, int job) {
	bool pressed = ui.pressed == action && ui.pressed_job == job;
	gem_black(cr);
	if (pressed) {
		gem_fill(cr, x, y, w, BUTTON_H);
		gem_white(cr);
	} else {
		gem_frame(cr, x, y, w, BUTTON_H, 1);
	}
	gem_text(cr, label, x + (w - gem_text_width(cr, label)) / 2, y, BUTTON_H);
	add_hit(x, y, w, BUTTON_H, action, job);
}

/* Text cut off at w. */
static void clipped(cairo_t *cr, const char *s, int x, int y, int w) {
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, ROW_H);
	cairo_clip(cr);
	gem_text(cr, s, x, y, ROW_H);
	cairo_restore(cr);
}

/* Text on black, to catch the eye, as in the menu bar. */
static void inverse(cairo_t *cr, const char *s, int x, int y) {
	int w = (int)gem_text_width(cr, s);
	gem_black(cr);
	gem_fill(cr, x - 2, y + 3, w + 4, ROW_H - 6);
	gem_white(cr);
	gem_text(cr, s, x, y, ROW_H);
	gem_black(cr);
}

static int heading(cairo_t *cr, const char *s, int y, int w) {
	gem_black(cr);
	gem_text(cr, s, PAD, y, ROW_H);
	gem_fill(cr, PAD, y + ROW_H - 3, w - 2 * PAD, 1);
	return y + ROW_H;
}

static int note(cairo_t *cr, const char *s, int y, int w) {
	gem_black(cr);
	gem_text(cr, s, 2 * PAD, y, ROW_H);
	gem_grey_out(cr, 0, y, w, ROW_H);
	return y + ROW_H;
}

/* Title, printer, then how it is: the columns of a job's row. */
enum { COL_TITLE = 2 * PAD, COL_PRINTER = 200, COL_STATE = 310 };

static int job_row(cairo_t *cr, const struct job *j, bool active, int y,
		int w) {
	gem_black(cr);
	clipped(cr, j->title, COL_TITLE, y, COL_PRINTER - COL_TITLE - PAD);
	clipped(cr, j->printer, COL_PRINTER, y, COL_STATE - COL_PRINTER - PAD);
	char *state = job_state(j);
	int end = w - PAD;
	if (active) {
		int bw = button_width(cr, "Cancel");
		button(cr, end - bw, y + (ROW_H - BUTTON_H) / 2, bw, "Cancel",
			ACT_CANCEL_JOB, j->id);
		end -= bw + PAD;
		const char *wrong = trouble(printer_named(j->printer));
		if (wrong != NULL && j->state != IPP_JSTATE_HELD) {
			g_free(state);
			state = g_strdup(wrong);
		}
		gem_black(cr);
		if (wrong != NULL) {
			inverse(cr, state, COL_STATE, y);
		} else {
			clipped(cr, state, COL_STATE, y, end - COL_STATE);
		}
	} else {
		char *t = when(j->completed ? j->completed : j->created);
		char *s = g_strdup_printf("%s %s", state, t);
		clipped(cr, s, COL_STATE, y, end - COL_STATE);
		g_free(s);
		g_free(t);
	}
	g_free(state);
	return y + ROW_H;
}

static int printer_row(cairo_t *cr, const struct printer *p, int y, int w) {
	gem_black(cr);
	clipped(cr, p->name, COL_TITLE, y, COL_STATE - COL_TITLE - PAD);
	const char *wrong = trouble(p);
	if (wrong != NULL) {
		inverse(cr, wrong, COL_STATE, y);
	} else {
		const char *state = p->state == IPP_PSTATE_PROCESSING ? "Printing" :
			!p->accepting ? "Not accepting jobs" : "Ready";
		clipped(cr, state, COL_STATE, y, w - PAD - COL_STATE);
	}
	return y + ROW_H;
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	g_array_set_size(ui.hits, 0);
	int y = PAD / 2;
	if (!model.reached) {
		y = note(cr, "Can't reach the print service (CUPS).", y, w);
	} else {
		y = heading(cr, "Printing", y, w);
		if (model.active->len == 0) {
			y = note(cr, "Nothing is printing.", y, w);
		}
		for (guint i = 0; i < model.active->len; i++) {
			y = job_row(cr, model.active->pdata[i], true, y, w);
		}
		y += PAD;
		y = heading(cr, "Recently finished", y, w);
		if (model.recent->len == 0) {
			y = note(cr, "Nothing yet.", y, w);
		}
		for (guint i = 0; i < model.recent->len; i++) {
			y = job_row(cr, model.recent->pdata[i], false, y, w);
		}
		y += PAD;
		y = heading(cr, "Printers", y, w);
		if (model.printers->len == 0) {
			y = note(cr, "No printers set up.", y, w);
		}
		for (guint i = 0; i < model.printers->len; i++) {
			y = printer_row(cr, model.printers->pdata[i], y, w);
		}
	}
	y += PAD / 2;

	/* Bottom: what happened. */
	int foot = MAX(y, h - BAR_H);
	gem_black(cr);
	gem_fill(cr, 0, foot, w, 1);
	clipped(cr, ui.message ? ui.message : "", PAD, foot + (BAR_H - ROW_H) / 2,
		w - 2 * PAD);

	int need = y + BAR_H;
	if (need != ui.height) {
		ui.height = need;
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), need);
	}
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	gem_draw_pixelated(cr, w, h, paint, NULL);
}

static struct hit *hit_at(double x, double y) {
	for (guint i = 0; i < ui.hits->len; i++) {
		struct hit *hit = &g_array_index(ui.hits, struct hit, i);
		if (x >= hit->x && x < hit->x + hit->w && y >= hit->y &&
				y < hit->y + hit->h) {
			return hit;
		}
	}
	return NULL;
}

static void pressed(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	struct hit *hit = hit_at(x, y);
	ui.pressed = hit != NULL ? hit->action : ACT_NONE;
	ui.pressed_job = hit != NULL ? hit->job : 0;
	gtk_widget_queue_draw(ui.area);
}

/* As in GEM, a button acts when released over it. */
static void released(GtkGestureClick *gesture, int n, double x, double y,
		void *data) {
	struct hit *hit = hit_at(x, y);
	enum action action = ui.pressed;
	int job = ui.pressed_job;
	ui.pressed = ACT_NONE;
	if (hit != NULL && hit->action == action && hit->job == job) {
		act(action, job);
	}
	gtk_widget_queue_draw(ui.area);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
		guint code, GdkModifierType mods, void *data) {
	if (keyval == GDK_KEY_Escape || ((mods & GDK_CONTROL_MASK) &&
			(keyval == GDK_KEY_w || keyval == GDK_KEY_W))) {
		act(ACT_CLOSE, 0);
		return TRUE;
	}
	return FALSE;
}

static void build_menus(struct app_menu *m, void *data) {
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);
}

static void menu_activate(uint32_t id, void *data) {
	act((enum action)id, 0);
}

static void window_changed(void) {
	gtk_widget_queue_draw(ui.area);
}

static void activate(GtkApplication *app, void *data) {
	if (ui.window != NULL) {
		refresh();
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	ui.hits = g_array_new(FALSE, TRUE, sizeof(struct hit));
	ui.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ui.window), "Printing");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), WIDTH, -1);
	ui.area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), 200);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw, NULL, NULL);
	gtk_window_set_child(GTK_WINDOW(ui.window), ui.area);

	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
	g_signal_connect(click, "released", G_CALLBACK(released), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);

	cups_watch(window_changed);
	gtk_window_present(GTK_WINDOW(ui.window));
}

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "--menu-app") == 0) {
		self = strchr(argv[0], '/') != NULL ? g_strdup(argv[0]) :
			g_find_program_in_path(argv[0]);
		return menu_app();
	}
	GtkApplication *app = gtk_application_new("org.gemwm.Printing",
		G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
