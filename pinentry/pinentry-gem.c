/*
 * pinentry-gem: GnuPG's passphrase prompt, as a GEM alert box.
 *
 * gpg-agent starts a pinentry when it needs a passphrase, and talks to it
 * on stdin and stdout (the Assuan protocol: SETDESC, SETPROMPT, GETPIN...).
 * The pinentries that come with GnuPG want X or a terminal; this one is a
 * Wayland window, so programs that run pass or gpg with no terminal
 * (GemMail's password command, Augur's key command) get asked rather than
 * fail. In ~/.gnupg/gpg-agent.conf:
 *
 *     pinentry-program /usr/local/bin/pinentry-gem
 *
 * With no Wayland display (over SSH, say), it becomes pinentry-curses.
 *
 * The passphrase is never written anywhere but back to the agent, and its
 * memory is cleared once it's sent.
 */
#include <errno.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gem-ui.h"

#define TEXT_W 360
#define ICON 32
#define INNER (GEM_BORDER + 2 * GEM_PAD)

/* GnuPG's error codes, from the pinentry source (GPG_ERR_SOURCE_PINENTRY). */
#define ERR(code) ((5u << 24) | (code))
#define ERR_CANCELED ERR(99)
#define ERR_NOT_CONFIRMED ERR(114)
#define ERR_TIMEOUT ERR(62)
#define ERR_UNKNOWN_COMMAND ERR(275)

enum kind { ASK_PIN, ASK_CONFIRM, ASK_MESSAGE };
enum { HIT_OK, HIT_CANCEL, HIT_NOTOK };

/* What the agent has told us to say. */
static struct {
	char *title, *desc, *prompt, *error, *ok, *cancel, *notok;
	char *repeat;          /* SETREPEAT: ask twice, with this prompt */
	char *repeat_error;
	int timeout;           /* seconds, 0: none */
} say;

/* The box on screen. */
static struct {
	GtkWidget *window, *area, *field, *field2;
	GArray *hits;
	enum kind kind;
	char **lines;          /* the description, wrapped */
	char *mismatch;        /* the repeat didn't match: shown as the error */
	int answer;            /* HIT_*, or -1 while waiting */
	bool timed_out;
	int field_y, field2_y, h, w;
	const char *buttons[3];
	int button_ids[3], n_buttons, button_w;
} box;

/* ---- The protocol ------------------------------------------------------- */

/* %XX escapes, as the agent sends text. */
static char *unescape(const char *s) {
	GString *out = g_string_new(NULL);
	for (; *s != '\0'; s++) {
		if (s[0] == '%' && g_ascii_isxdigit(s[1]) && g_ascii_isxdigit(s[2])) {
			g_string_append_c(out, (char)(g_ascii_xdigit_value(s[1]) * 16 +
				g_ascii_xdigit_value(s[2])));
			s += 2;
		} else {
			g_string_append_c(out, *s);
		}
	}
	return g_string_free(out, FALSE);
}

/* A data line's escapes: %, CR and LF. */
static void send_data(const char *text) {
	GString *line = g_string_new("D ");
	for (const char *p = text; *p != '\0'; p++) {
		if (*p == '%' || *p == '\r' || *p == '\n') {
			g_string_append_printf(line, "%%%02X", (unsigned char)*p);
		} else {
			g_string_append_c(line, *p);
		}
	}
	g_string_append_c(line, '\n');
	fputs(line->str, stdout);
	/* It may have held the passphrase. */
	memset(line->str, 0, line->len);
	g_string_free(line, TRUE);
}

static void reply_ok(void) {
	fputs("OK\n", stdout);
	fflush(stdout);
}

static void reply_err(unsigned code, const char *why) {
	printf("ERR %u %s <Pinentry>\n", code, why);
	fflush(stdout);
}

/* A button's label without its mnemonic's underscore. */
static char *label(const char *agent, const char *fallback) {
	if (agent == NULL) {
		return g_strdup(fallback);
	}
	GString *s = g_string_new(NULL);
	for (const char *p = agent; *p != '\0'; p++) {
		if (*p == '_' && p[1] != '\0') {
			p++;
		}
		g_string_append_c(s, *p);
	}
	return g_string_free(s, FALSE);
}

static void set(char **slot, const char *value) {
	g_free(*slot);
	*slot = value != NULL && value[0] != '\0' ? unescape(value) : NULL;
}

static void reset(void) {
	char **slots[] = { &say.title, &say.desc, &say.prompt, &say.error, &say.ok,
		&say.cancel, &say.notok, &say.repeat, &say.repeat_error };
	for (size_t i = 0; i < G_N_ELEMENTS(slots); i++) {
		g_clear_pointer(slots[i], g_free);
	}
	say.timeout = 0;
}

/* ---- The box --------------------------------------------------------------- */

/* A key, cut out of a black square. */
static const char *const key_icon[] = {
	"................",
	"....######......",
	"...########.....",
	"..###....###....",
	"..##......##....",
	"..##......##....",
	"..###....###....",
	"...########.....",
	"....######......",
	"......##........",
	"......##........",
	"......####......",
	"......####......",
	"......##........",
	"......#####.....",
	"......#####.....",
	"......##........",
	"......##........",
	"................",
};

static void paint_icon(cairo_t *cr, int x, int y) {
	gem_black(cr);
	gem_fill(cr, x, y, ICON, ICON);
	gem_white(cr);
	gem_bitmap(cr, key_icon, G_N_ELEMENTS(key_icon), x + 8, y + 7);
	gem_black(cr);
}

static int text_x(void) {
	return INNER + ICON + 2 * GEM_PAD;
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	g_array_set_size(box.hits, 0);
	gem_dialog_frame(cr, w, h);
	paint_icon(cr, INNER, INNER);
	int y = INNER;
	for (int i = 0; box.lines[i] != NULL; i++, y += GEM_ROW_H) {
		gem_text(cr, box.lines[i], text_x(), y, GEM_ROW_H);
	}
	if (box.kind == ASK_PIN) {
		const char *prompt = say.prompt != NULL ? say.prompt : "Passphrase:";
		gem_text(cr, prompt, text_x(), box.field_y - GEM_ROW_H, GEM_ROW_H);
		if (box.field2 != NULL) {
			gem_text(cr, say.repeat, text_x(), box.field2_y - GEM_ROW_H, GEM_ROW_H);
		}
	}
	/* What went wrong last time, inverted. */
	const char *error = box.mismatch != NULL ? box.mismatch : say.error;
	if (error != NULL) {
		int ey = h - GEM_BORDER - 2 * GEM_PAD - GEM_BUTTON_H - GEM_PAD - GEM_ROW_H;
		int ew = (int)gem_text_width(cr, error) + GEM_PAD;
		gem_fill(cr, text_x() - GEM_PAD / 2, ey, MIN(ew, w - text_x() - INNER),
			GEM_ROW_H);
		gem_white(cr);
		gem_text_clipped(cr, error, text_x(), ey, w - text_x() - INNER, GEM_ROW_H);
		gem_black(cr);
	}
	int by = h - GEM_BORDER - 2 * GEM_PAD - GEM_BUTTON_H;
	int bx = w - INNER - box.n_buttons * box.button_w -
		(box.n_buttons - 1) * GEM_PAD;
	for (int i = 0; i < box.n_buttons; i++) {
		gem_button(cr, box.hits, bx + i * (box.button_w + GEM_PAD), by,
			box.button_w, box.buttons[i], box.button_ids[i],
			box.button_ids[i] == HIT_OK);
	}
}

static void answer(int id) {
	if (box.kind == ASK_PIN && id == HIT_OK && box.field2 != NULL &&
			strcmp(gtk_editable_get_text(GTK_EDITABLE(box.field)),
				gtk_editable_get_text(GTK_EDITABLE(box.field2))) != 0) {
		/* Not the same twice: say so, and ask again. */
		g_free(box.mismatch);
		box.mismatch = g_strdup(say.repeat_error != NULL ? say.repeat_error :
			"The passphrases don't match.");
		/* The first is kept: type the repeat again. */
		gtk_editable_set_text(GTK_EDITABLE(box.field2), "");
		gtk_widget_grab_focus(box.field2);
		gtk_widget_queue_draw(box.area);
		return;
	}
	box.answer = id;
}

static void released(GtkGestureClick *g, int n, double x, double y,
		gpointer data) {
	const struct gem_hit *hit = gem_hit_at(box.hits, x, y);
	if (hit != NULL) {
		answer(hit->id);
	}
}

static gboolean key(GtkEventControllerKey *k, guint keyval, guint code,
		GdkModifierType state, gpointer data) {
	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
		answer(HIT_OK);
		return TRUE;
	}
	if (keyval == GDK_KEY_Escape) {
		answer(box.kind == ASK_MESSAGE ? HIT_OK : HIT_CANCEL);
		return TRUE;
	}
	return FALSE;
}

static gboolean close_request(GtkWindow *w, gpointer data) {
	answer(box.kind == ASK_MESSAGE ? HIT_OK : HIT_CANCEL);
	return TRUE; /* it goes when the answer's sent */
}

static gboolean timed_out(gpointer data) {
	box.timed_out = true;
	box.answer = HIT_CANCEL;
	return G_SOURCE_REMOVE;
}

static GtkWidget *field_at(int y) {
	GtkWidget *f = gtk_password_entry_new();
	gtk_widget_add_css_class(f, "gem-field");
	gtk_editable_set_width_chars(GTK_EDITABLE(f), 1);
	gtk_widget_set_halign(f, GTK_ALIGN_START);
	gtk_widget_set_valign(f, GTK_ALIGN_START);
	gtk_widget_set_margin_start(f, text_x());
	gtk_widget_set_margin_top(f, y);
	gtk_widget_set_size_request(f, box.w - text_x() - INNER, GEM_FIELD_H);
	return f;
}

/* Clears what was typed: GTK's buffer, as far as we can reach it. */
static void wipe(GtkWidget *field) {
	if (field != NULL) {
		gtk_editable_set_text(GTK_EDITABLE(field), "");
	}
}

/* Shows the box and waits for the answer: HIT_OK, HIT_CANCEL or HIT_NOTOK.
 * For a passphrase, *pin gets it (to be cleared by the caller). */
static int ask(enum kind kind, bool one_button, char **pin) {
	box.kind = kind;
	box.answer = -1;
	box.timed_out = false;
	g_clear_pointer(&box.mismatch, g_free);
	const char *desc = say.desc != NULL ? say.desc :
		kind == ASK_PIN ? "Please enter your passphrase." : "";
	box.lines = gem_wrap(desc, TEXT_W);
	int n_lines = g_strv_length(box.lines);

	/* Buttons: [Not OK] Cancel OK, all one width, OK the default. */
	char *ok = label(say.ok, "OK"), *cancel = label(say.cancel, "Cancel");
	char *notok = say.notok != NULL ? label(say.notok, "") : NULL;
	box.n_buttons = 0;
	if (kind == ASK_CONFIRM && notok != NULL && !one_button) {
		box.button_ids[box.n_buttons] = HIT_NOTOK;
		box.buttons[box.n_buttons++] = notok;
	}
	if (kind != ASK_MESSAGE && !one_button) {
		box.button_ids[box.n_buttons] = HIT_CANCEL;
		box.buttons[box.n_buttons++] = cancel;
	}
	box.button_ids[box.n_buttons] = HIT_OK;
	box.buttons[box.n_buttons++] = ok;
	box.button_w = 0;
	for (int i = 0; i < box.n_buttons; i++) {
		box.button_w = MAX(box.button_w, gem_button_width(box.buttons[i]));
	}

	int text_w = 220;
	for (int i = 0; i < n_lines; i++) {
		text_w = MAX(text_w, (int)gem_measure(box.lines[i]));
	}
	int buttons_w = box.n_buttons * box.button_w + (box.n_buttons - 1) * GEM_PAD;
	box.w = MAX(text_x() + text_w, INNER + buttons_w) + INNER;
	int y = INNER + MAX(n_lines * GEM_ROW_H, ICON) + GEM_PAD;
	if (kind == ASK_PIN) {
		y += GEM_ROW_H;
		box.field_y = y;
		y += GEM_FIELD_H + GEM_PAD;
		if (say.repeat != NULL) {
			y += GEM_ROW_H;
			box.field2_y = y;
			y += GEM_FIELD_H + GEM_PAD;
		}
	}
	y += GEM_ROW_H + GEM_PAD; /* room for an error */
	box.h = y + GEM_BUTTON_H + 2 * GEM_PAD + GEM_BORDER;

	box.window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(box.window),
		say.title != NULL ? say.title : "Passphrase");
	gtk_window_set_resizable(GTK_WINDOW(box.window), FALSE);
	gtk_widget_add_css_class(box.window, "gem");
	GtkWidget *overlay = gtk_overlay_new();
	box.area = gem_pixel_area_new(box.w, box.h, paint, NULL);
	/* Something has the keys: the field, or with none, the box. */
	gtk_widget_set_focusable(box.area, kind != ASK_PIN);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), box.area);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(released), NULL);
	gtk_widget_add_controller(box.area, GTK_EVENT_CONTROLLER(click));
	box.field = box.field2 = NULL;
	if (kind == ASK_PIN) {
		box.field = field_at(box.field_y);
		gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box.field);
		if (say.repeat != NULL) {
			box.field2 = field_at(box.field2_y);
			gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box.field2);
		}
	}
	gtk_window_set_child(GTK_WINDOW(box.window), overlay);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key), NULL);
	gtk_widget_add_controller(box.window, keys);
	g_signal_connect(box.window, "close-request", G_CALLBACK(close_request), NULL);
	gtk_window_present(GTK_WINDOW(box.window));
	gtk_widget_grab_focus(box.field != NULL ? box.field : box.area);

	guint timer = say.timeout > 0 ?
		g_timeout_add_seconds(say.timeout, timed_out, NULL) : 0;
	while (box.answer < 0) {
		g_main_context_iteration(NULL, TRUE);
	}
	if (timer != 0 && !box.timed_out) {
		g_source_remove(timer);
	}
	if (pin != NULL && box.answer == HIT_OK && box.field != NULL) {
		*pin = g_strdup(gtk_editable_get_text(GTK_EDITABLE(box.field)));
	}
	wipe(box.field);
	wipe(box.field2);
	gtk_window_destroy(GTK_WINDOW(box.window));
	/* Let it go from the screen before the agent has its answer. */
	while (g_main_context_pending(NULL)) {
		g_main_context_iteration(NULL, FALSE);
	}
	box.window = NULL;
	g_strfreev(box.lines);
	g_free(ok);
	g_free(cancel);
	g_free(notok);
	return box.answer;
}

/* ---- Commands --------------------------------------------------------------- */

static void getpin(void) {
	char *pin = NULL;
	int a = ask(ASK_PIN, false, &pin);
	if (a == HIT_OK && pin != NULL) {
		if (say.repeat != NULL) {
			fputs("S PIN_REPEATED\n", stdout);
		}
		send_data(pin);
		memset(pin, 0, strlen(pin));
		reply_ok();
	} else if (box.timed_out) {
		reply_err(ERR_TIMEOUT, "Timeout");
	} else {
		reply_err(ERR_CANCELED, "Operation cancelled");
	}
	g_free(pin);
	/* An error is for one asking only. */
	g_clear_pointer(&say.error, g_free);
}

static void confirm(const char *args) {
	bool one = strstr(args, "--one-button") != NULL;
	int a = ask(ASK_CONFIRM, one, NULL);
	if (a == HIT_OK) {
		reply_ok();
	} else if (box.timed_out) {
		reply_err(ERR_TIMEOUT, "Timeout");
	} else if (a == HIT_NOTOK) {
		reply_err(ERR_NOT_CONFIRMED, "Not confirmed");
	} else {
		reply_err(ERR_CANCELED, "Operation cancelled");
	}
	g_clear_pointer(&say.error, g_free);
}

static void getinfo(const char *what) {
	if (strcmp(what, "flavor") == 0) {
		send_data("gem");
	} else if (strcmp(what, "version") == 0) {
		send_data("0.1");
	} else if (strcmp(what, "pid") == 0) {
		char pid[24];
		g_snprintf(pid, sizeof pid, "%d", (int)getpid());
		send_data(pid);
	} else if (strcmp(what, "ttyinfo") == 0) {
		send_data("- - -");
	} else {
		reply_err(ERR(280), "Unknown GETINFO"); /* GPG_ERR_ASS_PARAMETER */
		return;
	}
	reply_ok();
}

/* Reads commands until BYE or the agent goes. */
static void serve(void) {
	printf("OK Pleased to meet you, process %d\n", (int)getpid());
	fflush(stdout);
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, stdin)) >= 0) {
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
			line[--n] = '\0';
		}
		if (line[0] == '\0' || line[0] == '#') {
			continue;
		}
		char *args = strchr(line, ' ');
		if (args != NULL) {
			*args++ = '\0';
		} else {
			args = "";
		}
		const char *cmd = line;
		struct { const char *name; char **slot; } texts[] = {
			{ "SETDESC", &say.desc }, { "SETPROMPT", &say.prompt },
			{ "SETTITLE", &say.title }, { "SETERROR", &say.error },
			{ "SETOK", &say.ok }, { "SETCANCEL", &say.cancel },
			{ "SETNOTOK", &say.notok }, { "SETREPEATERROR", &say.repeat_error },
		};
		bool done = false;
		for (size_t i = 0; i < G_N_ELEMENTS(texts) && !done; i++) {
			if (g_ascii_strcasecmp(cmd, texts[i].name) == 0) {
				set(texts[i].slot, args);
				reply_ok();
				done = true;
			}
		}
		if (done) {
			continue;
		}
		if (g_ascii_strcasecmp(cmd, "SETREPEAT") == 0) {
			set(&say.repeat, args[0] != '\0' ? args : "Repeat:");
			reply_ok();
		} else if (g_ascii_strcasecmp(cmd, "SETTIMEOUT") == 0) {
			say.timeout = atoi(args);
			reply_ok();
		} else if (g_ascii_strcasecmp(cmd, "GETPIN") == 0) {
			getpin();
		} else if (g_ascii_strcasecmp(cmd, "CONFIRM") == 0) {
			confirm(args);
		} else if (g_ascii_strcasecmp(cmd, "MESSAGE") == 0) {
			ask(ASK_MESSAGE, true, NULL);
			reply_ok();
		} else if (g_ascii_strcasecmp(cmd, "GETINFO") == 0) {
			getinfo(args);
		} else if (g_ascii_strcasecmp(cmd, "RESET") == 0) {
			reset();
			reply_ok();
		} else if (g_ascii_strcasecmp(cmd, "BYE") == 0) {
			reply_ok();
			break;
		} else if (g_ascii_strcasecmp(cmd, "OPTION") == 0 ||
				g_ascii_strcasecmp(cmd, "NOP") == 0 ||
				g_ascii_strcasecmp(cmd, "CLEARPASSPHRASE") == 0 ||
				g_ascii_strncasecmp(cmd, "SET", 3) == 0) {
			/* SETKEYINFO, SETQUALITYBAR... and the agent's options:
			 * nothing to do with them here. */
			reply_ok();
		} else {
			reply_err(ERR_UNKNOWN_COMMAND, "Unknown IPC command");
		}
	}
	free(line);
}

/* No Wayland display to ask on: a terminal pinentry, as GnuPG would have
 * started. */
static void fall_back(char *argv[]) {
	const char *others[] = { "/usr/bin/pinentry-curses", "/usr/bin/pinentry-tty" };
	for (size_t i = 0; i < G_N_ELEMENTS(others); i++) {
		argv[0] = (char *)others[i];
		execv(others[i], argv);
	}
	fprintf(stderr, "pinentry-gem: no display, and no pinentry-curses: %s\n",
		strerror(errno));
	exit(1);
}

int main(int argc, char *argv[]) {
	const char *wayland = g_getenv("WAYLAND_DISPLAY");
	if (wayland == NULL || wayland[0] == '\0') {
		fall_back(argv);
	}
	/* Not GtkApplication: several may be up at once, and none wants the
	 * session bus to itself. */
	gdk_set_allowed_backends("wayland");
	/* Its app ID, for the menu bar's name (org.gemwm.Pinentry.desktop). */
	g_set_prgname("org.gemwm.Pinentry");
	if (!gtk_init_check()) {
		fall_back(argv);
	}
	gem_ui_load_css();
	box.hits = gem_hits_new();
	serve();
	return 0;
}
