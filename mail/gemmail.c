/*
 * GemMail: an IMAP mail reader and writer, drawn like GEM.
 *
 *   folders   | From          Subject                     Date |
 *   Inbox (3) | ◆ Ann Example Lunch?                     08:15 |
 *   Sent      |   Bob         Re: the report             Sep 28|
 *   Trash     |                                                |
 *   info line: 12 messages, 3 unread
 *
 * Folders on the left; the main pane lists a folder's messages, and
 * opening one shows it in that same pane (Back returns to the list), its
 * body in a WebKit view that runs no scripts and loads nothing remote
 * until asked. Mail comes and goes over IMAP and SMTP in a thread of its
 * own (imap.c); messages are taken apart and put together with GMime
 * (message.c). The account is in ~/.config/gemmail/settings (account.h).
 */
#include <gmime/gmime.h>
#include <gtk/gtk.h>
#include <string.h>
#include <webkit/webkit.h>
#include "account.h"
#include "app-menu.h"
#include "cache.h"
#include "gem-draw.h"
#include "imap.h"
#include "message.h"
#include "passwords.h"

#define ROW_H 19
#define FOLDERS_W 200
#define GADGET 19       /* a scroll bar's width, its arrow boxes */
#define PAD 8
#define BUTTON_H 22
#define INFO_H 20       /* the info line, with its rule */
#define FIELD_H 24
#define LIMIT 1000      /* messages listed (and kept) per folder */
#define AHEAD 100       /* the newest fetched ahead, to read offline */
#define AHEAD_SIZE (2 * 1024 * 1024) /* but none bigger */
#define REFRESH 180     /* seconds between checks for new mail */

enum action {
	ACT_NONE, ACT_BACK, ACT_REPLY, ACT_REPLY_ALL, ACT_FORWARD, ACT_DELETE,
	ACT_UNREAD, ACT_SHOW_IMAGES, ACT_ALWAYS_IMAGES, ACT_STOP_IMAGES,
	ACT_ATTACHMENT, ACT_NEW, ACT_GET_MAIL,
	ACT_ARCHIVE, ACT_SELECT_ALL, ACT_MOVE, ACT_MOVE_POPUP, ACT_MOVE_OK,
	ACT_MOVE_CANCEL,
	ACT_QUIT, ACT_OPEN,
	ACT_SEND, ACT_CANCEL, ACT_PASSWORD_OK, ACT_PASSWORD_CANCEL,
};

struct hit {
	int x, y, w, h;
	enum action action;
	int index;
};

struct key {
	const char *trigger;
	enum action action;
};

static struct {
	GtkApplication *app;
	GtkWidget *window, *folders, *stack, *list, *scroll, *header, *view, *info;
	struct app_menu *menu;
	struct account *account;
	char *account_error;
	struct mail *mail;
	struct cache *cache;

	GPtrArray *folder_list; /* struct folder */
	int folder;             /* selected, or -1 */
	GPtrArray *messages;    /* struct summary */
	int selected;           /* in messages, or -1: where the keys are */
	int anchor;             /* where a Shift range starts */
	GHashTable *picked;     /* the messages selected, by UID */
	int pending_pick;       /* pressed on a selected one: picked alone on
	                         * release, unless it's dragged */
	int drop_folder;        /* the folder messages are dragged over, or -1 */
	int drag_row;           /* where a drag in the list started */
	bool dragging_messages;
	double ghost_x, ghost_y; /* the pointer, dragging, in the ghost's space */
	GtkWidget *ghost;       /* what's dragged, drawn over the window */
	int top;                /* first row shown */
	bool loading;

	/* The open message: its summary's uid, where it's from, and it. */
	struct message *open;
	guint32 open_uid;
	char *open_mailbox;
	bool remote_images;
	int header_h;

	GArray *header_hits;
	char *status;           /* the info line */
	int drag_offset;        /* dragging the scroll bar's slider */
	bool dragging;

	GtkWidget *password;    /* its dialog, while asking */
	char *session;          /* the password manager's, once unlocked */
} ui = { .folder = -1, .selected = -1, .anchor = -1, .pending_pick = -1,
	.drop_folder = -1 };

static void load_messages(void);
static GtkWidget *pixel_area(int w, int h, GtkDrawingAreaDrawFunc draw,
	GCallback pressed);
static void move_to(struct folder *dest);
static void open_message(int index);
static void show_list(void);
static void compose(struct draft *d);

/* ---- Drawing helpers ------------------------------------------------------ */

static void add_hit(GArray *hits, int x, int y, int w, int h,
		enum action action, int index) {
	struct hit hit = { x, y, w, h, action, index };
	g_array_append_val(hits, hit);
}

static const struct hit *hit_at(GArray *hits, double x, double y) {
	for (guint i = 0; hits != NULL && i < hits->len; i++) {
		const struct hit *h = &g_array_index(hits, struct hit, i);
		if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) {
			return h;
		}
	}
	return NULL;
}

/* Text clipped to width w. */
static void clipped(cairo_t *cr, const char *s, int x, int y, int w, int h) {
	cairo_save(cr);
	cairo_rectangle(cr, x, y, MAX(w, 0), h);
	cairo_clip(cr);
	gem_text(cr, s, x, y, h);
	cairo_restore(cr);
}

/* A GEM button: a box with its label; returns its width. */
static int button(cairo_t *cr, GArray *hits, int x, int y, const char *label,
		enum action action, int index, bool bold) {
	int w = (int)gem_text_width(cr, label) + 2 * PAD;
	gem_white(cr);
	gem_fill(cr, x, y, w, BUTTON_H);
	gem_black(cr);
	gem_frame(cr, x, y, w, BUTTON_H, bold ? 2 : 1);
	gem_text(cr, label, x + PAD, y, BUTTON_H);
	add_hit(hits, x, y, w, BUTTON_H, action, index);
	return w;
}

/* When, briefly: the time today, the day this year, else the date. */
static char *short_date(gint64 unix) {
	if (unix == 0) {
		return g_strdup("");
	}
	GDateTime *when = g_date_time_new_from_unix_local(unix);
	GDateTime *now = g_date_time_new_now_local();
	const char *format = g_date_time_get_year(when) != g_date_time_get_year(now) ?
		"%Y-%m-%d" : g_date_time_get_day_of_year(when) ==
		g_date_time_get_day_of_year(now) ? "%H:%M" : "%b %e";
	char *s = g_date_time_format(when, format);
	g_date_time_unref(when);
	g_date_time_unref(now);
	return s;
}

static void set_status(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void set_status(const char *format, ...) {
	g_free(ui.status);
	if (format == NULL) {
		ui.status = NULL;
	} else {
		va_list ap;
		va_start(ap, format);
		ui.status = g_strdup_vprintf(format, ap);
		va_end(ap);
	}
	if (ui.info != NULL) {
		gtk_widget_queue_draw(ui.info);
	}
}

static struct folder *current_folder(void) {
	return ui.folder_list != NULL && ui.folder >= 0 &&
		ui.folder < (int)ui.folder_list->len ?
		ui.folder_list->pdata[ui.folder] : NULL;
}

static struct folder *folder_with_role(enum folder_role role) {
	for (guint i = 0; ui.folder_list != NULL && i < ui.folder_list->len; i++) {
		struct folder *f = ui.folder_list->pdata[i];
		if (f->role == role) {
			return f;
		}
	}
	return NULL;
}

/* The folder's count line: how many, how many unread. */
static void status_counts(void) {
	struct folder *f = current_folder();
	if (f == NULL) {
		return;
	}
	const char *plural = f->messages == 1 ? "" : "s";
	if (f->unseen > 0) {
		set_status("%s: %u message%s, %u unread", f->name, f->messages, plural,
			f->unseen);
	} else {
		set_status("%s: %u message%s", f->name, f->messages, plural);
	}
	if (ui.open == NULL) {
		gtk_window_set_title(GTK_WINDOW(ui.window), f->name);
	}
}

/* ---- The folders --------------------------------------------------------- */

/* A folder, 13x10. */
static const char *const folder_icon[] = {
	"#####........",
	"#....#.......",
	"#############",
	"#...........#",
	"#...........#",
	"#...........#",
	"#...........#",
	"#...........#",
	"#...........#",
	"#############",
};

static void paint_folders(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_fill(cr, w - 1, 0, 1, h);
	if (ui.folder_list == NULL) {
		gem_text(cr, ui.account == NULL ? "No account" : "Connecting...",
			PAD, PAD, ROW_H);
		return;
	}
	for (guint i = 0; i < ui.folder_list->len; i++) {
		struct folder *f = ui.folder_list->pdata[i];
		int y = PAD / 2 + i * ROW_H;
		bool on = (int)i == ui.folder || (int)i == ui.drop_folder;
		gem_black(cr);
		if (on) {
			gem_fill(cr, 0, y, w - 1, ROW_H);
			gem_white(cr);
		}
		int x = PAD + f->depth * 12;
		gem_bitmap(cr, folder_icon, G_N_ELEMENTS(folder_icon), x, y + 4);
		char count[16] = "";
		if (f->unseen > 0) {
			snprintf(count, sizeof(count), "%u", f->unseen);
		}
		int cw = (int)gem_text_width(cr, count);
		clipped(cr, f->name, x + 18, y, w - 1 - PAD - cw - PAD - (x + 18), ROW_H);
		gem_text(cr, count, w - 1 - PAD - cw, y, ROW_H);
		if (!f->selectable) {
			gem_grey_out(cr, 0, y, w - 1, ROW_H);
		}
	}
}

static void draw_folders(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_folders, NULL);
}

static void choose_folder(int index) {
	if (ui.folder_list == NULL || index < 0 || index >= (int)ui.folder_list->len ||
			!((struct folder *)ui.folder_list->pdata[index])->selectable) {
		return;
	}
	ui.folder = index;
	g_clear_pointer(&ui.messages, summaries_free);
	ui.selected = -1;
	g_hash_table_remove_all(ui.picked);
	ui.top = 0;
	show_list();
	gtk_widget_queue_draw(ui.folders);
	load_messages();
}

static void folders_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	choose_folder(((int)y - PAD / 2) / ROW_H);
}

/* ---- Loading ------------------------------------------------------------ */

static void show_folders(GPtrArray *folders) {
	char *was = current_folder() != NULL ?
		g_strdup(current_folder()->mailbox) : NULL;
	folders_free(ui.folder_list);
	/* Kept: the thread frees its own copy after this. */
	ui.folder_list = g_ptr_array_new();
	for (guint i = 0; i < folders->len; i++) {
		struct folder *f = g_memdup2(folders->pdata[i], sizeof(struct folder));
		f->mailbox = g_strdup(f->mailbox);
		f->name = g_strdup(f->name);
		g_ptr_array_add(ui.folder_list, f);
		if (g_strcmp0(f->mailbox, was) == 0) {
			ui.folder = i;
		}
	}
	gtk_widget_queue_draw(ui.folders);
	if (was == NULL) {
		choose_folder(0); /* the Inbox, first */
	} else {
		status_counts();
	}
	g_free(was);
	app_menu_update(ui.menu);
}

static void got_folders(GPtrArray *folders, const char *error, void *data) {
	if (error != NULL) {
		set_status("%s", error);
		gtk_widget_queue_draw(ui.folders);
		g_free(data);
		return;
	}
	show_folders(folders);
	if (data != NULL) {
		set_status("%s", (char *)data); /* what was done, over the counts */
		g_free(data);
	}
}

/* The newest messages not yet had, fetched while nothing else is
 * happening: there to open at once, or offline. */
static void fetch_ahead(struct folder *f) {
	GArray *uids = g_array_new(FALSE, FALSE, sizeof(guint32));
	for (guint i = 0; i < ui.messages->len && i < AHEAD; i++) {
		struct summary *s = ui.messages->pdata[i];
		if (s->size <= AHEAD_SIZE) {
			g_array_append_val(uids, s->uid);
		}
	}
	mail_prefetch(ui.mail, f->mailbox, (guint32 *)uids->data, uids->len);
	g_array_free(uids, TRUE);
}

static void show_messages(struct folder *f, GPtrArray *messages) {
	/* Keep the selection on the same messages. */
	guint32 uid = ui.messages != NULL && ui.selected >= 0 &&
		ui.selected < (int)ui.messages->len ?
		((struct summary *)ui.messages->pdata[ui.selected])->uid : 0;
	guint32 anchor = ui.messages != NULL && ui.anchor >= 0 &&
		ui.anchor < (int)ui.messages->len ?
		((struct summary *)ui.messages->pdata[ui.anchor])->uid : 0;
	GHashTable *was = ui.picked;
	ui.picked = g_hash_table_new(NULL, NULL);
	summaries_free(ui.messages);
	ui.messages = g_ptr_array_new();
	ui.selected = ui.anchor = -1;
	guint unseen = 0;
	for (guint i = 0; i < messages->len; i++) {
		struct summary *s = g_memdup2(messages->pdata[i], sizeof(struct summary));
		s->from = g_strdup(s->from);
		s->subject = g_strdup(s->subject);
		g_ptr_array_add(ui.messages, s);
		unseen += !s->seen;
		if (uid != 0 && s->uid == uid) {
			ui.selected = i;
		}
		if (anchor != 0 && s->uid == anchor) {
			ui.anchor = i;
		}
		if (g_hash_table_contains(was, GUINT_TO_POINTER(s->uid))) {
			g_hash_table_add(ui.picked, GUINT_TO_POINTER(s->uid));
		}
	}
	g_hash_table_destroy(was);
	f->messages = MAX(f->messages, messages->len);
	f->unseen = MAX(f->unseen, unseen);
	gtk_widget_queue_draw(ui.list);
	gtk_widget_queue_draw(ui.scroll);
	gtk_widget_queue_draw(ui.folders);
	app_menu_update(ui.menu);
}

static void got_messages(GPtrArray *messages, const char *error, void *data) {
	char *mailbox = data;
	ui.loading = false;
	struct folder *f = current_folder();
	if (f == NULL || strcmp(f->mailbox, mailbox) != 0) {
		g_free(mailbox);
		return; /* another folder's been chosen since */
	}
	g_free(mailbox);
	if (error != NULL) {
		set_status("%s", error);
		gtk_widget_queue_draw(ui.list);
		return;
	}
	show_messages(f, messages);
	status_counts();
	fetch_ahead(f);
}

static void load_messages(void) {
	struct folder *f = current_folder();
	if (f == NULL || ui.mail == NULL) {
		return;
	}
	if (ui.messages == NULL) {
		/* What was there last time, while the server's asked. */
		GPtrArray *cached = cache_messages(ui.cache, f->mailbox, LIMIT);
		if (cached->len > 0) {
			show_messages(f, cached);
		}
		summaries_free(cached);
	}
	ui.loading = true;
	set_status("Checking %s...", f->name);
	mail_list_messages(ui.mail, f->mailbox, LIMIT, got_messages,
		g_strdup(f->mailbox));
	gtk_widget_queue_draw(ui.list);
}

/* New mail: the folders' counts, and the folder shown. */
static void get_mail(void) {
	if (ui.mail == NULL) {
		return;
	}
	mail_list_folders(ui.mail, got_folders, NULL);
	if (ui.open == NULL) {
		load_messages();
	}
}

static gboolean refresh(gpointer data) {
	get_mail();
	return G_SOURCE_CONTINUE;
}

/* ---- The list ------------------------------------------------------------ */

static int visible_rows(void) {
	return MAX(1, (gtk_widget_get_height(ui.list) - ROW_H) / ROW_H);
}

static int message_count(void) {
	return ui.messages != NULL ? (int)ui.messages->len : 0;
}

static void scroll_to(int top) {
	int max = MAX(0, message_count() - visible_rows());
	ui.top = CLAMP(top, 0, max);
	gtk_widget_queue_draw(ui.list);
	gtk_widget_queue_draw(ui.scroll);
}

/* Keeps the selected row in view. */
static void reveal(void) {
	if (ui.selected < ui.top) {
		scroll_to(ui.selected);
	} else if (ui.selected >= ui.top + visible_rows()) {
		scroll_to(ui.selected - visible_rows() + 1);
	}
}

/* An unread message's mark: a diamond. */
static void diamond(cairo_t *cr, int cx, int cy) {
	cairo_move_to(cr, cx, cy - 4);
	cairo_line_to(cr, cx + 4, cy);
	cairo_line_to(cr, cx, cy + 4);
	cairo_line_to(cr, cx - 4, cy);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* ---- Selecting ----------------------------------------------------------- */

static bool is_picked(const struct summary *s) {
	return g_hash_table_contains(ui.picked, GUINT_TO_POINTER(s->uid));
}

static void pick(int i) {
	struct summary *s = ui.messages->pdata[i];
	g_hash_table_add(ui.picked, GUINT_TO_POINTER(s->uid));
}

static void picked_changed(void) {
	guint n = g_hash_table_size(ui.picked);
	if (n > 1) {
		set_status("%u messages selected", n);
	} else {
		status_counts();
	}
	gtk_widget_queue_draw(ui.list);
	app_menu_update(ui.menu);
}

/* Just this one. */
static void pick_one(int i) {
	g_hash_table_remove_all(ui.picked);
	ui.selected = ui.anchor = i;
	if (i >= 0 && i < message_count()) {
		pick(i);
	}
}

/* From the anchor to this one; added to what's selected if keep. */
static void pick_range(int i, bool keep) {
	if (!keep) {
		g_hash_table_remove_all(ui.picked);
	}
	if (ui.anchor < 0 || ui.anchor >= message_count()) {
		ui.anchor = i;
	}
	for (int k = MIN(ui.anchor, i); k <= MAX(ui.anchor, i); k++) {
		pick(k);
	}
	ui.selected = i;
}

static void pick_toggle(int i) {
	struct summary *s = ui.messages->pdata[i];
	if (!g_hash_table_remove(ui.picked, GUINT_TO_POINTER(s->uid))) {
		pick(i);
	}
	ui.selected = ui.anchor = i;
}

static void pick_all(void) {
	for (int i = 0; i < message_count(); i++) {
		pick(i);
	}
	picked_changed();
}

/* What an action's for: the message open, else those selected (in the
 * list's order), else the one the keys are on. */
static GPtrArray *targets(void) {
	GPtrArray *t = g_ptr_array_new();
	for (int i = 0; i < message_count(); i++) {
		struct summary *s = ui.messages->pdata[i];
		if (ui.open != NULL ? s->uid == ui.open_uid : is_picked(s)) {
			g_ptr_array_add(t, s);
		}
	}
	if (t->len == 0 && ui.open == NULL && ui.selected >= 0 &&
			ui.selected < message_count()) {
		g_ptr_array_add(t, ui.messages->pdata[ui.selected]);
	}
	return t;
}

static void paint_list(cairo_t *cr, int w, int h, void *data) {
	int from_x = 20, from_w = MIN(200, w / 4), date_w = 90;
	int subject_x = from_x + from_w + PAD;
	int subject_w = w - subject_x - date_w - PAD;
	gem_black(cr);
	gem_text(cr, "From", from_x, 0, ROW_H);
	gem_text(cr, "Subject", subject_x, 0, ROW_H);
	gem_text(cr, "Date", w - PAD - gem_text_width(cr, "Date"), 0, ROW_H);
	for (int x = 0; x < w; x += 2) {
		gem_fill(cr, x, ROW_H - 2, 1, 1);
	}
	if (ui.account == NULL) {
		int y = ROW_H * 2;
		char **lines = g_strsplit(ui.account_error, "\n", -1);
		for (int i = 0; lines[i] != NULL; i++, y += ROW_H) {
			gem_text(cr, lines[i], PAD * 2, y, ROW_H);
		}
		g_strfreev(lines);
		return;
	}
	if (message_count() == 0) {
		const char *what = ui.loading ? "Checking for mail..." : "No messages";
		gem_text(cr, what, (w - gem_text_width(cr, what)) / 2, h / 3, ROW_H);
		return;
	}
	int rows = visible_rows();
	for (int r = 0; r < rows && ui.top + r < message_count(); r++) {
		int i = ui.top + r;
		struct summary *s = ui.messages->pdata[i];
		int y = ROW_H + r * ROW_H;
		gem_black(cr);
		if (is_picked(s)) {
			gem_fill(cr, 0, y, w, ROW_H);
			gem_white(cr);
		}
		if (!s->seen) {
			diamond(cr, 10, y + ROW_H / 2);
		}
		clipped(cr, s->from, from_x, y, from_w, ROW_H);
		clipped(cr, s->subject[0] ? s->subject : "(no subject)", subject_x, y,
			subject_w, ROW_H);
		char *date = short_date(s->date);
		gem_text(cr, date, w - PAD - gem_text_width(cr, date), y, ROW_H);
		g_free(date);
	}
}

static void draw_list(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_list, NULL);
}

static int row_at(double y) {
	int r = ((int)y - ROW_H) / ROW_H;
	int i = ui.top + r;
	return y >= ROW_H && i < message_count() ? i : -1;
}

static void list_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	int i = row_at(y);
	if (i < 0) {
		return;
	}
	GdkModifierType state = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(g));
	bool ctrl = state & GDK_CONTROL_MASK, shift = state & GDK_SHIFT_MASK;
	ui.pending_pick = -1;
	if (shift) {
		pick_range(i, ctrl);
	} else if (ctrl) {
		pick_toggle(i);
	} else if (n == 1 && is_picked(ui.messages->pdata[i]) &&
			g_hash_table_size(ui.picked) > 1) {
		/* Maybe to drag them all: just this one only if it's let go. */
		ui.pending_pick = i;
		ui.selected = i;
	} else {
		pick_one(i);
	}
	picked_changed();
	if (n == 2 && !ctrl && !shift) {
		open_message(i);
	}
}

static void list_released(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	if (ui.pending_pick >= 0 && ui.pending_pick < message_count()) {
		pick_one(ui.pending_pick);
		picked_changed();
	}
	ui.pending_pick = -1;
}

/* ---- Dragging messages to a folder --------------------------------------- */

static int folder_at(double y) {
	int i = ((int)y - PAD / 2) / ROW_H;
	return ui.folder_list != NULL && y >= PAD / 2 &&
		i < (int)ui.folder_list->len ? i : -1;
}

/* A folder they can go to: any other that holds mail. */
static bool can_drop(int i) {
	struct folder *f = i >= 0 ? ui.folder_list->pdata[i] : NULL;
	return f != NULL && f->selectable && i != ui.folder;
}

static void set_drop_folder(int i) {
	if (i != ui.drop_folder) {
		ui.drop_folder = i;
		gtk_widget_queue_draw(ui.folders);
	}
}

/* Dragged in the window, as GEM's desktop does: an outline follows the
 * pointer, and the folder under it lights up. */
static void paint_ghost(cairo_t *cr, int w, int h, void *data) {
	/* See-through, but for the outline. */
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	if (!ui.dragging_messages) {
		return;
	}
	guint n = g_hash_table_size(ui.picked);
	struct summary *s = ui.selected >= 0 && ui.selected < message_count() ?
		ui.messages->pdata[ui.selected] : NULL;
	char *label = n > 1 ? g_strdup_printf("%u messages", n) :
		g_strdup(s != NULL && s->subject[0] ? s->subject : "(no subject)");
	int gw = MIN((int)gem_text_width(cr, label), 260) + 2 * PAD + 2;
	int gh = ROW_H + 2;
	int x = (int)ui.ghost_x - PAD, y = (int)ui.ghost_y - gh / 2;
	gem_white(cr);
	gem_fill(cr, x, y, gw, gh);
	gem_black(cr);
	gem_frame(cr, x, y, gw, gh, 1);
	clipped(cr, label, x + PAD + 1, y + 1, gw - 2 * PAD - 2, ROW_H);
	g_free(label);
}

static void draw_ghost(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_ghost, NULL);
}

static void list_drag_begin(GtkGestureDrag *g, double x, double y,
		gpointer data) {
	ui.drag_row = ui.open == NULL ? row_at(y) : -1;
}

static void list_drag_update(GtkGestureDrag *g, double dx, double dy,
		gpointer data) {
	double sx, sy;
	if (ui.drag_row < 0 || !gtk_gesture_drag_get_start_point(g, &sx, &sy)) {
		return;
	}
	if (!ui.dragging_messages) {
		if (dx * dx + dy * dy < 8 * 8 || ui.drag_row >= message_count()) {
			return;
		}
		ui.dragging_messages = true;
		ui.pending_pick = -1;
		if (!is_picked(ui.messages->pdata[ui.drag_row])) {
			pick_one(ui.drag_row);
		}
		picked_changed();
		gtk_widget_set_visible(ui.ghost, TRUE);
	}
	graphene_point_t at = GRAPHENE_POINT_INIT(sx + dx, sy + dy), in;
	if (gtk_widget_compute_point(ui.list, ui.ghost, &at, &in)) {
		ui.ghost_x = in.x;
		ui.ghost_y = in.y;
	}
	int i = -1;
	if (gtk_widget_compute_point(ui.list, ui.folders, &at, &in) &&
			in.x >= 0 && in.x < gtk_widget_get_width(ui.folders)) {
		i = folder_at(in.y);
	}
	set_drop_folder(can_drop(i) ? i : -1);
	gtk_widget_queue_draw(ui.ghost);
}

static void list_drag_end(GtkGestureDrag *g, double dx, double dy,
		gpointer data) {
	if (!ui.dragging_messages) {
		return;
	}
	int i = ui.drop_folder;
	ui.dragging_messages = false;
	gtk_widget_set_visible(ui.ghost, FALSE);
	set_drop_folder(-1);
	if (i >= 0) {
		move_to(ui.folder_list->pdata[i]);
	}
}

static gboolean list_scrolled(GtkEventControllerScroll *c, double dx,
		double dy, gpointer data) {
	scroll_to(ui.top + (int)(dy * 3));
	return TRUE;
}

/* ---- The scroll bar, GEM's ---------------------------------------------- */

/* The slider's place and size in the track, which is between the arrows. */
static void slider(int h, int *y, int *len) {
	int track = h - 2 * GADGET;
	int total = MAX(message_count(), 1), shown = visible_rows();
	*len = total <= shown ? track : MAX(GADGET, track * shown / total);
	int range = MAX(total - shown, 1);
	*y = GADGET + (track - *len) * ui.top / range;
}

static void paint_arrow(cairo_t *cr, int y, bool up) {
	double cx = GADGET / 2.0 + 0.5, cy = y + GADGET / 2.0;
	cairo_move_to(cr, cx, up ? cy - 4 : cy + 4);
	cairo_line_to(cr, cx + 5, up ? cy + 3 : cy - 3);
	cairo_line_to(cr, cx - 5, up ? cy + 3 : cy - 3);
	cairo_close_path(cr);
	cairo_fill(cr);
}

static void paint_scroll(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_fill(cr, 0, 0, 1, h);
	/* The dithered track. */
	for (int y = GADGET; y < h - GADGET; y++) {
		for (int x = 1 + (y & 1); x < w; x += 2) {
			gem_fill(cr, x, y, 1, 1);
		}
	}
	gem_fill(cr, 0, GADGET - 1, w, 1);
	gem_fill(cr, 0, h - GADGET, w, 1);
	gem_white(cr);
	gem_fill(cr, 1, 0, w - 1, GADGET - 1);
	gem_fill(cr, 1, h - GADGET + 1, w - 1, GADGET - 1);
	gem_black(cr);
	paint_arrow(cr, 0, true);
	paint_arrow(cr, h - GADGET, false);
	int sy, len;
	slider(h, &sy, &len);
	gem_white(cr);
	gem_fill(cr, 1, sy, w - 1, len);
	gem_black(cr);
	gem_frame(cr, 0, sy, w, len, 1);
}

static void draw_scroll(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_scroll, NULL);
}

static void scroll_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	int h = gtk_widget_get_height(ui.scroll), sy, len;
	slider(h, &sy, &len);
	if (y < GADGET) {
		scroll_to(ui.top - 1);
	} else if (y >= h - GADGET) {
		scroll_to(ui.top + 1);
	} else if (y < sy) {
		scroll_to(ui.top - visible_rows());
	} else if (y >= sy + len) {
		scroll_to(ui.top + visible_rows());
	} else {
		ui.dragging = true;
		ui.drag_offset = (int)y - sy;
	}
}

static void scroll_dragged(GtkGestureDrag *g, double dx, double dy,
		gpointer data) {
	if (!ui.dragging) {
		return;
	}
	double sx, sy0;
	gtk_gesture_drag_get_start_point(g, &sx, &sy0);
	int h = gtk_widget_get_height(ui.scroll), sy, len;
	slider(h, &sy, &len);
	int track = h - 2 * GADGET - len;
	int pos = (int)(sy0 + dy) - ui.drag_offset - GADGET;
	int range = MAX(message_count() - visible_rows(), 0);
	scroll_to(track > 0 ? pos * range / track : 0);
}

static void scroll_released(GtkGestureDrag *g, double dx, double dy,
		gpointer data) {
	ui.dragging = false;
}

/* ---- The open message --------------------------------------------------- */

/* Nothing remote until asked: no tracking pixels, no fonts from afar. */
static const char *policy(bool remote) {
	return remote ?
		"default-src 'none'; img-src * cid: data:; style-src * 'unsafe-inline'; "
		"font-src * data:; media-src *" :
		"default-src 'none'; img-src cid: data:; style-src 'unsafe-inline'";
}

/* GEM's scroll bars, and GEM's selection. */
static const char page_css[] =
	"::selection { background: #000; color: #fff; }"
	"::-webkit-scrollbar { width: 19px; height: 19px; background: #fff; }"
	"::-webkit-scrollbar-track { background: repeating-conic-gradient("
	"  #000 0% 25%, #fff 0% 50%) 0 0 / 2px 2px; border-left: 1px solid #000; }"
	"::-webkit-scrollbar-thumb { background: #fff; border: 1px solid #000; }";

/* Senders whose images are shown without asking: addresses, one to a
 * line, in ~/.config/gemmail/show-images. */
static GHashTable *image_senders;

static char *image_senders_path(void) {
	return g_build_filename(g_get_user_config_dir(), "gemmail", "show-images",
		NULL);
}

static GHashTable *senders(void) {
	if (image_senders != NULL) {
		return image_senders;
	}
	image_senders = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	char *path = image_senders_path(), *text = NULL;
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i] != NULL; i++) {
			char *line = g_strstrip(lines[i]);
			if (line[0] != '\0' && line[0] != '#') {
				g_hash_table_add(image_senders, g_ascii_strdown(line, -1));
			}
		}
		g_strfreev(lines);
	}
	g_free(text);
	g_free(path);
	return image_senders;
}

static void set_image_sender(const char *sender, bool shown) {
	if (shown) {
		g_hash_table_add(senders(), g_strdup(sender));
	} else {
		g_hash_table_remove(senders(), sender);
	}
	GPtrArray *sorted = g_hash_table_get_keys_as_ptr_array(senders());
	g_ptr_array_sort_values(sorted, (GCompareFunc)strcmp);
	GString *text = g_string_new("# GemMail shows these senders' images "
		"without asking.\n");
	for (guint i = 0; i < sorted->len; i++) {
		g_string_append_printf(text, "%s\n", (char *)sorted->pdata[i]);
	}
	char *path = image_senders_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	GError *error = NULL;
	if (!g_file_set_contents(path, text->str, -1, &error)) {
		set_status("Couldn't save %s: %s", path, error->message);
		g_error_free(error);
	}
	g_free(dir);
	g_free(path);
	g_string_free(text, TRUE);
	g_ptr_array_unref(sorted);
}

/* The open message's sender is on the list. */
static bool sender_trusted(void) {
	return ui.open != NULL && ui.open->sender != NULL &&
		g_hash_table_contains(senders(), ui.open->sender);
}

static bool has_remote(const char *html) {
	static GRegex *remote;
	if (remote == NULL) {
		remote = g_regex_new(
			"(src|background|srcset)\\s*=\\s*[\"']?\\s*https?:|url\\(\\s*[\"']?https?:",
			G_REGEX_CASELESS, 0, NULL);
	}
	return html != NULL && g_regex_match(remote, html, 0, NULL);
}

/* Text as a page: escaped, wrapped, its web addresses made links. */
static char *text_page(const char *text) {
	char *escaped = g_markup_escape_text(text, -1);
	GRegex *url = g_regex_new("(https?://[^\\s<>\"]+)", 0, 0, NULL);
	char *linked = g_regex_replace(url, escaped, -1, 0,
		"<a href=\"\\1\">\\1</a>", 0, NULL);
	g_regex_unref(url);
	char *page = g_strdup_printf("<style>body { margin: 12px; "
		"font: 15px/1.45 monospace; white-space: pre-wrap; "
		"overflow-wrap: anywhere; color: #000; background: #fff; }"
		"a { color: #000; }</style><body>%s</body>", linked);
	g_free(linked);
	g_free(escaped);
	return page;
}

static void show_body(void) {
	struct message *m = ui.open;
	char *body = m->html != NULL ? g_strdup(m->html) :
		text_page(m->text != NULL ? m->text : "");
	/* First, so it heads the page, before anything it covers. */
	char *page = g_strdup_printf("<meta charset=\"utf-8\">"
		"<meta http-equiv=\"Content-Security-Policy\" content=\"%s\">"
		"<style>%s</style>%s", policy(ui.remote_images), page_css, body);
	webkit_web_view_load_html(WEBKIT_WEB_VIEW(ui.view), page, NULL);
	g_free(page);
	g_free(body);
}

static void paint_header(cairo_t *cr, int w, int h, void *data) {
	GArray *hits = ui.header_hits;
	g_array_set_size(hits, 0);
	struct message *m = ui.open;
	if (m == NULL) {
		return;
	}
	int x = PAD, y = PAD;
	x += button(cr, hits, x, y, "< Back", ACT_BACK, 0, false) + PAD;
	x += button(cr, hits, x, y, "Reply", ACT_REPLY, 0, true) + PAD;
	x += button(cr, hits, x, y, "Reply All", ACT_REPLY_ALL, 0, false) + PAD;
	x += button(cr, hits, x, y, "Forward", ACT_FORWARD, 0, false) + PAD;
	x += button(cr, hits, x, y, "Archive", ACT_ARCHIVE, 0, false) + PAD;
	x += button(cr, hits, x, y, "Delete", ACT_DELETE, 0, false) + PAD;
	if (m->html != NULL && has_remote(m->html)) {
		if (!ui.remote_images) {
			x += button(cr, hits, x, y, "Show Images", ACT_SHOW_IMAGES, 0, false) +
				PAD;
		}
		if (sender_trusted()) {
			button(cr, hits, x, y, "Stop Showing Images", ACT_STOP_IMAGES, 0,
				false);
		} else if (m->sender != NULL) {
			/* Who, if there's room to say. */
			char *always = g_strdup_printf("Always for %s", m->sender);
			if (x + gem_text_width(cr, always) + 2 * PAD > w - PAD) {
				g_free(always);
				always = g_strdup("Always for Sender");
			}
			button(cr, hits, x, y, always, ACT_ALWAYS_IMAGES, 0, false);
			g_free(always);
		}
	}
	y += BUTTON_H + PAD;

	const struct { const char *label, *value; } fields[] = {
		{ "From:", m->from }, { "To:", m->to }, { "Cc:", m->cc },
		{ "Date:", m->date }, { "Subject:", m->subject },
	};
	int lw = (int)gem_text_width(cr, "Subject:") + PAD;
	gem_black(cr);
	for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
		if (fields[i].value == NULL || fields[i].value[0] == '\0') {
			continue;
		}
		gem_text(cr, fields[i].label, PAD, y, ROW_H);
		clipped(cr, fields[i].value, PAD + lw, y, w - 2 * PAD - lw, ROW_H);
		y += ROW_H;
	}
	if (m->attachments->len > 0) {
		/* Each a small box: click to save it in Downloads. */
		gem_text(cr, "Files:", PAD, y + 2, ROW_H);
		int ax = PAD + lw;
		for (guint i = 0; i < m->attachments->len; i++) {
			struct attachment *a = m->attachments->pdata[i];
			int bw = (int)gem_text_width(cr, a->filename) + 2 * PAD;
			if (ax + bw > w - PAD && ax > PAD + lw) {
				ax = PAD + lw;
				y += BUTTON_H + 2;
			}
			ax += button(cr, hits, ax, y, a->filename, ACT_ATTACHMENT, i, false) +
				PAD / 2;
		}
		y += BUTTON_H + 2;
	}
	y += PAD / 2;
	gem_black(cr);
	gem_fill(cr, 0, y, w, 1);
	y += 1;
	if (y != ui.header_h) {
		ui.header_h = y;
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.header), y);
	}
}

static void draw_header(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_header, NULL);
}

static void run_action(enum action action, int index);

static void header_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	const struct hit *h = hit_at(ui.header_hits, x, y);
	if (h != NULL) {
		run_action(h->action, h->index);
	}
}

static void show_list(void) {
	g_clear_pointer(&ui.open, message_free);
	g_clear_pointer(&ui.open_mailbox, g_free);
	gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "list");
	gtk_widget_grab_focus(ui.list);
	status_counts();
	app_menu_update(ui.menu);
}

static struct summary *summary_for(guint32 uid) {
	for (int i = 0; i < message_count(); i++) {
		struct summary *s = ui.messages->pdata[i];
		if (s->uid == uid) {
			return s;
		}
	}
	return NULL;
}

static void fetched(GBytes *raw, const char *error, void *data) {
	guint32 uid = GPOINTER_TO_UINT(data);
	if (error != NULL) {
		set_status("%s", error);
		return;
	}
	struct message *m = message_parse(raw);
	if (m == NULL) {
		set_status("That message couldn't be read");
		return;
	}
	if (current_folder() == NULL) {
		message_free(m);
		return;
	}
	message_free(ui.open);
	ui.open = m;
	ui.open_uid = uid;
	g_free(ui.open_mailbox);
	ui.open_mailbox = g_strdup(current_folder()->mailbox);
	/* Trusted senders' images show, unless the server says the message
	 * may not really be theirs. */
	ui.remote_images = sender_trusted() &&
		m->sender_check != SENDER_UNVERIFIED;
	struct summary *s = summary_for(uid);
	struct folder *f = current_folder();
	if (s != NULL && !s->seen) {
		s->seen = true;
		if (f != NULL && f->unseen > 0) {
			f->unseen--;
		}
		gtk_widget_queue_draw(ui.folders);
	}
	gtk_window_set_title(GTK_WINDOW(ui.window),
		m->subject[0] ? m->subject : "(no subject)");
	gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "message");
	gtk_widget_queue_draw(ui.header);
	show_body();
	if (g_strcmp0(ui.status, "Opening...") == 0) {
		set_status(NULL); /* but not "Archived..." and the like */
	}
	if (sender_trusted() && !ui.remote_images && has_remote(m->html)) {
		set_status("Images not shown: the server couldn't confirm this is "
			"from %s", m->sender);
	}
	app_menu_update(ui.menu);
}

static void open_message(int index) {
	if (index < 0 || index >= message_count()) {
		return;
	}
	struct summary *s = ui.messages->pdata[index];
	pick_one(index);
	GBytes *cached = cache_body(ui.cache, current_folder()->mailbox, s->uid);
	if (cached != NULL) {
		if (!s->seen) {
			mail_set_seen(ui.mail, current_folder()->mailbox, &s->uid, 1, true,
				NULL, NULL);
		}
		fetched(cached, NULL, GUINT_TO_POINTER(s->uid));
		g_bytes_unref(cached);
		return;
	}
	set_status("Opening...");
	mail_fetch(ui.mail, current_folder()->mailbox, s->uid, !s->seen, fetched,
		GUINT_TO_POINTER(s->uid));
}

/* Links go to the browser (mailto: to a new message); nothing else leaves
 * the page. */
static gboolean view_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType type, gpointer data) {
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
		return FALSE;
	}
	WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(
		WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	const char *uri = webkit_uri_request_get_uri(
		webkit_navigation_action_get_request(action));
	if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
			g_strcmp0(uri, "about:blank") == 0) {
		return FALSE; /* the message itself */
	}
	webkit_policy_decision_ignore(decision);
	if (webkit_navigation_action_get_navigation_type(action) ==
			WEBKIT_NAVIGATION_TYPE_LINK_CLICKED ||
			type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
		if (g_str_has_prefix(uri, "mailto:")) {
			struct draft *d = draft_new();
			g_free(d->to);
			d->to = g_uri_unescape_string(uri + 7, NULL);
			char *q = strchr(d->to, '?');
			if (q != NULL) {
				*q = '\0';
			}
			compose(d);
		} else if (g_str_has_prefix(uri, "http://") ||
				g_str_has_prefix(uri, "https://")) {
			g_app_info_launch_default_for_uri(uri, NULL, NULL);
		}
	}
	return TRUE;
}

/* cid: the message's own images, for its HTML. */
static void cid_request(WebKitURISchemeRequest *request, gpointer data) {
	const char *path = webkit_uri_scheme_request_get_path(request);
	struct attachment *a = ui.open != NULL && path != NULL ?
		g_hash_table_lookup(ui.open->inline_parts, path) : NULL;
	if (a == NULL) {
		GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"not in this message");
		webkit_uri_scheme_request_finish_error(request, e);
		g_error_free(e);
		return;
	}
	GInputStream *in = g_memory_input_stream_new_from_bytes(a->data);
	webkit_uri_scheme_request_finish(request, in, g_bytes_get_size(a->data),
		a->type);
	g_object_unref(in);
}

static gboolean no_menu(void) {
	return TRUE;
}

static GtkWidget *view_new(void) {
	WebKitSettings *settings = webkit_settings_new();
	webkit_settings_set_enable_javascript(settings, FALSE);
	webkit_settings_set_enable_javascript_markup(settings, FALSE);
	webkit_settings_set_auto_load_images(settings, TRUE);
	WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
	GtkWidget *view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
		"settings", settings, "network-session", session, NULL);
	g_object_unref(settings);
	g_object_unref(session);
	WebKitWebContext *context = webkit_web_view_get_context(WEBKIT_WEB_VIEW(view));
	webkit_web_context_register_uri_scheme(context, "cid", cid_request, NULL,
		NULL);
	webkit_security_manager_register_uri_scheme_as_secure(
		webkit_web_context_get_security_manager(context), "cid");
	g_signal_connect(view, "decide-policy", G_CALLBACK(view_policy), NULL);
	/* No WebKit menu of its own (Reload, Inspect...). */
	g_signal_connect(view, "context-menu", G_CALLBACK(no_menu), NULL);
	gtk_widget_set_vexpand(view, TRUE);
	return view;
}

/* ---- Acting -------------------------------------------------------------- */

static struct summary *selected_summary(void) {
	return ui.messages != NULL && ui.selected >= 0 &&
		ui.selected < message_count() ? ui.messages->pdata[ui.selected] : NULL;
}

static void save_attachment(int index) {
	struct attachment *a = ui.open != NULL &&
		index < (int)ui.open->attachments->len ?
		ui.open->attachments->pdata[index] : NULL;
	if (a == NULL) {
		return;
	}
	const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
	char *folder = g_strdup(dir != NULL ? dir :
		g_build_filename(g_get_home_dir(), "Downloads", NULL));
	g_mkdir_with_parents(folder, 0755);
	char *path = NULL;
	for (int n = 1; path == NULL; n++) {
		char *name = n == 1 ? g_strdup(a->filename) :
			g_strdup_printf("%d-%s", n, a->filename);
		path = g_build_filename(folder, name, NULL);
		g_free(name);
		if (g_file_test(path, G_FILE_TEST_EXISTS)) {
			g_clear_pointer(&path, g_free);
		}
	}
	GError *e = NULL;
	if (g_file_set_contents(path, g_bytes_get_data(a->data, NULL),
			g_bytes_get_size(a->data), &e)) {
		char *base = g_path_get_basename(path);
		set_status("Saved Downloads/%s", base);
		g_free(base);
	} else {
		set_status("Couldn't save it: %s", e->message);
		g_error_free(e);
	}
	g_free(path);
	g_free(folder);
}

/* Archiving or deleting: the message leaves the list at once (and in the
 * message view, the next one opens); if the server says no, the list's
 * loaded again. */

struct moving {
	char *mailbox;
	char *done; /* what to say when it's done */
};

static void moved(const char *error, void *data) {
	struct moving *mv = data;
	if (error != NULL) {
		set_status("%s", error);
		struct folder *f = current_folder();
		if (f != NULL && strcmp(f->mailbox, mv->mailbox) == 0) {
			load_messages(); /* back as it is on the server */
		}
	} else {
		set_status("%s", mv->done);
		/* The other folder's count grows. */
		mail_list_folders(ui.mail, got_folders, g_strdup(mv->done));
	}
	g_free(mv->mailbox);
	g_free(mv->done);
	g_free(mv);
}

/* Where archived mail goes: the folder flagged \Archive, else \All
 * (Gmail's All Mail). */
static struct folder *archive_folder(void) {
	struct folder *f = folder_with_role(ROLE_ARCHIVE);
	return f != NULL ? f : folder_with_role(ROLE_ALL);
}

static void send_away(struct folder *f, GPtrArray *t, const char *to,
	bool create, struct moving *mv);

static void take_away(bool archive) {
	struct folder *f = current_folder();
	GPtrArray *t = f != NULL ? targets() : NULL;
	if (t == NULL || t->len == 0) {
		if (t != NULL) {
			g_ptr_array_free(t, TRUE);
		}
		return;
	}
	guint n = t->len;
	const char *to;
	bool create = false;
	struct moving *mv = g_new0(struct moving, 1);
	mv->mailbox = g_strdup(f->mailbox);
	if (archive) {
		struct folder *a = archive_folder();
		if (a == f) {
			set_status("%s in %s already", n > 1 ? "They're" : "It's", f->name);
			g_free(mv->mailbox);
			g_free(mv);
			g_ptr_array_free(t, TRUE);
			return;
		}
		/* No archive folder yet: one's made, as Thunderbird does. */
		to = a != NULL ? a->mailbox :
			ui.account->archive != NULL ? ui.account->archive : "Archive";
		create = a == NULL;
		const char *where = a != NULL ? a->name : to;
		mv->done = n > 1 ?
			g_strdup_printf("Archived %u messages in %s.", n, where) :
			g_strdup_printf("Archived in %s.", where);
	} else {
		/* Out of Trash, it's gone for good. */
		struct folder *trash = folder_with_role(ROLE_TRASH);
		to = trash != NULL && trash != f ? trash->mailbox : NULL;
		mv->done = n > 1 ?
			g_strdup_printf(to != NULL ? "Moved %u messages to Trash." :
			"Deleted %u messages.", n) :
			g_strdup(to != NULL ? "Moved to Trash." : "Deleted.");
	}
	send_away(f, t, to, create, mv);
}

/* Sends the messages t (freed) from f to another folder (to NULL: gone
 * for good): out of the list at once, the next one selected (or, reading,
 * opened). */
static void send_away(struct folder *f, GPtrArray *t, const char *to,
		bool create, struct moving *mv) {
	guint n = t->len;
	guint32 *uids = g_new(guint32, n);
	for (guint k = 0; k < n; k++) {
		uids[k] = ((struct summary *)t->pdata[k])->uid;
	}
	mail_move(ui.mail, f->mailbox, uids, n, to, create, moved, mv);
	g_free(uids);

	/* Out of the list; the next is whatever's now where the first was. */
	int first = -1;
	for (guint k = 0; k < n; k++) {
		struct summary *s = t->pdata[k];
		guint index;
		g_ptr_array_find(ui.messages, s, &index);
		if (first < 0) {
			first = index;
		}
		if (!s->seen && f->unseen > 0) {
			f->unseen--;
		}
		if (f->messages > 0) {
			f->messages--;
		}
		g_free(s->from);
		g_free(s->subject);
		g_free(s);
		g_ptr_array_remove_index(ui.messages, index);
	}
	g_ptr_array_free(t, TRUE);
	/* The next: the one after (older), or before if it was the last. */
	int next = MIN(first, message_count() - 1);
	pick_one(next);
	gtk_widget_queue_draw(ui.list);
	gtk_widget_queue_draw(ui.scroll);
	gtk_widget_queue_draw(ui.folders);
	if (ui.open != NULL) {
		if (next >= 0) {
			open_message(next);
		} else {
			show_list();
		}
	} else {
		reveal();
	}
	app_menu_update(ui.menu);
}

/* Marks read, or if all are read already, unread. */
static void toggle_seen(void) {
	struct folder *f = current_folder();
	GPtrArray *t = f != NULL ? targets() : NULL;
	if (t == NULL || t->len == 0) {
		if (t != NULL) {
			g_ptr_array_free(t, TRUE);
		}
		return;
	}
	bool seen = false;
	for (guint k = 0; k < t->len; k++) {
		seen |= !((struct summary *)t->pdata[k])->seen;
	}
	GArray *uids = g_array_new(FALSE, FALSE, sizeof(guint32));
	for (guint k = 0; k < t->len; k++) {
		struct summary *s = t->pdata[k];
		if (s->seen != seen) {
			s->seen = seen;
			f->unseen = seen ? (f->unseen > 0 ? f->unseen - 1 : 0) :
				f->unseen + 1;
			g_array_append_val(uids, s->uid);
		}
	}
	mail_set_seen(ui.mail, f->mailbox, (guint32 *)uids->data, uids->len, seen,
		NULL, NULL);
	g_array_free(uids, TRUE);
	g_ptr_array_free(t, TRUE);
	if (g_hash_table_size(ui.picked) > 1) {
		picked_changed();
	} else {
		status_counts();
	}
	gtk_widget_queue_draw(ui.list);
	gtk_widget_queue_draw(ui.folders);
	app_menu_update(ui.menu);
}

/* Moves what's selected (or open) to another folder. */
static void move_to(struct folder *dest) {
	struct folder *f = current_folder();
	GPtrArray *t = f != NULL && dest != NULL && dest != f &&
		dest->selectable ? targets() : NULL;
	if (t == NULL || t->len == 0) {
		if (t != NULL) {
			g_ptr_array_free(t, TRUE);
		}
		return;
	}
	struct moving *mv = g_new0(struct moving, 1);
	mv->mailbox = g_strdup(f->mailbox);
	mv->done = t->len > 1 ?
		g_strdup_printf("Moved %u messages to %s.", t->len, dest->name) :
		g_strdup_printf("Moved to %s.", dest->name);
	send_away(f, t, dest->mailbox, false, mv);
}

/* ---- Move to Folder: a GEM dialog with a pop-up of the folders ---------- */

static struct {
	GtkWidget *window, *area, *popover, *popup_area;
	GArray *hits;
	GPtrArray *mailboxes; /* those it can go to: the server's names */
	char *title;
	int chosen, hover;
	int top;       /* the first folder shown in the pop-up, scrolled */
	int arrow;     /* the arrow the pointer's on (ARROW_UP/DOWN), or 0 */
	guint scroller; /* scrolling while it's there */
} mover;

/* The pop-up's most rows; with more folders than that, its first and last
 * rows are arrows that scroll it, as GEM's scrolling pop-ups do. */
#define POPUP_ROWS 14
enum { ARROW_UP = -2, ARROW_DOWN = -3 };

static char *last_move; /* the folder chosen last time */

static struct folder *folder_named(const char *mailbox) {
	for (guint i = 0; ui.folder_list != NULL && i < ui.folder_list->len; i++) {
		struct folder *f = ui.folder_list->pdata[i];
		if (strcmp(f->mailbox, mailbox) == 0) {
			return f;
		}
	}
	return NULL;
}

static struct folder *mover_folder(int i) {
	return i >= 0 && i < (int)mover.mailboxes->len ?
		folder_named(mover.mailboxes->pdata[i]) : NULL;
}

/* A GEM pop-up button: a shadowed box showing the choice. */
static void paint_mover(cairo_t *cr, int w, int h, void *data) {
	g_array_set_size(mover.hits, 0);
	gem_black(cr);
	gem_frame(cr, 0, 0, w, h, 1);
	gem_frame(cr, 3, 3, w - 6, h - 6, 2);
	int x = 3 + 2 * PAD, y = 3 + 2 * PAD;
	gem_text(cr, mover.title, x, y, ROW_H);
	y += ROW_H + PAD;
	int pw = w - 2 * x - 2;
	gem_frame(cr, x, y, pw, BUTTON_H, 1);
	gem_fill(cr, x + 2, y + BUTTON_H, pw, 2);
	gem_fill(cr, x + pw, y + 2, 2, BUTTON_H);
	struct folder *f = mover_folder(mover.chosen);
	clipped(cr, f != NULL ? f->name : "", x + PAD, y, pw - 2 * PAD, BUTTON_H);
	add_hit(mover.hits, x, y, pw + 2, BUTTON_H + 2, ACT_MOVE_POPUP, 0);
	int by = h - 3 - 2 * PAD - BUTTON_H;
	int bw = MAX(gem_text_width(cr, "Cancel"), gem_text_width(cr, "Move")) +
		2 * PAD;
	int bx = w - 3 - 2 * PAD - 2 * bw - PAD;
	for (int i = 0; i < 2; i++) {
		const char *label = i == 0 ? "Cancel" : "Move";
		int bxi = bx + i * (bw + PAD);
		gem_frame(cr, bxi, by, bw, BUTTON_H, i == 1 ? 2 : 1);
		gem_text(cr, label, bxi + (bw - gem_text_width(cr, label)) / 2, by,
			BUTTON_H);
		add_hit(mover.hits, bxi, by, bw, BUTTON_H,
			i == 0 ? ACT_MOVE_CANCEL : ACT_MOVE_OK, 0);
	}
}

static void draw_mover(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_mover, NULL);
}

/* The pop-up: the folders, the chosen one checked, the one under the
 * pointer inverted. */
static bool popup_scrolls(void) {
	return (int)mover.mailboxes->len > POPUP_ROWS;
}

/* How many folders it shows at once. */
static int popup_shown(void) {
	return popup_scrolls() ? POPUP_ROWS - 2 : (int)mover.mailboxes->len;
}

static void popup_scroll_to(int top) {
	top = CLAMP(top, 0, (int)mover.mailboxes->len - popup_shown());
	if (top != mover.top) {
		mover.top = top;
		gtk_widget_queue_draw(mover.popup_area);
	}
}

/* Scrolled so the folder i is in view. */
static void popup_reveal(int i) {
	if (i < mover.top) {
		popup_scroll_to(i);
	} else if (i >= mover.top + popup_shown()) {
		popup_scroll_to(i - popup_shown() + 1);
	}
}

/* An arrow row: up or down, greyed when there's no further to go. */
static void popup_arrow(cairo_t *cr, int w, int y, bool up) {
	int cx = w / 2, cy = y + ROW_H / 2;
	gem_black(cr);
	for (int k = 0; k < 5; k++) {
		int row = up ? cy - 2 + k : cy + 2 - k;
		gem_fill(cr, cx - k, row, 2 * k + 1, 1);
	}
	bool end = up ? mover.top == 0 :
		mover.top + popup_shown() >= (int)mover.mailboxes->len;
	if (end) {
		gem_grey_out(cr, 1, y, w - 4, ROW_H);
	}
}

static void paint_popup(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_frame(cr, 0, 0, w - 2, h - 2, 1);
	gem_fill(cr, 2, h - 2, w - 2, 2);
	gem_fill(cr, w - 2, 2, 2, h - 2);
	int y = 1;
	if (popup_scrolls()) {
		popup_arrow(cr, w - 2, y, true);
		y += ROW_H;
	}
	for (int r = 0; r < popup_shown(); r++, y += ROW_H) {
		int i = mover.top + r;
		struct folder *f = mover_folder(i);
		gem_black(cr);
		if (i == mover.hover) {
			gem_fill(cr, 1, y, w - 4, ROW_H);
			gem_white(cr);
		}
		if (i == mover.chosen) {
			cairo_set_line_width(cr, 2);
			cairo_move_to(cr, 4, y + ROW_H / 2.0);
			cairo_line_to(cr, 7, y + ROW_H / 2.0 + 3);
			cairo_line_to(cr, 12, y + ROW_H / 2.0 - 4);
			cairo_stroke(cr);
		}
		if (f != NULL) {
			clipped(cr, f->name, 18 + f->depth * 12, y, w - 22 - f->depth * 12,
				ROW_H);
		}
	}
	if (popup_scrolls()) {
		popup_arrow(cr, w - 2, y, false);
	}
}

static void draw_popup(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_popup, NULL);
}

/* The folder at y, an arrow, or -1. */
static int popup_row(double y) {
	int r = ((int)y - 1) / ROW_H;
	if (y < 1) {
		return -1;
	}
	if (popup_scrolls()) {
		if (r == 0) {
			return ARROW_UP;
		}
		if (r == POPUP_ROWS - 1) {
			return ARROW_DOWN;
		}
		r--;
	}
	return r < popup_shown() ? mover.top + r : -1;
}

static gboolean popup_scroll_tick(gpointer data) {
	popup_scroll_to(mover.top + (mover.arrow == ARROW_UP ? -1 : 1));
	return G_SOURCE_CONTINUE;
}

/* On an arrow, it scrolls, and keeps scrolling while the pointer stays. */
static void popup_set_arrow(int arrow) {
	if (arrow == mover.arrow) {
		return;
	}
	mover.arrow = arrow;
	if (mover.scroller != 0) {
		g_source_remove(mover.scroller);
		mover.scroller = 0;
	}
	if (arrow != 0) {
		popup_scroll_tick(NULL);
		mover.scroller = g_timeout_add(90, popup_scroll_tick, NULL);
	}
}

static void popup_motion(GtkEventControllerMotion *c, double x, double y,
		gpointer data) {
	int i = popup_row(y);
	popup_set_arrow(i == ARROW_UP || i == ARROW_DOWN ? i : 0);
	i = i >= 0 ? i : -1;
	if (i != mover.hover) {
		mover.hover = i;
		gtk_widget_queue_draw(mover.popup_area);
	}
}

static void popup_leave(GtkEventControllerMotion *c, gpointer data) {
	popup_set_arrow(0);
}

static gboolean popup_wheel(GtkEventControllerScroll *c, double dx, double dy,
		gpointer data) {
	popup_scroll_to(mover.top + (int)(dy * 3));
	return TRUE;
}

static void popup_choose(int i) {
	if (i >= 0) {
		mover.chosen = i;
	}
	gtk_popover_popdown(GTK_POPOVER(mover.popover));
	gtk_widget_queue_draw(mover.area);
}

static void popup_released(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	int i = popup_row(y);
	if (i == ARROW_UP || i == ARROW_DOWN) {
		return; /* the arrows scroll; they don't choose */
	}
	popup_choose(i);
}

/* Up and Down go through the folders, Return chooses. */
static gboolean popup_key(GtkEventControllerKey *c, guint keyval,
		guint keycode, GdkModifierType state, gpointer data) {
	int n = (int)mover.mailboxes->len;
	int i = mover.hover >= 0 ? mover.hover : mover.chosen;
	switch (keyval) {
	case GDK_KEY_Up:
	case GDK_KEY_Down:
		i = CLAMP(i + (keyval == GDK_KEY_Up ? -1 : 1), 0, n - 1);
		break;
	case GDK_KEY_Page_Up:
	case GDK_KEY_Page_Down:
		i = CLAMP(i + (keyval == GDK_KEY_Page_Up ? -1 : 1) * popup_shown(), 0,
			n - 1);
		break;
	case GDK_KEY_Home:
		i = 0;
		break;
	case GDK_KEY_End:
		i = n - 1;
		break;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
	case GDK_KEY_space:
		popup_choose(i);
		return TRUE;
	default:
		return FALSE;
	}
	mover.hover = i;
	popup_reveal(i);
	gtk_widget_queue_draw(mover.popup_area);
	return TRUE;
}

static void popup_closed(GtkPopover *popover, gpointer data) {
	popup_set_arrow(0);
}

static void mover_close(bool ok) {
	if (mover.window == NULL) {
		return;
	}
	struct folder *dest = ok ? mover_folder(mover.chosen) : NULL;
	if (dest != NULL) {
		g_free(last_move);
		last_move = g_strdup(dest->mailbox);
	}
	gtk_window_destroy(GTK_WINDOW(mover.window));
	if (dest != NULL) {
		move_to(dest);
	}
}

static void mover_destroyed(GtkWidget *w, gpointer data) {
	popup_set_arrow(0);
	g_ptr_array_free(mover.mailboxes, TRUE);
	g_array_unref(mover.hits);
	g_free(mover.title);
	memset(&mover, 0, sizeof(mover));
}

static void open_popup(void) {
	const struct hit *h = NULL;
	for (guint i = 0; i < mover.hits->len; i++) {
		if (g_array_index(mover.hits, struct hit, i).action == ACT_MOVE_POPUP) {
			h = &g_array_index(mover.hits, struct hit, i);
		}
	}
	if (h == NULL) {
		return;
	}
	/* Over the button, as wide, the chosen folder where the button was. */
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(mover.popup_area),
		h->w + 2);
	int rows = popup_scrolls() ? POPUP_ROWS : (int)mover.mailboxes->len;
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(mover.popup_area),
		rows * ROW_H + 4);
	GdkRectangle at = { h->x, h->y, h->w, 1 };
	gtk_popover_set_pointing_to(GTK_POPOVER(mover.popover), &at);
	mover.hover = mover.chosen;
	/* The chosen folder in the middle, where it can be. */
	mover.top = -1;
	popup_scroll_to(mover.chosen - popup_shown() / 2);
	gtk_popover_popup(GTK_POPOVER(mover.popover));
}

static void mover_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	const struct hit *h = hit_at(mover.hits, x, y);
	if (h == NULL) {
		return;
	}
	if (h->action == ACT_MOVE_POPUP) {
		open_popup();
	} else {
		mover_close(h->action == ACT_MOVE_OK);
	}
}

/* Return moves, Escape cancels; Up and Down change the folder. */
static gboolean mover_key(GtkEventControllerKey *c, guint keyval,
		guint keycode, GdkModifierType state, gpointer data) {
	switch (keyval) {
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		mover_close(true);
		return TRUE;
	case GDK_KEY_Escape:
		mover_close(false);
		return TRUE;
	case GDK_KEY_Up:
	case GDK_KEY_Down:
		mover.chosen = CLAMP(mover.chosen + (keyval == GDK_KEY_Up ? -1 : 1), 0,
			(int)mover.mailboxes->len - 1);
		gtk_widget_queue_draw(mover.area);
		return TRUE;
	case GDK_KEY_space:
		open_popup();
		return TRUE;
	}
	return FALSE;
}

static void move_dialog(void) {
	struct folder *f = current_folder();
	GPtrArray *t = f != NULL ? targets() : NULL;
	guint n = t != NULL ? t->len : 0;
	if (t != NULL) {
		g_ptr_array_free(t, TRUE);
	}
	if (n == 0 || mover.window != NULL) {
		return;
	}
	mover.mailboxes = g_ptr_array_new_with_free_func(g_free);
	for (guint i = 0; i < ui.folder_list->len; i++) {
		struct folder *o = ui.folder_list->pdata[i];
		if (o != f && o->selectable) {
			if (g_strcmp0(o->mailbox, last_move) == 0) {
				mover.chosen = mover.mailboxes->len;
			}
			g_ptr_array_add(mover.mailboxes, g_strdup(o->mailbox));
		}
	}
	if (mover.mailboxes->len == 0) {
		g_ptr_array_free(mover.mailboxes, TRUE);
		mover.mailboxes = NULL;
		set_status("There's no other folder to move to");
		return;
	}
	mover.title = n > 1 ? g_strdup_printf("Move %u messages to:", n) :
		g_strdup("Move this message to:");
	mover.hits = g_array_new(FALSE, FALSE, sizeof(struct hit));
	mover.hover = -1;
	mover.window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(mover.window), "Move to Folder");
	gtk_window_set_transient_for(GTK_WINDOW(mover.window), GTK_WINDOW(ui.window));
	gtk_window_set_application(GTK_WINDOW(mover.window), ui.app);
	gtk_window_set_modal(GTK_WINDOW(mover.window), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(mover.window), FALSE);
	gtk_widget_add_css_class(mover.window, "gem-mail");
	mover.area = pixel_area(320, 3 + 2 * PAD + ROW_H + PAD + BUTTON_H + 2 +
		2 * PAD + BUTTON_H + 2 * PAD + 3, draw_mover, G_CALLBACK(mover_pressed));
	gtk_window_set_child(GTK_WINDOW(mover.window), mover.area);
	/* Something has the focus, for the pop-up to hand it back to. */
	gtk_widget_set_focusable(mover.area, TRUE);

	mover.popover = gtk_popover_new();
	gtk_popover_set_has_arrow(GTK_POPOVER(mover.popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(mover.popover), GTK_POS_BOTTOM);
	gtk_widget_add_css_class(mover.popover, "gem-popup");
	gtk_widget_set_parent(mover.popover, mover.area);
	/* A popover goes before what it's on. */
	g_signal_connect_swapped(mover.area, "destroy",
		G_CALLBACK(gtk_widget_unparent), mover.popover);
	mover.popup_area = pixel_area(0, 0, draw_popup, NULL);
	GtkGesture *release = gtk_gesture_click_new();
	g_signal_connect(release, "released", G_CALLBACK(popup_released), NULL);
	gtk_widget_add_controller(mover.popup_area, GTK_EVENT_CONTROLLER(release));
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "enter", G_CALLBACK(popup_motion), NULL);
	g_signal_connect(motion, "motion", G_CALLBACK(popup_motion), NULL);
	g_signal_connect(motion, "leave", G_CALLBACK(popup_leave), NULL);
	gtk_widget_add_controller(mover.popup_area, motion);
	GtkEventController *wheel = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(wheel, "scroll", G_CALLBACK(popup_wheel), NULL);
	gtk_widget_add_controller(mover.popup_area, wheel);
	GtkEventController *popup_keys = gtk_event_controller_key_new();
	g_signal_connect(popup_keys, "key-pressed", G_CALLBACK(popup_key), NULL);
	gtk_widget_add_controller(mover.popover, popup_keys);
	g_signal_connect(mover.popover, "closed", G_CALLBACK(popup_closed), NULL);
	gtk_popover_set_child(GTK_POPOVER(mover.popover), mover.popup_area);

	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(mover_key), NULL);
	gtk_widget_add_controller(mover.window, keys);
	g_signal_connect(mover.window, "destroy", G_CALLBACK(mover_destroyed), NULL);
	gtk_window_present(GTK_WINDOW(mover.window));
	gtk_widget_grab_focus(mover.area);
}

static void run_action(enum action action, int index) {
	switch (action) {
	case ACT_BACK:
		show_list();
		break;
	case ACT_REPLY:
	case ACT_REPLY_ALL:
		if (ui.open != NULL) {
			compose(draft_reply(ui.open, ui.account->address,
				action == ACT_REPLY_ALL));
		}
		break;
	case ACT_FORWARD:
		if (ui.open != NULL) {
			compose(draft_forward(ui.open));
		}
		break;
	case ACT_DELETE:
	case ACT_ARCHIVE:
		take_away(action == ACT_ARCHIVE);
		break;
	case ACT_UNREAD:
		toggle_seen();
		break;
	case ACT_SHOW_IMAGES:
		ui.remote_images = true;
		gtk_widget_queue_draw(ui.header);
		show_body();
		app_menu_update(ui.menu);
		break;
	case ACT_ALWAYS_IMAGES:
	case ACT_STOP_IMAGES:
		if (ui.open == NULL || ui.open->sender == NULL) {
			break;
		}
		set_image_sender(ui.open->sender, action == ACT_ALWAYS_IMAGES);
		ui.remote_images = action == ACT_ALWAYS_IMAGES;
		set_status(action == ACT_ALWAYS_IMAGES ?
			"Images from %s will show" : "Images from %s will ask first",
			ui.open->sender);
		gtk_widget_queue_draw(ui.header);
		show_body();
		app_menu_update(ui.menu);
		break;
	case ACT_ATTACHMENT:
		save_attachment(index);
		break;
	case ACT_NEW:
		if (ui.account != NULL) {
			compose(draft_new());
		}
		break;
	case ACT_GET_MAIL:
		get_mail();
		break;
	case ACT_OPEN:
		open_message(ui.selected);
		break;
	case ACT_MOVE:
		move_dialog();
		break;
	case ACT_SELECT_ALL:
		if (ui.open == NULL) {
			pick_all();
		}
		break;
	case ACT_QUIT:
		gtk_window_destroy(GTK_WINDOW(ui.window));
		break;
	default:
		break;
	}
}

/* Up and Down choose a message, Return opens it, Escape goes back; A
 * archives, Delete deletes. */
static gboolean window_key(GtkEventControllerKey *c, guint keyval,
		guint keycode, GdkModifierType state, gpointer data) {
	if (ui.password != NULL) {
		return FALSE;
	}
	bool listing = ui.open == NULL;
	switch (keyval) {
	case GDK_KEY_Up:
	case GDK_KEY_Down:
		if (!listing || message_count() == 0) {
			return FALSE;
		}
	{
		int i = CLAMP(ui.selected + (keyval == GDK_KEY_Up ? -1 : 1), 0,
			message_count() - 1);
		if (state & GDK_SHIFT_MASK) {
			pick_range(i, false);
		} else {
			pick_one(i);
		}
		reveal();
		picked_changed();
		return TRUE;
	}
	case GDK_KEY_Page_Up:
	case GDK_KEY_Page_Down:
		if (!listing) {
			return FALSE;
		}
		scroll_to(ui.top + (keyval == GDK_KEY_Page_Up ? -1 : 1) * visible_rows());
		return TRUE;
	case GDK_KEY_Return:
	case GDK_KEY_KP_Enter:
		if (listing) {
			open_message(ui.selected);
			return TRUE;
		}
		return FALSE;
	case GDK_KEY_Escape:
	case GDK_KEY_BackSpace:
		if (!listing) {
			show_list();
			return TRUE;
		}
		if (keyval == GDK_KEY_Escape && g_hash_table_size(ui.picked) > 1) {
			pick_one(ui.selected);
			picked_changed();
			return TRUE;
		}
		return FALSE;
	case GDK_KEY_Delete:
		take_away(false);
		return TRUE;
	case GDK_KEY_a:
		if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
			return FALSE;
		}
		take_away(true);
		return TRUE;
	case GDK_KEY_m:
		if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) {
			return FALSE;
		}
		move_dialog();
		return TRUE;
	}
	return FALSE;
}

/* ---- Menus --------------------------------------------------------------- */

static void build_menus(struct app_menu *m, void *data) {
	bool account = ui.account != NULL;
	GPtrArray *t = ui.messages != NULL ? targets() : g_ptr_array_new();
	bool unread = false;
	for (guint k = 0; k < t->len; k++) {
		unread |= !((struct summary *)t->pdata[k])->seen;
	}
	uint32_t have = t->len > 0 ? 0 : APP_MENU_DISABLED;
	struct summary *s = selected_summary();
	uint32_t open = ui.open != NULL ? 0 : APP_MENU_DISABLED;
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_NEW, "New Message", "^N",
		account ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_GET_MAIL, "Get Mail", "F5",
		account ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_QUIT, "Close", "^Q", 0);
	app_menu_add_menu(m, "Message");
	app_menu_add_item(m, ACT_OPEN, "Open", "Return",
		ui.open == NULL && s != NULL ? 0 : APP_MENU_DISABLED);
	app_menu_add_item(m, ACT_BACK, "Back to List", "Esc", open);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_REPLY, "Reply", "^R", open);
	app_menu_add_item(m, ACT_REPLY_ALL, "Reply All", "^Shift+R", open);
	app_menu_add_item(m, ACT_FORWARD, "Forward", "^L", open);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_SELECT_ALL, "Select All", "^A",
		ui.open == NULL && message_count() > 0 ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_UNREAD, unread ? "Mark as Read" :
		"Mark as Unread", "^U", have);
	app_menu_add_item(m, ACT_ARCHIVE, "Archive", "A", have);
	app_menu_add_item(m, ACT_MOVE, "Move to Folder...", "M", have);
	app_menu_add_item(m, ACT_DELETE, "Delete", "Del", have);
	app_menu_add_separator(m);
	bool remote = ui.open != NULL && ui.open->sender != NULL &&
		has_remote(ui.open->html);
	app_menu_add_item(m, ACT_SHOW_IMAGES, "Show Images", NULL,
		remote && !ui.remote_images ? 0 : APP_MENU_DISABLED);
	if (sender_trusted()) {
		app_menu_add_item(m, ACT_STOP_IMAGES, "Stop Showing Images", NULL,
			remote ? 0 : APP_MENU_DISABLED);
	} else {
		app_menu_add_item(m, ACT_ALWAYS_IMAGES, "Always Show Sender's Images",
			NULL, remote ? 0 : APP_MENU_DISABLED);
	}
	g_ptr_array_free(t, TRUE);
}

static void menu_activate(uint32_t id, void *data) {
	if (ui.password == NULL) {
		run_action(id, 0);
	}
}

static gboolean shortcut(GtkWidget *w, GVariant *args, gpointer data) {
	run_action(GPOINTER_TO_INT(data), 0);
	return TRUE;
}

static void add_shortcuts(GtkWidget *window, const struct key *keys, int n) {
	GtkEventController *c = gtk_shortcut_controller_new();
	gtk_event_controller_set_propagation_phase(c, GTK_PHASE_CAPTURE);
	for (int i = 0; i < n; i++) {
		gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(c),
			gtk_shortcut_new(gtk_shortcut_trigger_parse_string(keys[i].trigger),
				gtk_callback_action_new(shortcut,
					GINT_TO_POINTER(keys[i].action), NULL)));
	}
	gtk_widget_add_controller(window, c);
}

/* ---- The info line ------------------------------------------------------- */

static void paint_info(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	if (ui.status != NULL) {
		clipped(cr, ui.status, PAD, 1, w - 2 * PAD, h - 1);
	}
}

static void draw_info(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_info, NULL);
}

/* ---- A GEM dialog with a field (the password) ----------------------------- */

struct dialog {
	GtkWidget *window, *area, *field;
	char **lines;
	const char *ok;
	GArray *hits;
	void (*answered)(struct dialog *d, bool ok);
};

static int dialog_field_y(struct dialog *d) {
	return 3 + 2 * PAD + g_strv_length(d->lines) * ROW_H + PAD / 2;
}

static void paint_dialog(cairo_t *cr, int w, int h, void *data) {
	struct dialog *d = data;
	g_array_set_size(d->hits, 0);
	gem_black(cr);
	gem_frame(cr, 0, 0, w, h, 1);
	gem_frame(cr, 3, 3, w - 6, h - 6, 2);
	for (int i = 0; d->lines[i] != NULL; i++) {
		gem_text(cr, d->lines[i], 3 + 2 * PAD, 3 + 2 * PAD + i * ROW_H, ROW_H);
	}
	int by = h - 3 - 2 * PAD - BUTTON_H;
	int bw = MAX(gem_text_width(cr, "Cancel"), gem_text_width(cr, d->ok)) + 2 * PAD;
	int bx = w - 3 - 2 * PAD - 2 * bw - PAD;
	for (int i = 0; i < 2; i++) {
		const char *label = i == 0 ? "Cancel" : d->ok;
		int x = bx + i * (bw + PAD);
		gem_frame(cr, x, by, bw, BUTTON_H, i == 1 ? 2 : 1);
		gem_text(cr, label, x + (bw - gem_text_width(cr, label)) / 2, by, BUTTON_H);
		add_hit(d->hits, x, by, bw, BUTTON_H,
			i == 0 ? ACT_PASSWORD_CANCEL : ACT_PASSWORD_OK, 0);
	}
}

static void draw_dialog(GtkDrawingArea *area, cairo_t *cr, int w, int h,
		gpointer data) {
	gem_draw_pixelated(cr, w, h, paint_dialog, data);
}

static void dialog_close(struct dialog *d, bool ok) {
	void (*answered)(struct dialog *, bool) = d->answered;
	d->answered = NULL;
	if (answered != NULL) {
		answered(d, ok);
	}
	gtk_window_destroy(GTK_WINDOW(d->window));
}

static void dialog_destroyed(GtkWidget *w, struct dialog *d) {
	if (d->answered != NULL) {
		d->answered(d, false);
	}
	g_strfreev(d->lines);
	g_array_unref(d->hits);
	g_free(d);
}

static void dialog_pressed(GtkGestureClick *g, int n, double x, double y,
		struct dialog *d) {
	const struct hit *h = hit_at(d->hits, x, y);
	if (h != NULL) {
		dialog_close(d, h->action == ACT_PASSWORD_OK);
	}
}

static gboolean dialog_key(GtkEventControllerKey *c, guint keyval,
		guint keycode, GdkModifierType state, struct dialog *d) {
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		dialog_close(d, true);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape) {
		dialog_close(d, false);
		return TRUE;
	}
	return FALSE;
}

static struct dialog *dialog_new(const char *text, const char *ok,
		void (*answered)(struct dialog *, bool)) {
	struct dialog *d = g_new0(struct dialog, 1);
	d->lines = g_strsplit(text, "\n", -1);
	d->ok = ok;
	d->hits = g_array_new(FALSE, FALSE, sizeof(struct hit));
	d->answered = answered;
	d->window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(d->window), "GemMail");
	gtk_window_set_transient_for(GTK_WINDOW(d->window), GTK_WINDOW(ui.window));
	gtk_window_set_application(GTK_WINDOW(d->window), ui.app);
	gtk_window_set_modal(GTK_WINDOW(d->window), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(d->window), FALSE);
	gtk_widget_add_css_class(d->window, "gem-mail");
	GtkWidget *overlay = gtk_overlay_new();
	d->area = gtk_drawing_area_new();
	int w = 380, fy = dialog_field_y(d);
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(d->area), w);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(d->area),
		fy + FIELD_H + 2 * PAD + BUTTON_H + 2 * PAD + 3);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->area), draw_dialog, d,
		NULL);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), d->area);
	d->field = gtk_password_entry_new();
	gtk_widget_add_css_class(d->field, "gem-field");
	gtk_widget_set_halign(d->field, GTK_ALIGN_START);
	gtk_widget_set_valign(d->field, GTK_ALIGN_START);
	gtk_widget_set_margin_start(d->field, 3 + 2 * PAD);
	gtk_widget_set_margin_top(d->field, fy);
	gtk_widget_set_size_request(d->field, w - 2 * (3 + 2 * PAD), FIELD_H);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->field);
	gtk_window_set_child(GTK_WINDOW(d->window), overlay);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(dialog_pressed), d);
	gtk_widget_add_controller(d->area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(dialog_key), d);
	gtk_widget_add_controller(d->window, keys);
	g_signal_connect(d->window, "destroy", G_CALLBACK(dialog_destroyed), d);
	gtk_window_present(GTK_WINDOW(d->window));
	gtk_widget_grab_focus(d->field);
	return d;
}

static void password_answered(struct dialog *d, bool ok) {
	ui.password = NULL;
	if (ok) {
		mail_set_password(ui.mail, gtk_editable_get_text(GTK_EDITABLE(d->field)));
	} else {
		mail_cancel_password(ui.mail);
	}
	gtk_editable_set_text(GTK_EDITABLE(d->field), "");
}

/* Asked for by hand: why says why (the command failed, the server said
 * no, the password manager had none). */
static void ask_by_hand(const char *why) {
	char *text = g_strdup_printf("%s\n\nPassword for %s:",
		why != NULL ? why : "The mail server needs your password.",
		ui.account->user);
	struct dialog *d = dialog_new(text, "OK", password_answered);
	ui.password = d->window;
	g_free(text);
}

/* From a password manager ([Passwords], as GemWeb's): the login for the
 * site whose username is the account's, unlocking it first if it's
 * locked. The unlocked session is kept (in memory) while GemMail runs. */

static void lookup_login(void);

static void unlocked(const char *session, const char *error, void *data);

static void unlock_answered(struct dialog *d, bool ok) {
	ui.password = NULL;
	if (!ok) {
		mail_cancel_password(ui.mail);
		return;
	}
	char *master = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->field)));
	gtk_editable_set_text(GTK_EDITABLE(d->field), "");
	set_status("Unlocking your passwords...");
	passwords_unlock(ui.account->unlock_command, master, NULL, unlocked, NULL);
}

static void ask_unlock(const char *error) {
	char *text = g_strdup_printf("%s\n\nMaster password:",
		error != NULL ? error : "Your passwords are locked.");
	struct dialog *d = dialog_new(text, "Unlock", unlock_answered);
	ui.password = d->window;
	g_free(text);
}

static void unlocked(const char *session, const char *error, void *data) {
	if (session == NULL) {
		ask_unlock(error);
		return;
	}
	secret_free(ui.session);
	ui.session = g_strdup(session);
	lookup_login();
}

static void found_login(enum passwords_result result, GPtrArray *logins,
		const char *error, void *data) {
	struct account *a = ui.account;
	if (result == PASSWORDS_LOCKED) {
		if (a->unlock_command != NULL) {
			ask_unlock(NULL);
		} else {
			ask_by_hand("Your passwords are locked.");
		}
		return;
	}
	if (result == PASSWORDS_FAILED) {
		char *why = g_strdup_printf("Passwords: %s", error);
		ask_by_hand(why);
		g_free(why);
		return;
	}
	struct login *login = NULL;
	for (guint i = 0; i < logins->len && login == NULL; i++) {
		struct login *l = logins->pdata[i];
		if (g_ascii_strcasecmp(l->username, a->user) == 0) {
			login = l;
		}
	}
	if (login == NULL && logins->len == 1) {
		login = logins->pdata[0];
	}
	if (login == NULL) {
		char *why = g_strdup_printf("No saved password for %s at %s.",
			a->user, a->site);
		ask_by_hand(why);
		g_free(why);
		return;
	}
	set_status("Connecting to %s...", a->imap.host);
	mail_set_password(ui.mail, login->password);
}

static void lookup_login(void) {
	set_status("Looking up your password...");
	passwords_lookup(ui.account->login_command, ui.session, ui.account->site,
		NULL, found_login, NULL);
}

/* The mail thread needs the password: why says why, or if nothing's gone
 * wrong, it's looked up in the password manager, if there is one. */
static void ask_password(const char *why, void *data) {
	if (why == NULL && ui.account->login_command != NULL) {
		lookup_login();
		return;
	}
	ask_by_hand(why);
}

/* ---- Composing ------------------------------------------------------------ */

struct compose {
	GtkWidget *window, *bar, *to, *cc, *subject, *body, *info;
	char *in_reply_to, *references;
	char *status;
	bool sending;
	bool closed; /* its window's gone, while a send finishes */
	GArray *hits;
	struct app_menu *menu;
};

static void compose_free(struct compose *c) {
	g_free(c->in_reply_to);
	g_free(c->references);
	g_free(c->status);
	g_array_unref(c->hits);
	g_free(c);
}

static void compose_status(struct compose *c, const char *s) {
	g_free(c->status);
	c->status = g_strdup(s);
	gtk_widget_queue_draw(c->info);
}

static void paint_bar(cairo_t *cr, int w, int h, void *data) {
	struct compose *c = data;
	g_array_set_size(c->hits, 0);
	int x = PAD;
	x += button(cr, c->hits, x, PAD, "Send", ACT_SEND, 0, true) + PAD;
	button(cr, c->hits, x, PAD, "Cancel", ACT_CANCEL, 0, false);
	if (c->sending) {
		gem_grey_out(cr, 0, 0, w, h);
	}
}

static void draw_bar(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_bar, d);
}

static void paint_compose_info(cairo_t *cr, int w, int h, void *data) {
	struct compose *c = data;
	gem_black(cr);
	gem_fill(cr, 0, 0, w, 1);
	if (c->status != NULL) {
		clipped(cr, c->status, PAD, 1, w - 2 * PAD, h - 1);
	}
}

static void draw_compose_info(GtkDrawingArea *a, cairo_t *cr, int w, int h,
		gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_compose_info, d);
}

static void paint_label(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_text(cr, data, PAD, 0, h);
}

static void draw_label(GtkDrawingArea *a, cairo_t *cr, int w, int h,
		gpointer d) {
	gem_draw_pixelated(cr, w, h, paint_label, d);
}

static void sent(const char *error, void *data) {
	struct compose *c = data;
	c->sending = false;
	if (c->closed) {
		set_status("%s", error != NULL ? error : "Sent.");
		compose_free(c);
		return;
	}
	gtk_widget_queue_draw(c->bar);
	if (error != NULL && !g_str_has_prefix(error, "Sent, but")) {
		compose_status(c, error);
		gtk_widget_set_sensitive(c->window, TRUE);
		return;
	}
	set_status("%s", error != NULL ? error : "Sent.");
	gtk_window_destroy(GTK_WINDOW(c->window));
	struct folder *f = current_folder();
	if (f != NULL && f->role == ROLE_SENT) {
		load_messages();
	}
}

static void send_message(struct compose *c) {
	if (c->sending) {
		return;
	}
	struct draft d = { 0 };
	d.to = (char *)gtk_editable_get_text(GTK_EDITABLE(c->to));
	d.cc = (char *)gtk_editable_get_text(GTK_EDITABLE(c->cc));
	d.subject = (char *)gtk_editable_get_text(GTK_EDITABLE(c->subject));
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->body));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	d.body = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	d.in_reply_to = c->in_reply_to;
	d.references = c->references;
	char **recipients = NULL, *error = NULL;
	GBytes *message = draft_build(&d, ui.account->from, &recipients, &error);
	g_free(d.body);
	if (message == NULL) {
		compose_status(c, error);
		g_free(error);
		return;
	}
	struct folder *sent_folder = folder_with_role(ROLE_SENT);
	c->sending = true;
	gtk_widget_queue_draw(c->bar);
	compose_status(c, "Sending...");
	mail_send(ui.mail, message, ui.account->address, recipients,
		sent_folder != NULL ? sent_folder->mailbox : NULL, sent, c);
	g_bytes_unref(message);
	g_strfreev(recipients);
}

static void compose_run(struct compose *c, enum action action) {
	if (action == ACT_SEND) {
		send_message(c);
	} else if (action == ACT_CANCEL && !c->sending) {
		gtk_window_destroy(GTK_WINDOW(c->window));
	}
}

static void bar_pressed(GtkGestureClick *g, int n, double x, double y,
		struct compose *c) {
	const struct hit *h = hit_at(c->hits, x, y);
	if (h != NULL) {
		compose_run(c, h->action);
	}
}

static void compose_menus(struct app_menu *m, void *data) {
	app_menu_add_menu(m, "Message");
	app_menu_add_item(m, ACT_SEND, "Send", "^Return", 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CANCEL, "Discard", "^W", 0);
}

static void compose_menu_activate(uint32_t id, void *data) {
	compose_run(data, id);
}

static gboolean compose_shortcut(GtkWidget *w, GVariant *args, gpointer data) {
	struct compose *c = g_object_get_data(G_OBJECT(w), "compose");
	compose_run(c, GPOINTER_TO_INT(data));
	return TRUE;
}

static void compose_destroyed(GtkWidget *w, struct compose *c) {
	if (c->sending) {
		/* The send finishes, and says how it went in the main window. */
		c->closed = true;
		return;
	}
	compose_free(c);
}

static GtkWidget *field_row(const char *label, GtkWidget **entry,
		const char *text) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_margin_end(row, PAD);
	gtk_widget_set_margin_bottom(row, PAD / 2);
	GtkWidget *l = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(l), 84);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(l), FIELD_H);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(l), draw_label,
		(gpointer)label, NULL);
	*entry = gtk_entry_new();
	gtk_widget_add_css_class(*entry, "gem-field");
	gtk_widget_set_hexpand(*entry, TRUE);
	gtk_editable_set_text(GTK_EDITABLE(*entry), text != NULL ? text : "");
	gtk_box_append(GTK_BOX(row), l);
	gtk_box_append(GTK_BOX(row), *entry);
	return row;
}

static void compose(struct draft *d) {
	struct compose *c = g_new0(struct compose, 1);
	c->hits = g_array_new(FALSE, FALSE, sizeof(struct hit));
	c->in_reply_to = g_strdup(d->in_reply_to);
	c->references = g_strdup(d->references);
	c->window = gtk_window_new();
	gtk_window_set_application(GTK_WINDOW(c->window), ui.app);
	gtk_window_set_title(GTK_WINDOW(c->window),
		d->subject[0] ? d->subject : "New Message");
	gtk_window_set_default_size(GTK_WINDOW(c->window), 720, 560);
	gtk_widget_add_css_class(c->window, "gem-mail");
	g_object_set_data(G_OBJECT(c->window), "compose", c);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	c->bar = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(c->bar),
		BUTTON_H + 2 * PAD);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(c->bar), draw_bar, c, NULL);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(bar_pressed), c);
	gtk_widget_add_controller(c->bar, GTK_EVENT_CONTROLLER(click));
	gtk_box_append(GTK_BOX(box), c->bar);
	gtk_box_append(GTK_BOX(box), field_row("To:", &c->to, d->to));
	gtk_box_append(GTK_BOX(box), field_row("Cc:", &c->cc, d->cc));
	gtk_box_append(GTK_BOX(box), field_row("Subject:", &c->subject, d->subject));

	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_widget_add_css_class(scrolled, "gem-body-frame");
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	c->body = gtk_text_view_new();
	gtk_widget_add_css_class(c->body, "gem-body");
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(c->body), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(c->body), PAD);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(c->body), PAD);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(c->body), PAD);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->body)),
		d->body, -1);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), c->body);
	gtk_box_append(GTK_BOX(box), scrolled);

	c->info = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(c->info), INFO_H);
	gtk_widget_set_margin_top(c->info, PAD);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(c->info), draw_compose_info,
		c, NULL);
	gtk_box_append(GTK_BOX(box), c->info);
	gtk_window_set_child(GTK_WINDOW(c->window), box);

	GtkEventController *keys = gtk_shortcut_controller_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>Return"),
			gtk_callback_action_new(compose_shortcut,
				GINT_TO_POINTER(ACT_SEND), NULL)));
	gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
		gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>w"),
			gtk_callback_action_new(compose_shortcut,
				GINT_TO_POINTER(ACT_CANCEL), NULL)));
	gtk_widget_add_controller(c->window, keys);
	g_signal_connect(c->window, "destroy", G_CALLBACK(compose_destroyed), c);
	c->menu = app_menu_new(c->window, compose_menus, compose_menu_activate, c);
	gtk_window_present(GTK_WINDOW(c->window));
	/* A reply starts in the body, above the quote; a new one at To. */
	if (d->to[0] != '\0') {
		GtkTextIter startIter;
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->body));
		gtk_text_buffer_get_start_iter(buffer, &startIter);
		gtk_text_buffer_place_cursor(buffer, &startIter);
		gtk_widget_grab_focus(c->body);
	} else {
		gtk_widget_grab_focus(c->to);
	}
	draft_free(d);
}

/* ---- The window ---------------------------------------------------------- */

static void load_css(void) {
	const char *font = g_getenv("GEMWM_FONT") ? g_getenv("GEMWM_FONT") :
		"monospace";
	int size = g_getenv("GEMWM_FONT_SIZE") ? atoi(g_getenv("GEMWM_FONT_SIZE")) : 0;
	size = size > 0 ? size : 14;
	char *css = g_strdup_printf(
		"window.gem-mail { background: #fff; }"
		".gem-field { background: #fff; color: #000; border: 1px solid #000;"
		"  border-radius: 0; box-shadow: none; outline: none;"
		"  min-height: %dpx; padding: 0 6px; font-family: \"%s\";"
		"  font-size: %dpx; caret-color: #000; }"
		".gem-field:focus-within { box-shadow: none; outline: none; }"
		".gem-field text selection { background: #000; color: #fff; }"
		".gem-body-frame { border: 1px solid #000; margin: 0 8px; }"
		"textview.gem-body, textview.gem-body text { background: #fff;"
		"  color: #000; font-family: monospace; font-size: 15px; }"
		"textview.gem-body text selection { background: #000; color: #fff; }"
		"popover.gem-popup, popover.gem-popup > contents { background: none;"
		"  border: none; border-radius: 0; box-shadow: none; padding: 0;"
		"  margin: 0; }"
		"scrollbar { background: #fff; border-left: 1px solid #000; }"
		"scrollbar slider { background: #fff; border: 1px solid #000;"
		"  border-radius: 0; min-width: 15px; min-height: 24px; margin: 1px; }",
		FIELD_H - 2, font, size);
	GtkCssProvider *provider = gtk_css_provider_new();
	gtk_css_provider_load_from_string(provider, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
		GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(provider);
	g_free(css);
}

static GtkWidget *pixel_area(int w, int h, GtkDrawingAreaDrawFunc draw,
		GCallback pressed) {
	GtkWidget *area = gtk_drawing_area_new();
	if (w > 0) {
		gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), w);
	}
	if (h > 0) {
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), h);
	}
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
	if (pressed != NULL) {
		GtkGesture *click = gtk_gesture_click_new();
		g_signal_connect(click, "pressed", pressed, NULL);
		gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	}
	return area;
}

static void activate(GtkApplication *app, gpointer data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	ui.app = app;
	ui.picked = g_hash_table_new(NULL, NULL);
	load_css();
	ui.header_hits = g_array_new(FALSE, FALSE, sizeof(struct hit));
	ui.account = account_load(&ui.account_error);

	ui.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ui.window), "GemMail");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 1000, 680);
	gtk_widget_add_css_class(ui.window, "gem-mail");

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(main, TRUE);
	ui.folders = pixel_area(FOLDERS_W, 0, draw_folders,
		G_CALLBACK(folders_pressed));
	gtk_box_append(GTK_BOX(main), ui.folders);

	ui.stack = gtk_stack_new();
	gtk_widget_set_hexpand(ui.stack, TRUE);
	GtkWidget *listing = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	ui.list = pixel_area(0, 0, draw_list, G_CALLBACK(list_pressed));
	GtkGesture *release = gtk_gesture_click_new();
	g_signal_connect(release, "released", G_CALLBACK(list_released), NULL);
	gtk_widget_add_controller(ui.list, GTK_EVENT_CONTROLLER(release));
	GtkGesture *pull = gtk_gesture_drag_new();
	g_signal_connect(pull, "drag-begin", G_CALLBACK(list_drag_begin), NULL);
	g_signal_connect(pull, "drag-update", G_CALLBACK(list_drag_update), NULL);
	g_signal_connect(pull, "drag-end", G_CALLBACK(list_drag_end), NULL);
	gtk_widget_add_controller(ui.list, GTK_EVENT_CONTROLLER(pull));
	gtk_widget_set_hexpand(ui.list, TRUE);
	gtk_widget_set_focusable(ui.list, TRUE);
	GtkEventController *wheel = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(wheel, "scroll", G_CALLBACK(list_scrolled), NULL);
	gtk_widget_add_controller(ui.list, wheel);
	ui.scroll = pixel_area(GADGET + 1, 0, draw_scroll, G_CALLBACK(scroll_pressed));
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-update", G_CALLBACK(scroll_dragged), NULL);
	g_signal_connect(drag, "drag-end", G_CALLBACK(scroll_released), NULL);
	gtk_widget_add_controller(ui.scroll, GTK_EVENT_CONTROLLER(drag));
	gtk_box_append(GTK_BOX(listing), ui.list);
	gtk_box_append(GTK_BOX(listing), ui.scroll);
	gtk_stack_add_named(GTK_STACK(ui.stack), listing, "list");

	GtkWidget *reading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	ui.header = pixel_area(0, 120, draw_header, G_CALLBACK(header_pressed));
	ui.view = view_new();
	gtk_box_append(GTK_BOX(reading), ui.header);
	gtk_box_append(GTK_BOX(reading), ui.view);
	gtk_stack_add_named(GTK_STACK(ui.stack), reading, "message");
	gtk_box_append(GTK_BOX(main), ui.stack);

	ui.info = pixel_area(0, INFO_H, draw_info, NULL);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), main);
	ui.ghost = pixel_area(0, 0, draw_ghost, NULL);
	gtk_widget_set_can_target(ui.ghost, FALSE);
	gtk_widget_set_visible(ui.ghost, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ui.ghost);
	gtk_box_append(GTK_BOX(outer), overlay);
	gtk_box_append(GTK_BOX(outer), ui.info);
	gtk_window_set_child(GTK_WINDOW(ui.window), outer);

	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(window_key), NULL);
	gtk_widget_add_controller(ui.window, keys);
	static const struct key shortcuts[] = {
		{ "<Control>n", ACT_NEW }, { "F5", ACT_GET_MAIL },
		{ "<Control>q", ACT_QUIT }, { "<Control>r", ACT_REPLY },
		{ "<Control><Shift>r", ACT_REPLY_ALL }, { "<Control>l", ACT_FORWARD },
		{ "<Control>u", ACT_UNREAD }, { "<Control>a", ACT_SELECT_ALL },
	};
	add_shortcuts(ui.window, shortcuts, G_N_ELEMENTS(shortcuts));
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);
	gtk_window_present(GTK_WINDOW(ui.window));
	gtk_widget_grab_focus(ui.list);

	if (ui.account == NULL) {
		set_status("No mail account");
		return;
	}
	ui.mail = mail_new(ui.account, ask_password, NULL);
	ui.cache = cache_open();
	GPtrArray *cached = cache_folders(ui.cache);
	if (cached != NULL) {
		show_folders(cached); /* and the Inbox, as it was */
		folders_free(cached);
	}
	ui.loading = true; /* until the first folder's listed */
	set_status("Connecting to %s...", ui.account->imap.host);
	mail_list_folders(ui.mail, got_folders, NULL);
	g_timeout_add_seconds(REFRESH, refresh, NULL);
}

int main(int argc, char *argv[]) {
	g_mime_init();
	GtkApplication *app = gtk_application_new("org.gemwm.GemMail",
		G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
