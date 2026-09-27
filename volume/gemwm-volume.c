/*
 * gemwm-volume: sound for GemWM, GEM style.
 *
 *   gemwm-volume                 the window: outputs, inputs and the
 *                                applications playing, each with a slider
 *                                and Mute; the round buttons pick the
 *                                default output and input
 *   gemwm-volume --menu-app      the menu bar item (see menu/menu.c): a
 *                                speaker and the output volume. A click
 *                                opens the window, a right-click mutes, and
 *                                scrolling over it turns it up and down
 *   gemwm-volume up|down|mute|mic-mute   for the volume keys
 *
 * It talks to PulseAudio, or PipeWire's PulseAudio server, and hears about
 * every change, so the bar follows the keys and other programs at once.
 */
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gtk/gtk.h>
#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app-menu.h"
#include "gem-draw.h"

#define STEP 5         /* percent, for the keys and the scroll wheel */
#define MAX_PERCENT 100
#define PAD 8
#define ROW_H 24
#define BUTTON_H 18
#define SLIDER_W 120
#define SLIDER_BOX 14  /* the slider's box */
#define TRACK_H 12

/* ---- PulseAudio --------------------------------------------------------- */

enum kind { KIND_OUTPUT, KIND_INPUT, KIND_STREAM };

/* An output, an input or an application's stream. */
struct channel {
	enum kind kind;
	uint32_t index;
	char *name;  /* PulseAudio's, for devices */
	char *label; /* what we show */
	pa_cvolume volume;
	bool mute;
};

static struct {
	pa_glib_mainloop *mainloop;
	pa_context *context;
	bool ready;
	GPtrArray *outputs, *inputs, *streams; /* struct channel */
	char *default_output, *default_input;
	/* A refresh in progress: its results, and how many answers are due. */
	GPtrArray *new_outputs, *new_inputs, *new_streams;
	char *new_default_output, *new_default_input;
	int pending;
	bool again;           /* something changed during the refresh */
	void (*changed)(void); /* after each refresh */
} pa;

static void channel_free(void *data) {
	struct channel *c = data;
	g_free(c->name);
	g_free(c->label);
	g_free(c);
}

static struct channel *channel_new(enum kind kind, uint32_t index,
		const char *name, const char *label, const pa_cvolume *volume,
		int mute) {
	struct channel *c = g_new0(struct channel, 1);
	c->kind = kind;
	c->index = index;
	c->name = g_strdup(name);
	c->label = g_strdup(label != NULL && label[0] != '\0' ? label : name);
	for (char *p = c->label; *p != '\0'; p++) {
		if (*p == '\t' || *p == '\n') {
			*p = ' ';
		}
	}
	c->volume = *volume;
	c->mute = mute;
	return c;
}

static int percent(const struct channel *c) {
	return (int)((pa_cvolume_max(&c->volume) * 100.0 + PA_VOLUME_NORM / 2) /
		PA_VOLUME_NORM);
}

static GPtrArray *channels(enum kind kind) {
	return kind == KIND_OUTPUT ? pa.outputs :
		kind == KIND_INPUT ? pa.inputs : pa.streams;
}

static struct channel *find(enum kind kind, uint32_t index) {
	GPtrArray *list = channels(kind);
	for (guint i = 0; list != NULL && i < list->len; i++) {
		struct channel *c = g_ptr_array_index(list, i);
		if (c->index == index) {
			return c;
		}
	}
	return NULL;
}

static struct channel *default_channel(enum kind kind) {
	GPtrArray *list = channels(kind);
	const char *name = kind == KIND_OUTPUT ? pa.default_output :
		pa.default_input;
	for (guint i = 0; list != NULL && i < list->len; i++) {
		struct channel *c = g_ptr_array_index(list, i);
		if (g_strcmp0(c->name, name) == 0) {
			return c;
		}
	}
	return NULL;
}

static void refresh(void);

static void answered(void) {
	if (--pa.pending > 0) {
		return;
	}
	g_clear_pointer(&pa.outputs, g_ptr_array_unref);
	g_clear_pointer(&pa.inputs, g_ptr_array_unref);
	g_clear_pointer(&pa.streams, g_ptr_array_unref);
	pa.outputs = g_steal_pointer(&pa.new_outputs);
	pa.inputs = g_steal_pointer(&pa.new_inputs);
	pa.streams = g_steal_pointer(&pa.new_streams);
	g_free(pa.default_output);
	g_free(pa.default_input);
	pa.default_output = g_steal_pointer(&pa.new_default_output);
	pa.default_input = g_steal_pointer(&pa.new_default_input);
	pa.changed();
	if (pa.again) {
		pa.again = false;
		refresh();
	}
}

static void on_server(pa_context *ctx, const pa_server_info *info, void *data) {
	pa.new_default_output = g_strdup(info->default_sink_name);
	pa.new_default_input = g_strdup(info->default_source_name);
	answered();
}

static void on_output(pa_context *ctx, const pa_sink_info *info, int eol,
		void *data) {
	if (eol) {
		answered();
		return;
	}
	g_ptr_array_add(pa.new_outputs, channel_new(KIND_OUTPUT, info->index,
		info->name, info->description, &info->volume, info->mute));
}

static void on_input(pa_context *ctx, const pa_source_info *info, int eol,
		void *data) {
	if (eol) {
		answered();
		return;
	}
	/* Monitors (what an output is playing) aren't microphones. */
	if (info->monitor_of_sink == PA_INVALID_INDEX) {
		g_ptr_array_add(pa.new_inputs, channel_new(KIND_INPUT, info->index,
			info->name, info->description, &info->volume, info->mute));
	}
}

static void on_stream(pa_context *ctx, const pa_sink_input_info *info, int eol,
		void *data) {
	if (eol) {
		answered();
		return;
	}
	const char *app = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
	g_ptr_array_add(pa.new_streams, channel_new(KIND_STREAM, info->index,
		info->name, app, &info->volume, info->mute));
}

/* Asks for everything again; the answers arrive in pieces. */
static void refresh(void) {
	if (!pa.ready) {
		return;
	}
	if (pa.pending > 0) {
		pa.again = true;
		return;
	}
	pa.new_outputs = g_ptr_array_new_with_free_func(channel_free);
	pa.new_inputs = g_ptr_array_new_with_free_func(channel_free);
	pa.new_streams = g_ptr_array_new_with_free_func(channel_free);
	pa.pending = 4;
	pa_operation_unref(pa_context_get_server_info(pa.context, on_server, NULL));
	pa_operation_unref(pa_context_get_sink_info_list(pa.context, on_output, NULL));
	pa_operation_unref(pa_context_get_source_info_list(pa.context, on_input,
		NULL));
	pa_operation_unref(pa_context_get_sink_input_info_list(pa.context,
		on_stream, NULL));
}

static void on_event(pa_context *ctx, pa_subscription_event_type_t type,
		uint32_t index, void *data) {
	refresh();
}

static void on_state(pa_context *ctx, void *data) {
	switch (pa_context_get_state(ctx)) {
	case PA_CONTEXT_READY:
		pa.ready = true;
		pa_context_set_subscribe_callback(ctx, on_event, NULL);
		pa_operation_unref(pa_context_subscribe(ctx, PA_SUBSCRIPTION_MASK_SINK |
			PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK_INPUT |
			PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
		refresh();
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		/* The sound server went away: nothing to show until it's back. */
		pa.ready = false;
		g_clear_pointer(&pa.outputs, g_ptr_array_unref);
		g_clear_pointer(&pa.inputs, g_ptr_array_unref);
		g_clear_pointer(&pa.streams, g_ptr_array_unref);
		pa.changed();
		break;
	default:
		break;
	}
}

/* Connects on GLib's main loop; with NOFAIL it waits for (and comes back
 * to) the server rather than giving up. */
static void pa_start(void (*changed)(void)) {
	pa.changed = changed;
	pa.mainloop = pa_glib_mainloop_new(NULL);
	pa.context = pa_context_new(pa_glib_mainloop_get_api(pa.mainloop),
		"GemWM Volume");
	pa_context_set_state_callback(pa.context, on_state, NULL);
	pa_context_connect(pa.context, NULL, PA_CONTEXT_NOFAIL, NULL);
}

static void set_volume(struct channel *c, int pct, pa_context_success_cb_t done,
		void *data) {
	pct = pct < 0 ? 0 : pct;
	pa_volume_t v = (pa_volume_t)((uint64_t)PA_VOLUME_NORM * pct / 100);
	/* Scaled, so the balance between the channels stays as it was. */
	if (pa_cvolume_max(&c->volume) == PA_VOLUME_MUTED) {
		pa_cvolume_set(&c->volume, c->volume.channels, v);
	} else {
		pa_cvolume_scale(&c->volume, v);
	}
	pa_operation *op =
		c->kind == KIND_OUTPUT ? pa_context_set_sink_volume_by_index(
			pa.context, c->index, &c->volume, done, data) :
		c->kind == KIND_INPUT ? pa_context_set_source_volume_by_index(
			pa.context, c->index, &c->volume, done, data) :
		pa_context_set_sink_input_volume(pa.context, c->index, &c->volume,
			done, data);
	if (op != NULL) {
		pa_operation_unref(op);
	}
}

static void set_mute(struct channel *c, bool mute, pa_context_success_cb_t done,
		void *data) {
	c->mute = mute;
	pa_operation *op =
		c->kind == KIND_OUTPUT ? pa_context_set_sink_mute_by_index(
			pa.context, c->index, mute, done, data) :
		c->kind == KIND_INPUT ? pa_context_set_source_mute_by_index(
			pa.context, c->index, mute, done, data) :
		pa_context_set_sink_input_mute(pa.context, c->index, mute, done, data);
	if (op != NULL) {
		pa_operation_unref(op);
	}
}

/* Up or down a step; turning it up unmutes. Past 100% (set elsewhere),
 * up leaves it alone. */
static void step(struct channel *c, int direction, pa_context_success_cb_t done,
		void *data) {
	int now = percent(c);
	int want = direction > 0 ? (now >= MAX_PERCENT ? now :
		MIN(MAX_PERCENT, (now / STEP + 1) * STEP)) :
		MAX(0, ((now + STEP - 1) / STEP - 1) * STEP);
	if (direction > 0 && c->mute) {
		set_mute(c, false, NULL, NULL);
	}
	set_volume(c, want, done, data);
}

/* ---- The volume keys ---------------------------------------------------- */

static struct {
	const char *command;
	GMainLoop *loop;
} key;

static void key_done(pa_context *ctx, int success, void *data) {
	g_main_loop_quit(key.loop);
}

static void key_changed(void) {
	if (key.command == NULL) {
		return;
	}
	const char *command = key.command;
	key.command = NULL; /* once */
	bool mic = strcmp(command, "mic-mute") == 0;
	struct channel *c = default_channel(mic ? KIND_INPUT : KIND_OUTPUT);
	if (c == NULL) {
		g_main_loop_quit(key.loop);
	} else if (strcmp(command, "up") == 0 || strcmp(command, "down") == 0) {
		step(c, strcmp(command, "up") == 0 ? 1 : -1, key_done, NULL);
	} else {
		set_mute(c, !c->mute, key_done, NULL);
	}
}

static gboolean key_timeout(void *data) {
	fprintf(stderr, "gemwm-volume: no sound server\n");
	g_main_loop_quit(key.loop);
	return G_SOURCE_REMOVE;
}

static int keys(const char *command) {
	key.command = command;
	key.loop = g_main_loop_new(NULL, FALSE);
	pa_start(key_changed);
	g_timeout_add_seconds(3, key_timeout, NULL);
	g_main_loop_run(key.loop);
	return 0;
}

/* ---- The menu bar item -------------------------------------------------- */

static char *self; /* this program, to start the window */

/* The speaker, with 0-3 sound waves for the level, or an X when muted:
 * points on a 15x11 grid. */
enum { ICON_W = 15, ICON_H = 11 };
static const signed char speaker[][2] = {
	{4,0}, {3,1},{4,1}, {2,2},{4,2}, {0,3},{1,3},{2,3},{4,3}, {0,4},{4,4},
	{0,5},{4,5}, {0,6},{4,6}, {0,7},{1,7},{2,7},{4,7}, {2,8},{4,8},
	{3,9},{4,9}, {4,10}, {-1,-1},
};
static const signed char waves[3][10][2] = {
	{ {6,3},{7,4},{7,5},{7,6},{6,7}, {-1,-1} },
	{ {8,2},{9,3},{10,4},{10,5},{10,6},{9,7},{8,8}, {-1,-1} },
	{ {11,1},{12,2},{13,3},{13,4},{13,5},{13,6},{13,7},{12,8},{11,9}, {-1,-1} },
};
static const signed char cross[][2] = {
	{7,3},{11,3}, {8,4},{10,4}, {9,5}, {8,6},{10,6}, {7,7},{11,7}, {-1,-1},
};

static void plot(bool px[ICON_H][ICON_W], const signed char (*points)[2]) {
	for (; points[0][0] >= 0; points++) {
		px[points[0][1]][points[0][0]] = true;
	}
}

static void status_print(void) {
	static char *last;
	struct channel *c = default_channel(KIND_OUTPUT);
	GString *line = g_string_new(NULL);
	if (c != NULL) {
		bool px[ICON_H][ICON_W] = { { false } };
		int level = percent(c);
		plot(px, speaker);
		if (c->mute) {
			plot(px, cross);
		} else {
			for (int w = 0; w < 3 && level > w * 34; w++) {
				plot(px, waves[w]);
			}
		}
		g_string_append_printf(line, "bitmap:%dx%d:", ICON_W, ICON_H);
		for (int y = 0; y < ICON_H; y++) {
			for (int x = 0; x < ICON_W; x += 8) {
				unsigned byte = 0;
				for (int b = 0; b < 8 && x + b < ICON_W; b++) {
					byte |= px[y][x + b] ? 0x80u >> b : 0;
				}
				g_string_append_printf(line, "%02x", byte);
			}
		}
		if (c->mute) {
			g_string_append(line, "\tMuted");
		} else {
			g_string_append_printf(line, "\t%d%%", level);
		}
		/* The tooltip: which output this is. */
		g_string_append_printf(line, "\t\t%s%s", c->label,
			c->mute ? " (muted)" : "");
	}
	if (g_strcmp0(line->str, last) != 0) {
		printf("%s\n", line->str);
		fflush(stdout);
		g_free(last);
		last = g_strdup(line->str);
	}
	g_string_free(line, TRUE);
}

static void read_bar(GObject *source, GAsyncResult *result, void *data) {
	GMainLoop *loop = data;
	char *line = g_data_input_stream_read_line_finish(
		G_DATA_INPUT_STREAM(source), result, NULL, NULL);
	if (line == NULL) {
		g_main_loop_quit(loop); /* the bar has gone */
		return;
	}
	struct channel *c = default_channel(KIND_OUTPUT);
	if (strcmp(line, "click 1") == 0) {
		/* Single instance: a second click brings the window forward. */
		char *argv[] = { self, NULL };
		g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL,
			NULL);
	} else if (strcmp(line, "click 3") == 0 && c != NULL) {
		set_mute(c, !c->mute, NULL, NULL);
		status_print();
	} else if (g_str_has_prefix(line, "scroll ") && c != NULL) {
		step(c, strcmp(line, "scroll up") == 0 ? 1 : -1, NULL, NULL);
		status_print();
	}
	g_free(line);
	g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(source),
		G_PRIORITY_DEFAULT, NULL, read_bar, loop);
}

static int menu_app(void) {
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	pa_start(status_print);
	status_print();
	GInputStream *in = g_unix_input_stream_new(0, FALSE);
	GDataInputStream *lines = g_data_input_stream_new(in);
	g_data_input_stream_read_line_async(lines, G_PRIORITY_DEFAULT, NULL,
		read_bar, loop);
	g_main_loop_run(loop);
	return 0;
}

/* ---- The window --------------------------------------------------------- */

enum action { ACT_NONE, ACT_DEFAULT, ACT_MUTE, ACT_SLIDER, ACT_CLOSE,
	ACT_MUTE_OUTPUT };

/* Something clickable, as last drawn. */
struct hit {
	int x, y, w, h;
	enum action action;
	enum kind kind;
	uint32_t index;
};

static struct {
	GtkWidget *window, *area;
	struct app_menu *menu;
	GArray *hits;
	struct hit pressed; /* action ACT_NONE when nothing is */
	int height;         /* what the rows need */
} ui;

static void add_hit(int x, int y, int w, int h, enum action action,
		const struct channel *c) {
	struct hit hit = { x, y, w, h, action, c->kind, c->index };
	g_array_append_val(ui.hits, hit);
}

static bool is_pressed(enum action action, const struct channel *c) {
	return ui.pressed.action == action && ui.pressed.kind == c->kind &&
		ui.pressed.index == c->index;
}

/* GEM's round button, set or not. */
static void radio(cairo_t *cr, int x, int y, bool on) {
	static const char *const ring[] = {
		"...####...",
		".##....##.",
		".#......#.",
		"#........#",
		"#........#",
		"#........#",
		"#........#",
		".#......#.",
		".##....##.",
		"...####...",
	};
	static const char *const dot[] = {
		"....", ".##.", "####", "####", ".##.", "....",
	};
	gem_black(cr);
	gem_bitmap(cr, ring, 10, x, y);
	if (on) {
		gem_bitmap(cr, dot, 6, x + 3, y + 2);
	}
}

/* A GEM slider: a dotted track with a box on it. */
static void slider(cairo_t *cr, int x, int y, const struct channel *c) {
	int pct = MIN(percent(c), MAX_PERCENT);
	gem_black(cr);
	gem_frame(cr, x, y, SLIDER_W, TRACK_H, 1);
	for (int ty = y + 1; ty < y + TRACK_H - 1; ty++) {
		for (int tx = x + 1 + (ty & 1); tx < x + SLIDER_W - 1; tx += 2) {
			gem_fill(cr, tx, ty, 1, 1);
		}
	}
	int bx = x + (SLIDER_W - SLIDER_BOX) * pct / 100;
	gem_white(cr);
	gem_fill(cr, bx, y, SLIDER_BOX, TRACK_H);
	gem_black(cr);
	gem_frame(cr, bx, y, SLIDER_BOX, TRACK_H, 1);
	if (is_pressed(ACT_SLIDER, c)) {
		gem_fill(cr, bx + 2, y + 2, SLIDER_BOX - 4, TRACK_H - 4);
	}
	add_hit(x, y - 3, SLIDER_W, TRACK_H + 6, ACT_SLIDER, c);
}

static int row(cairo_t *cr, const struct channel *c, bool is_default, int y,
		int w) {
	int mute_w = (int)gem_text_width(cr, "Mute") + 2 * PAD;
	int pct_w = (int)gem_text_width(cr, "100%");
	int mute_x = w - PAD - mute_w;
	int pct_x = mute_x - PAD - pct_w;
	int slider_x = pct_x - PAD - SLIDER_W;
	int name_x = PAD;

	if (c->kind != KIND_STREAM) {
		radio(cr, PAD, y + (ROW_H - 10) / 2, is_default);
		if (!is_default) {
			add_hit(PAD - 2, y, 14, ROW_H, ACT_DEFAULT, c);
		}
		name_x = PAD + 16;
	}
	cairo_save(cr);
	cairo_rectangle(cr, name_x, y, slider_x - PAD - name_x, ROW_H);
	cairo_clip(cr);
	gem_black(cr);
	gem_text(cr, c->label, name_x, y, ROW_H);
	cairo_restore(cr);

	slider(cr, slider_x, y + (ROW_H - TRACK_H) / 2, c);
	char pct[16];
	snprintf(pct, sizeof(pct), "%d%%", percent(c));
	gem_black(cr);
	gem_text(cr, pct, pct_x + pct_w - gem_text_width(cr, pct), y, ROW_H);
	if (c->mute) {
		gem_grey_out(cr, slider_x, y, SLIDER_W + PAD + pct_w, ROW_H);
	}

	/* Mute is a GEM toggle: inverted while on. */
	int by = y + (ROW_H - BUTTON_H) / 2;
	gem_black(cr);
	if (c->mute != is_pressed(ACT_MUTE, c)) {
		gem_fill(cr, mute_x, by, mute_w, BUTTON_H);
		gem_white(cr);
	} else {
		gem_frame(cr, mute_x, by, mute_w, BUTTON_H, 1);
	}
	gem_text(cr, "Mute", mute_x + PAD, by, BUTTON_H);
	add_hit(mute_x, by, mute_w, BUTTON_H, ACT_MUTE, c);
	return y + ROW_H;
}

static int heading(cairo_t *cr, const char *label, int y, int w) {
	gem_black(cr);
	gem_text(cr, label, PAD, y, ROW_H);
	for (int x = PAD; x < w - PAD; x += 2) {
		gem_fill(cr, x, y + ROW_H - 3, 1, 1);
	}
	return y + ROW_H;
}

static int note(cairo_t *cr, const char *s, int y, int w) {
	gem_black(cr);
	gem_text(cr, s, PAD + 16, y, ROW_H);
	gem_grey_out(cr, 0, y, w, ROW_H);
	return y + ROW_H;
}

static int section(cairo_t *cr, const char *title, enum kind kind,
		const char *none, int y, int w) {
	y = heading(cr, title, y, w);
	GPtrArray *list = channels(kind);
	struct channel *def = kind != KIND_STREAM ? default_channel(kind) : NULL;
	if (list == NULL || list->len == 0) {
		return note(cr, none, y, w) + PAD;
	}
	for (guint i = 0; i < list->len; i++) {
		struct channel *c = g_ptr_array_index(list, i);
		y = row(cr, c, c == def, y, w);
	}
	return y + PAD;
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	g_array_set_size(ui.hits, 0);
	int y = PAD / 2;
	if (!pa.ready) {
		note(cr, "No sound server.", y, w);
		y += ROW_H;
	} else {
		y = section(cr, "Output", KIND_OUTPUT, "No outputs.", y, w);
		y = section(cr, "Input", KIND_INPUT, "No inputs.", y, w);
		y = section(cr, "Applications", KIND_STREAM, "Nothing playing.", y, w);
	}
	/* The window grows to fit, e.g. when an application starts playing. */
	if (y != ui.height) {
		ui.height = y;
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), y);
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

/* The slider's box follows the pointer. */
static void slide_to(double x) {
	struct channel *c = find(ui.pressed.kind, ui.pressed.index);
	if (c == NULL) {
		return;
	}
	double at = (x - ui.pressed.x - SLIDER_BOX / 2.0) / (SLIDER_W - SLIDER_BOX);
	int pct = (int)(CLAMP(at, 0, 1) * 100 + 0.5);
	if (pct != percent(c)) {
		set_volume(c, pct, NULL, NULL);
		gtk_widget_queue_draw(ui.area);
	}
}

static void act(enum action action, struct channel *c) {
	struct channel *output = default_channel(KIND_OUTPUT);
	switch (action) {
	case ACT_DEFAULT:
		pa_operation_unref(c->kind == KIND_OUTPUT ?
			pa_context_set_default_sink(pa.context, c->name, NULL, NULL) :
			pa_context_set_default_source(pa.context, c->name, NULL, NULL));
		break;
	case ACT_MUTE:
		set_mute(c, !c->mute, NULL, NULL);
		break;
	case ACT_MUTE_OUTPUT:
		if (output != NULL) {
			set_mute(output, !output->mute, NULL, NULL);
		}
		break;
	case ACT_CLOSE:
		gtk_window_close(GTK_WINDOW(ui.window));
		break;
	default:
		break;
	}
	gtk_widget_queue_draw(ui.area);
	app_menu_update(ui.menu);
}

static double drag_x, drag_y;

static void drag_begin(GtkGestureDrag *gesture, double x, double y, void *data) {
	struct hit *hit = hit_at(x, y);
	drag_x = x;
	drag_y = y;
	ui.pressed = hit != NULL ? *hit : (struct hit){ .action = ACT_NONE };
	if (ui.pressed.action == ACT_SLIDER) {
		slide_to(x);
	}
	gtk_widget_queue_draw(ui.area);
}

static void drag_update(GtkGestureDrag *gesture, double dx, double dy,
		void *data) {
	if (ui.pressed.action == ACT_SLIDER) {
		slide_to(drag_x + dx);
	}
}

/* As in GEM, a button acts when released over it. */
static void drag_end(GtkGestureDrag *gesture, double dx, double dy, void *data) {
	struct hit pressed = ui.pressed;
	ui.pressed.action = ACT_NONE;
	struct hit *hit = hit_at(drag_x + dx, drag_y + dy);
	if (pressed.action != ACT_SLIDER && hit != NULL &&
			hit->action == pressed.action && hit->kind == pressed.kind &&
			hit->index == pressed.index) {
		struct channel *c = find(pressed.kind, pressed.index);
		if (c != NULL) {
			act(pressed.action, c);
		}
	}
	gtk_widget_queue_draw(ui.area);
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint keyval,
		guint code, GdkModifierType mods, void *data) {
	if (keyval == GDK_KEY_Escape ||
			((mods & GDK_CONTROL_MASK) && (keyval == GDK_KEY_w ||
				keyval == GDK_KEY_W))) {
		act(ACT_CLOSE, NULL);
		return TRUE;
	}
	return FALSE;
}

/* The menus in GemWM's menu bar. Options is merged into GemWM's own. */
static void build_menus(struct app_menu *m, void *data) {
	struct channel *output = default_channel(KIND_OUTPUT);
	app_menu_add_menu(m, "File");
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);
	app_menu_add_menu(m, "Options");
	app_menu_add_item(m, ACT_MUTE_OUTPUT, "Mute Output", "",
		output == NULL ? APP_MENU_DISABLED :
		output->mute ? APP_MENU_CHECKED : 0);
}

static void menu_activate(uint32_t id, void *data) {
	act((enum action)id, NULL);
}

static void window_changed(void) {
	gtk_widget_queue_draw(ui.area);
	app_menu_update(ui.menu);
}

static void activate(GtkApplication *app, void *data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	ui.hits = g_array_new(FALSE, TRUE, sizeof(struct hit));
	ui.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ui.window), "Volume");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 520, -1);
	ui.area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ui.area), 200);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw, NULL, NULL);
	gtk_window_set_child(GTK_WINDOW(ui.window), ui.area);

	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(drag_begin), NULL);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_update), NULL);
	g_signal_connect(drag, "drag-end", G_CALLBACK(drag_end), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);

	pa_start(window_changed);
	gtk_window_present(GTK_WINDOW(ui.window));
}

int main(int argc, char *argv[]) {
	if (argc > 1 && strcmp(argv[1], "--menu-app") == 0) {
		self = strchr(argv[0], '/') != NULL ? g_strdup(argv[0]) :
			g_find_program_in_path(argv[0]);
		return menu_app();
	}
	if (argc > 1 && (strcmp(argv[1], "up") == 0 || strcmp(argv[1], "down") == 0 ||
			strcmp(argv[1], "mute") == 0 || strcmp(argv[1], "mic-mute") == 0)) {
		return keys(argv[1]);
	}
	if (argc > 1) {
		fprintf(stderr, "usage: gemwm-volume [--menu-app | up | down | mute | "
			"mic-mute]\n");
		return 2;
	}
	GtkApplication *app = gtk_application_new("org.gemwm.Volume",
		G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
