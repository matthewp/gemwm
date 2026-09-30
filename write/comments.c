#include <string.h>
#include "comments.h"
#include "edit/commands.h"
#include "gem-scrollbar.h"
#include "gem-ui.h"
#include "settings.h"

#define DEFAULT_W 280
#define MIN_W 200
#define MIN_PAGE 320       /* the widest the panel goes leaves this for the page */
#define HANDLE_W 5
#define HEADER_H (GEM_ROW_H + 2)
#define CARD_HEAD_H (2 * GEM_ROW_H)
#define BUTTONS_H (GEM_BUTTON_H + 2 * 3)
#define INDENT 16          /* a reply's */

enum { HIT_CLOSE, HIT_REPLY, HIT_DELETE, HIT_BUTTON };

struct comments {
	WpPageView *view;
	WpEditor *ed;
	WpDocument *doc;
	comments_run_fn run;
	void (*shown_changed)(void *data);
	void *data;

	GtkWidget *panel, *body, *header, *cards, *scroller, *empty;
	GArray *header_hits;
	GtkWidget *draft, *draft_text;
	WpPos draft_a, draft_b;
	bool drafting, shown;
	guint refresh_id;
	size_t count;           /* threads last shown */
	int width, drag_width;
	double drag_x;
};

/* A card's header: who, when, and its buttons. */
struct card {
	struct comments *c;
	uint32_t id;
	bool reply;
	char *author, *when;
	GArray *hits;
};

/* A row of buttons at the foot of a draft or reply. */
struct buttons {
	const char *labels[2];
	void (*pressed)(int which, void *data);
	void *data;
	GArray *hits;
};

static void rebuild(struct comments *c);

/* ---- Pieces ---------------------------------------------------------------- */

static char *pretty_date(const char *iso) {
	GTimeZone *tz = g_time_zone_new_local();
	GDateTime *dt = iso != NULL && iso[0] != '\0' ?
		g_date_time_new_from_iso8601(iso, tz) : NULL;
	char *s = dt != NULL ? g_date_time_format(dt, "%-d %b %Y, %H:%M") :
		g_strdup(iso != NULL ? iso : "");
	if (dt != NULL) {
		g_date_time_unref(dt);
	}
	g_time_zone_unref(tz);
	return s;
}

static char *text_view_text(GtkTextView *tv) {
	GtkTextIter s, e;
	gtk_text_buffer_get_bounds(gtk_text_view_get_buffer(tv), &s, &e);
	return gtk_text_buffer_get_text(gtk_text_view_get_buffer(tv), &s, &e, FALSE);
}

/* A text view reports the height its layout had when last validated, which
 * happens in its own allocation: a new one is measured 0 tall, then finds
 * its height and asks again, and GTK drops that request, made mid-layout.
 * The height it finds goes into its vertical adjustment, so that changing
 * is the cue to ask again, from an idle. */
static gboolean text_view_resize_idle(gpointer tv) {
	g_object_set_data(G_OBJECT(tv), "gw-resize-idle", NULL);
	gtk_widget_queue_resize(tv);
	g_object_unref(tv);
	return G_SOURCE_REMOVE;
}

static void text_view_height_changed(GtkAdjustment *adj, gpointer tv) {
	if (g_object_get_data(G_OBJECT(tv), "gw-resize-idle") == NULL) {
		guint id = g_idle_add(text_view_resize_idle, g_object_ref(tv));
		g_object_set_data(G_OBJECT(tv), "gw-resize-idle", GUINT_TO_POINTER(id));
	}
}

static GtkWidget *text_view_new(void) {
	GtkWidget *tv = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(tv), FALSE);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 3);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 3);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 4);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 4);
	gtk_widget_add_css_class(tv, "gem-text");
	gtk_widget_add_css_class(tv, "gw-edit");
	GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tv));
	if (adj == NULL) {
		adj = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
		gtk_scrollable_set_vadjustment(GTK_SCROLLABLE(tv), adj);
	}
	g_signal_connect_object(adj, "changed", G_CALLBACK(text_view_height_changed),
		tv, 0);
	return tv;
}

static void paint_buttons(cairo_t *cr, int w, int h, void *data) {
	struct buttons *b = data;
	g_array_set_size(b->hits, 0);
	int bw = MAX(gem_button_width(b->labels[0]), gem_button_width(b->labels[1]));
	int x = w - 2 * bw - GEM_PAD - 2;
	for (int i = 0; i < 2; i++) {
		gem_button(cr, b->hits, x + i * (bw + GEM_PAD), 3, bw, b->labels[i], i,
			i == 1);
	}
}

static void buttons_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct buttons *b = data;
	const struct gem_hit *hit = gem_hit_at(b->hits, x, y);
	if (hit != NULL) {
		b->pressed(hit->id, b->data);
	}
}

static void buttons_free(gpointer data) {
	struct buttons *b = data;
	g_array_unref(b->hits);
	g_free(b);
}

/* Cancel and the action, at the right. */
static GtkWidget *button_row(const char *action,
		void (*pressed)(int which, void *data), void *data) {
	struct buttons *b = g_new0(struct buttons, 1);
	b->labels[0] = "Cancel";
	b->labels[1] = action;
	b->pressed = pressed;
	b->data = data;
	b->hits = gem_hits_new();
	GtkWidget *area = gem_pixel_area_new(0, BUTTONS_H, paint_buttons, b);
	g_object_set_data_full(G_OBJECT(area), "gw-buttons", b, buttons_free);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(buttons_pressed), b);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	return area;
}

/* ---- The panel's header ---------------------------------------------------- */

/* Like a window's: a close box, and the title in the middle. */
static void paint_header(cairo_t *cr, int w, int h, void *data) {
	struct comments *c = data;
	g_array_set_size(c->header_hits, 0);
	gem_black(cr);
	gem_fill(cr, 0, h - 1, w, 1);
	gem_frame(cr, 0, 0, GEM_ROW_H + 1, h, 1);
	for (int i = 4; i < GEM_ROW_H - 3; i++) {
		gem_fill(cr, i, i, 1, 1);
		gem_fill(cr, GEM_ROW_H - i, i, 1, 1);
	}
	gem_hit_add(c->header_hits, 0, 0, GEM_ROW_H + 1, h, HIT_CLOSE, 0);
	const char *title = "Comments";
	gem_text(cr, title, (w - (int)gem_text_width(cr, title)) / 2, 1, GEM_ROW_H);
}

static void header_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct comments *c = data;
	const struct gem_hit *hit = gem_hit_at(c->header_hits, x, y);
	if (hit != NULL && hit->id == HIT_CLOSE) {
		comments_show(c, false);
		gtk_widget_grab_focus(GTK_WIDGET(c->view));
	}
}

/* ---- Cards ----------------------------------------------------------------- */

static uint32_t id_of(GtkWidget *w) {
	return GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "gw-comment"));
}

static void paint_card_head(cairo_t *cr, int w, int h, void *data) {
	struct card *k = data;
	g_array_set_size(k->hits, 0);
	int x = w;
	x -= gem_button_width("Delete");
	gem_button(cr, k->hits, x, 0, 0, "Delete", HIT_DELETE, false);
	if (!k->reply) {
		x -= gem_button_width("Reply") + GEM_PAD / 2;
		gem_button(cr, k->hits, x, 0, 0, "Reply", HIT_REPLY, false);
	}
	gem_black(cr);
	gem_text_clipped(cr, k->author, 0, 0, x - GEM_PAD, GEM_ROW_H);
	gem_text_clipped(cr, k->when, 0, GEM_ROW_H, w, GEM_ROW_H);
	gem_grey_out(cr, 0, GEM_ROW_H, w, GEM_ROW_H);
}

static void card_free(gpointer data) {
	struct card *k = data;
	g_free(k->author);
	g_free(k->when);
	g_array_unref(k->hits);
	g_free(k);
}

static void open_reply(struct comments *c, GtkWidget *thread);

static void card_head_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct card *k = data;
	const struct gem_hit *hit = gem_hit_at(k->hits, x, y);
	if (hit == NULL) {
		return;
	}
	GtkWidget *head = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	if (hit->id == HIT_DELETE) {
		k->c->run("comment-delete", g_variant_new_uint32(k->id), k->c->data);
		gtk_widget_grab_focus(GTK_WIDGET(k->c->view));
	} else if (hit->id == HIT_REPLY) {
		GtkWidget *thread = gtk_widget_get_ancestor(head, GTK_TYPE_BOX);
		while (thread != NULL && g_object_get_data(G_OBJECT(thread), "gw-thread") == NULL) {
			thread = gtk_widget_get_parent(thread);
		}
		if (thread != NULL) {
			open_reply(k->c, thread);
		}
	}
}

/* An edited card's words are kept when it loses the focus, as one step. */
static void card_text_left(GtkEventControllerFocus *f, gpointer data) {
	struct comments *c = data;
	GtkTextView *tv = GTK_TEXT_VIEW(gtk_event_controller_get_widget(
		GTK_EVENT_CONTROLLER(f)));
	uint32_t id = id_of(GTK_WIDGET(tv));
	char *text = text_view_text(tv);
	const WpComment *cm = wp_document_comment(c->doc, id);
	if (cm != NULL && strcmp(cm->text, text) != 0) {
		c->run("comment-edit", g_variant_new("(us)", id, text), c->data);
	}
	g_free(text);
	comments_refresh(c);
}

/* Clicking a card's words puts a text view in their place, to edit. */
static void card_label_released(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct comments *c = data;
	GtkWidget *label = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	GtkWidget *card = gtk_widget_get_parent(label);
	uint32_t id = id_of(card);
	const WpComment *cm = wp_document_comment(c->doc, id);
	if (cm == NULL) {
		return;
	}
	GtkWidget *tv = text_view_new();
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
	gtk_text_buffer_set_text(buf, cm->text, -1);
	g_object_set_data(G_OBJECT(tv), "gw-comment", GUINT_TO_POINTER(id));
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "leave", G_CALLBACK(card_text_left), c);
	gtk_widget_add_controller(tv, focus);
	gtk_box_insert_child_after(GTK_BOX(card), tv, label);
	gtk_box_remove(GTK_BOX(card), label);
	gtk_widget_grab_focus(tv);
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(buf, &end);
	gtk_text_buffer_place_cursor(buf, &end);
}

/* Clicking a card selects its text in the document. */
static void card_pressed(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	struct comments *c = data;
	GtkWidget *card = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	wp_editor_select_comment(c->ed, id_of(card));
}

static GtkWidget *card_new(struct comments *c, uint32_t id, bool reply,
		const char *author, const char *date, const char *text) {
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_add_css_class(card, "gw-card");
	if (reply) {
		gtk_widget_set_margin_start(card, INDENT);
	}
	g_object_set_data(G_OBJECT(card), "gw-comment", GUINT_TO_POINTER(id));

	struct card *k = g_new0(struct card, 1);
	k->c = c;
	k->id = id;
	k->reply = reply;
	k->author = g_strdup(author[0] != '\0' ? author : "Unknown");
	k->when = pretty_date(date);
	k->hits = gem_hits_new();
	GtkWidget *head = gem_pixel_area_new(0, CARD_HEAD_H, paint_card_head, k);
	g_object_set_data_full(G_OBJECT(head), "gw-card", k, card_free);
	GtkGesture *press = gtk_gesture_click_new();
	g_signal_connect(press, "released", G_CALLBACK(card_head_pressed), k);
	gtk_widget_add_controller(head, GTK_EVENT_CONTROLLER(press));
	gtk_box_append(GTK_BOX(card), head);

	GtkWidget *body = gtk_label_new(text[0] != '\0' ? text : "(empty)");
	gtk_widget_add_css_class(body, "gem-text");
	gtk_label_set_wrap(GTK_LABEL(body), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(body), 0);
	gtk_widget_set_margin_top(body, 2);
	gtk_widget_set_margin_bottom(body, 2);
	gtk_widget_set_tooltip_text(body, "Click to edit");
	gtk_widget_set_cursor_from_name(body, "text");
	GtkGesture *edit = gtk_gesture_click_new();
	g_signal_connect(edit, "released", G_CALLBACK(card_label_released), c);
	gtk_widget_add_controller(body, GTK_EVENT_CONTROLLER(edit));
	gtk_box_append(GTK_BOX(card), body);

	GtkGesture *click = gtk_gesture_click_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
		GTK_PHASE_CAPTURE);
	g_signal_connect(click, "pressed", G_CALLBACK(card_pressed), c);
	gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click));
	return card;
}

/* ---- Replies --------------------------------------------------------------- */

struct reply {
	struct comments *c;
	GtkWidget *editor, *text;
	uint32_t root;
};

static void reply_close(struct reply *r) {
	GtkWidget *view = GTK_WIDGET(r->c->view);
	gtk_box_remove(GTK_BOX(gtk_widget_get_parent(r->editor)), r->editor);
	gtk_widget_grab_focus(view);
}

static void reply_post(struct reply *r) {
	char *text = text_view_text(GTK_TEXT_VIEW(r->text));
	g_strstrip(text);
	struct comments *c = r->c;
	uint32_t root = r->root;
	reply_close(r);
	if (text[0] != '\0') {
		c->run("comment-reply", g_variant_new("(us)", root, text), c->data);
	}
	g_free(text);
}

static void reply_button(int which, void *data) {
	if (which == 1) {
		reply_post(data);
	} else {
		reply_close(data);
	}
}

/* Escape drops it, Ctrl+Return posts it. */
static gboolean reply_key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	if (keyval == GDK_KEY_Escape) {
		reply_close(data);
		return TRUE;
	}
	if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
			(state & GDK_CONTROL_MASK)) {
		reply_post(data);
		return TRUE;
	}
	return FALSE;
}

static void open_reply(struct comments *c, GtkWidget *thread) {
	for (GtkWidget *w = gtk_widget_get_first_child(thread); w != NULL;
			w = gtk_widget_get_next_sibling(w)) {
		struct reply *open = g_object_get_data(G_OBJECT(w), "gw-reply");
		if (open != NULL) {
			gtk_widget_grab_focus(open->text);
			return;
		}
	}
	struct reply *r = g_new0(struct reply, 1);
	r->c = c;
	r->root = id_of(thread);
	r->editor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(r->editor, "gw-card");
	gtk_widget_set_margin_start(r->editor, INDENT);
	g_object_set_data_full(G_OBJECT(r->editor), "gw-reply", r, g_free);
	r->text = text_view_new();
	gtk_widget_set_size_request(r->text, -1, 48);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(reply_key), r);
	gtk_widget_add_controller(r->text, keys);
	gtk_box_append(GTK_BOX(r->editor), r->text);
	gtk_box_append(GTK_BOX(r->editor), button_row("Reply", reply_button, r));
	gtk_box_append(GTK_BOX(thread), r->editor);
	gtk_widget_grab_focus(r->text);
}

/* ---- The draft ------------------------------------------------------------- */

static void update_empty(struct comments *c) {
	gtk_widget_set_visible(c->empty, c->count == 0 && !c->drafting);
}

static void draft_end(struct comments *c) {
	c->drafting = false;
	gtk_widget_set_visible(c->draft, FALSE);
	update_empty(c);
	gtk_widget_grab_focus(GTK_WIDGET(c->view));
}

static void draft_post(struct comments *c) {
	char *text = text_view_text(GTK_TEXT_VIEW(c->draft_text));
	g_strstrip(text);
	if (text[0] != '\0') {
		/* The text it was started for, whatever's selected now. */
		wp_editor_select(c->ed, c->draft_a, c->draft_b);
		c->run("comment-add", g_variant_new_string(text), c->data);
	}
	g_free(text);
	draft_end(c);
}

static void draft_button(int which, void *data) {
	if (which == 1) {
		draft_post(data);
	} else {
		draft_end(data);
	}
}

static gboolean draft_key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	if (keyval == GDK_KEY_Escape) {
		draft_end(data);
		return TRUE;
	}
	if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
			(state & GDK_CONTROL_MASK)) {
		draft_post(data);
		return TRUE;
	}
	return FALSE;
}

static void paint_draft_head(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_text(cr, "New comment", 0, 0, GEM_ROW_H);
}

void comments_add(struct comments *c) {
	if (!wp_editor_has_selection(c->ed)) {
		return;
	}
	wp_editor_get_selection(c->ed, &c->draft_a, &c->draft_b);
	c->drafting = true;
	comments_show(c, true);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->draft_text)),
		"", -1);
	gtk_widget_set_visible(c->draft, TRUE);
	update_empty(c);
	gtk_widget_grab_focus(c->draft_text);
}

/* ---- Building -------------------------------------------------------------- */

/* The card whose text the caret's in has a thicker border. */
void comments_caret_moved(struct comments *c) {
	uint32_t id = wp_editor_comment_at_caret(c->ed);
	for (GtkWidget *t = gtk_widget_get_first_child(c->cards); t != NULL;
			t = gtk_widget_get_next_sibling(t)) {
		GtkWidget *card = gtk_widget_get_first_child(t);
		if (card == NULL) {
			continue;
		}
		if (id != 0 && id_of(t) == id) {
			gtk_widget_add_css_class(card, "gw-active");
		} else {
			gtk_widget_remove_css_class(card, "gw-active");
		}
	}
}

/* The cards, from the comments query (the document's order, no orphans). */
static void rebuild(struct comments *c) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(c->cards)) != NULL) {
		gtk_box_remove(GTK_BOX(c->cards), child);
	}
	size_t n = 0;
	GVariant *list = NULL;
	if (wp_command_run(c->ed, "comments", NULL, &list, NULL) && list != NULL) {
		g_variant_ref_sink(list);
		GVariantIter it;
		GVariant *item;
		GtkWidget *thread = NULL; /* a top-level comment and its replies */
		g_variant_iter_init(&it, list);
		while ((item = g_variant_iter_next_value(&it)) != NULL) {
			guint32 id = 0, parent = 0;
			const char *author = "", *date = "", *text = "";
			g_variant_lookup(item, "id", "u", &id);
			g_variant_lookup(item, "parent", "u", &parent);
			g_variant_lookup(item, "author", "&s", &author);
			g_variant_lookup(item, "date", "&s", &date);
			g_variant_lookup(item, "text", "&s", &text);
			if (parent == 0 || thread == NULL) {
				thread = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
				g_object_set_data(G_OBJECT(thread), "gw-thread", GINT_TO_POINTER(1));
				g_object_set_data(G_OBJECT(thread), "gw-comment", GUINT_TO_POINTER(id));
				gtk_box_append(GTK_BOX(c->cards), thread);
				n++;
			}
			gtk_box_append(GTK_BOX(thread),
				card_new(c, id, parent != 0, author, date, text));
			g_variant_unref(item);
		}
		g_variant_unref(list);
	}
	/* A document that turns out to have comments shows them. */
	if (n > 0 && c->count == 0) {
		comments_show(c, true);
	}
	c->count = n;
	update_empty(c);
	comments_caret_moved(c);
}

/* Later, so a card whose focus-out just saved an edit isn't taken down in
 * the middle of its own handler. */
static gboolean rebuild_idle(gpointer data) {
	struct comments *c = data;
	c->refresh_id = 0;
	rebuild(c);
	return G_SOURCE_REMOVE;
}

void comments_refresh(struct comments *c) {
	if (c->refresh_id == 0) {
		c->refresh_id = g_idle_add(rebuild_idle, c);
	}
}

/* ---- Showing and sizing ---------------------------------------------------- */

bool comments_shown(struct comments *c) {
	return c->shown;
}

void comments_show(struct comments *c, bool shown) {
	if (shown == c->shown) {
		return;
	}
	c->shown = shown;
	gtk_widget_set_visible(c->panel, shown);
	if (c->shown_changed != NULL) {
		c->shown_changed(c->data);
	}
}

static void set_width(struct comments *c, int width) {
	GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(c->panel));
	int max = window != NULL ? gtk_widget_get_width(window) - MIN_PAGE : width;
	c->width = CLAMP(width, MIN_W, MAX(max, MIN_W));
	gtk_widget_set_size_request(c->body, c->width, -1);
}

static void paint_handle(cairo_t *cr, int w, int h, void *data) {
	gem_black(cr);
	gem_fill(cr, 0, 0, 1, h);
}

/* The handle moves as the panel grows, so the pointer's followed in the
 * window's coordinates. */
static double window_x(struct comments *c, GtkWidget *w, double x) {
	graphene_point_t p = GRAPHENE_POINT_INIT((float)x, 0), out;
	GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(w));
	return gtk_widget_compute_point(w, root, &p, &out) ? out.x : x;
}

static void handle_begin(GtkGestureDrag *g, double x, double y, gpointer data) {
	struct comments *c = data;
	GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	c->drag_width = c->width;
	c->drag_x = window_x(c, w, x);
}

static void handle_update(GtkGestureDrag *g, double dx, double dy,
		gpointer data) {
	struct comments *c = data;
	GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	double sx, sy;
	gtk_gesture_drag_get_start_point(g, &sx, &sy);
	set_width(c, c->drag_width + (int)(c->drag_x - window_x(c, w, sx + dx)));
}

static void handle_end(GtkGestureDrag *g, double dx, double dy, gpointer data) {
	struct comments *c = data;
	wp_settings_set_int("comments-width", c->width);
}

static void panel_destroyed(GtkWidget *w, gpointer data) {
	struct comments *c = data;
	g_clear_handle_id(&c->refresh_id, g_source_remove);
	g_array_unref(c->header_hits);
	g_free(c);
}

struct comments *comments_new(WpPageView *view, comments_run_fn run,
		void (*shown_changed)(void *data), void *data) {
	struct comments *c = g_new0(struct comments, 1);
	c->view = view;
	c->ed = wp_page_view_get_editor(view);
	c->doc = wp_editor_document(c->ed);
	c->run = run;
	c->shown_changed = shown_changed;
	c->data = data;
	c->header_hits = gem_hits_new();

	c->panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *handle = gem_pixel_area_new(HANDLE_W, 0, paint_handle, NULL);
	gtk_widget_set_cursor_from_name(handle, "col-resize");
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(handle_begin), c);
	g_signal_connect(drag, "drag-update", G_CALLBACK(handle_update), c);
	g_signal_connect(drag, "drag-end", G_CALLBACK(handle_end), c);
	gtk_widget_add_controller(handle, GTK_EVENT_CONTROLLER(drag));
	gtk_box_append(GTK_BOX(c->panel), handle);

	c->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(c->body, "gw-comments");
	c->width = wp_settings_get_int("comments-width", DEFAULT_W);
	if (c->width < MIN_W) {
		c->width = DEFAULT_W;
	}
	gtk_widget_set_size_request(c->body, c->width, -1);
	gtk_box_append(GTK_BOX(c->panel), c->body);

	c->header = gem_pixel_area_new(0, HEADER_H, paint_header, c);
	GtkGesture *press = gtk_gesture_click_new();
	g_signal_connect(press, "released", G_CALLBACK(header_pressed), c);
	gtk_widget_add_controller(c->header, GTK_EVENT_CONTROLLER(press));
	gtk_box_append(GTK_BOX(c->body), c->header);

	c->draft = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(c->draft, "gw-card");
	gtk_widget_set_margin_start(c->draft, GEM_PAD);
	gtk_widget_set_margin_end(c->draft, GEM_PAD);
	gtk_widget_set_margin_top(c->draft, GEM_PAD);
	gtk_box_append(GTK_BOX(c->draft),
		gem_pixel_area_new(0, GEM_ROW_H, paint_draft_head, NULL));
	c->draft_text = text_view_new();
	gtk_widget_set_size_request(c->draft_text, -1, 60);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(draft_key), c);
	gtk_widget_add_controller(c->draft_text, keys);
	gtk_box_append(GTK_BOX(c->draft), c->draft_text);
	gtk_box_append(GTK_BOX(c->draft), button_row("Comment", draft_button, c));
	gtk_widget_set_visible(c->draft, FALSE);
	gtk_box_append(GTK_BOX(c->body), c->draft);

	c->empty = gtk_label_new("Select some text, then\nEdit > Add Comment.");
	gtk_widget_add_css_class(c->empty, "gem-text");
	gtk_label_set_justify(GTK_LABEL(c->empty), GTK_JUSTIFY_CENTER);
	gtk_widget_set_margin_top(c->empty, 24);
	gtk_box_append(GTK_BOX(c->body), c->empty);

	c->cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, GEM_PAD);
	gtk_widget_set_margin_start(c->cards, GEM_PAD);
	gtk_widget_set_margin_end(c->cards, GEM_PAD);
	gtk_widget_set_margin_top(c->cards, GEM_PAD);
	gtk_widget_set_margin_bottom(c->cards, GEM_PAD);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_vexpand(row, TRUE);
	c->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(c->scroller),
		GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(c->scroller), c->cards);
	gtk_widget_set_hexpand(c->scroller, TRUE);
	gtk_box_append(GTK_BOX(row), c->scroller);
	gtk_box_append(GTK_BOX(row), gem_scrollbar_new(GTK_ORIENTATION_VERTICAL,
		gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(c->scroller))));
	gtk_box_append(GTK_BOX(c->body), row);

	/* Set, or the wrapping labels inside would have it take half the
	 * window. */
	gtk_widget_set_hexpand(c->panel, FALSE);
	gtk_widget_set_visible(c->panel, FALSE);
	g_signal_connect(c->panel, "destroy", G_CALLBACK(panel_destroyed), c);
	rebuild(c);
	return c;
}

GtkWidget *comments_widget(struct comments *c) {
	return c->panel;
}
