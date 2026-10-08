/*
 * gemwm-solitaire: Klondike, the solitaire of every desktop, drawn as GEM
 * would have drawn it: white cards with black outlines on the ST palette's
 * dark green, hearts and diamonds in red, and the court cards' letters in
 * the ST's own font, five times over. Cards are moved as GEM moved
 * windows: an outline follows the pointer, and the cards go there when you
 * let go. A double-click sends a card home to its foundation; a card a move
 * leaves face down turns over by itself; and once every card is face up,
 * the rest go home on their own. Winning ends in the cascade.
 *
 * Draw One or Draw Three, and the statistics (played, won, streaks) are
 * kept in ~/.local/state/gemwm/solitaire.
 */
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "app-menu.h"
#include "gem-alert.h"
#include "gem-draw.h"
#include "gem-ui.h"

#define CW 106         /* a card: half as big again as Windows' 71 x 96 */
#define CH 144
#define GAP 15         /* between columns */
#define MARGIN 12
#define ROW2 22        /* between the top row and the tableau */
#define DOWN_FAN 7     /* a face-down card's edge showing */
#define UP_FAN 25      /* a face-up card's index showing */
#define MIN_FAN 12     /* squeezed, in a tall column */
#define WASTE_FAN 20   /* Draw Three's fanned waste */
#define INFO_H 20      /* the info line */
#define DRAG_START 3   /* pixels the pointer moves before it's a drag */

/* The table: $040 of the ST's palette. */
#define TABLE 0.0, 0.573, 0.0
#define SLOT 0.0, 0.40, 0.0
#define RED_INK 0.8, 0.0, 0.0
#define NAVY 0.0, 0.0, 0.53

/* Piles: the stock and waste, four foundations, seven tableau columns. */
enum { STOCK, WASTE, FOUND0, TABLE0 = FOUND0 + 4, N_PILES = TABLE0 + 7 };
#define IS_FOUND(p) ((p) >= FOUND0 && (p) < TABLE0)
#define IS_TABLE(p) ((p) >= TABLE0)

/* A card is 0..51: its suit (clubs, diamonds, hearts, spades), then its
 * rank, ace to king. */
#define SUIT(c) ((c) / 13)
#define RANK(c) ((c) % 13 + 1)
#define IS_RED(c) (SUIT(c) == 1 || SUIT(c) == 2)

struct pile {
	int n;
	signed char card[52];
};

/* Everything a move changes, so undo is a copy. */
struct state {
	struct pile pile[N_PILES];
	bool up[52];
	int moves;
	int fanned;    /* Draw Three: cards from the last draw still showing */
};

/* Game menu, Options menu; menu item ids. */
enum {
	ACT_NEW = 1, ACT_UNDO, ACT_STATS, ACT_CLOSE, ACT_DRAW_ONE, ACT_DRAW_THREE,
};

static struct {
	GtkWidget *window, *area;
	GtkOverlay *host;
	struct app_menu *menu;

	struct state s;
	GArray *undo;          /* struct state, oldest first */
	int draw;              /* 1 or 3 */
	gint64 started;        /* when the first move was made, 0 before */
	int seconds;           /* when won: how long it took */
	bool counted;          /* this game is in the statistics as played */
	bool won;
	guint tick, finishing;

	/* The layout, from the last paint. */
	int w, h, x0, fan[7];

	/* A press on a card, and the outline once it moves. */
	bool pressing, dragging, from_stock;
	int from, from_index;
	double press_x, press_y, x, y;

	/* The cascade: cards bouncing away, leaving trails on a picture of
	 * the table. */
	cairo_surface_t *cascade;
	guint cascade_timer;
	struct state before;   /* the won table, back once the cards have flown */
	int flying;            /* the card in the air, or -1 */
	double fx, fy, vx, vy;
	int next_found;

	struct { int played, won, streak, best; } stats;
} ui;

/* ---- Saved: options and statistics -------------------------------------- */

static char *state_path(void) {
	return g_build_filename(g_get_user_state_dir(), "gemwm", "solitaire", NULL);
}

static void load_saved(void) {
	char *path = state_path();
	GKeyFile *kf = g_key_file_new();
	ui.draw = 1;
	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		ui.draw = g_key_file_get_integer(kf, "options", "draw", NULL) == 3 ? 3 : 1;
		ui.stats.played = g_key_file_get_integer(kf, "stats", "played", NULL);
		ui.stats.won = g_key_file_get_integer(kf, "stats", "won", NULL);
		ui.stats.streak = g_key_file_get_integer(kf, "stats", "streak", NULL);
		ui.stats.best = g_key_file_get_integer(kf, "stats", "best", NULL);
	}
	g_key_file_unref(kf);
	g_free(path);
}

static void save_saved(void) {
	char *path = state_path();
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_integer(kf, "options", "draw", ui.draw);
	g_key_file_set_integer(kf, "stats", "played", ui.stats.played);
	g_key_file_set_integer(kf, "stats", "won", ui.stats.won);
	g_key_file_set_integer(kf, "stats", "streak", ui.stats.streak);
	g_key_file_set_integer(kf, "stats", "best", ui.stats.best);
	g_key_file_save_to_file(kf, path, NULL);
	g_key_file_unref(kf);
	g_free(dir);
	g_free(path);
}

/* A game started and left unwon ends the winning streak. */
static void abandon(void) {
	if (ui.counted && !ui.won) {
		ui.stats.streak = 0;
		save_saved();
	}
}

/* ---- The game ----------------------------------------------------------- */

static int top(int p) {
	const struct pile *pl = &ui.s.pile[p];
	return pl->n > 0 ? pl->card[pl->n - 1] : -1;
}

static void push(int p, int c) {
	struct pile *pl = &ui.s.pile[p];
	pl->card[pl->n++] = (signed char)c;
}

static void changed(void) {
	gtk_widget_queue_draw(ui.area);
	app_menu_update(ui.menu);
}

static void deal(void) {
	memset(&ui.s, 0, sizeof(ui.s));
	int deck[52];
	for (int i = 0; i < 52; i++) {
		deck[i] = i;
	}
	for (int i = 51; i > 0; i--) {
		int j = g_random_int_range(0, i + 1);
		int t = deck[i];
		deck[i] = deck[j];
		deck[j] = t;
	}
	int k = 0;
	for (int col = 0; col < 7; col++) {
		for (int row = 0; row <= col; row++) {
			push(TABLE0 + col, deck[k]);
			ui.s.up[deck[k]] = row == col;
			k++;
		}
	}
	while (k < 52) {
		push(STOCK, deck[k++]);
	}
	g_array_set_size(ui.undo, 0);
	ui.started = 0;
	ui.seconds = 0;
	ui.counted = false;
	ui.won = false;
}

static void new_game(void) {
	abandon();
	deal();
	changed();
}

/* The first move starts the clock, and puts the game in the statistics. */
static void begin_move(void) {
	g_array_append_val(ui.undo, ui.s);
	if (ui.started == 0) {
		ui.started = g_get_monotonic_time();
	}
	if (!ui.counted) {
		ui.counted = true;
		ui.stats.played++;
		save_saved();
	}
	ui.s.moves++;
}

static int elapsed(void) {
	if (ui.won) {
		return ui.seconds;
	}
	return ui.started == 0 ? 0 :
		(int)((g_get_monotonic_time() - ui.started) / G_USEC_PER_SEC);
}

static bool fits_foundation(int c, int p) {
	int t = top(p);
	return t < 0 ? RANK(c) == 1 : SUIT(t) == SUIT(c) && RANK(c) == RANK(t) + 1;
}

/* Whether the cards from index on in pile from can go on pile to. */
static bool can_move(int from, int index, int to) {
	const struct pile *pl = &ui.s.pile[from];
	if (to == from || index < 0 || index >= pl->n) {
		return false;
	}
	int c = pl->card[index];
	int count = pl->n - index;
	if (IS_FOUND(to)) {
		return count == 1 && fits_foundation(c, to);
	}
	if (IS_TABLE(to)) {
		int t = top(to);
		return t < 0 ? RANK(c) == 13 :
			ui.s.up[t] && IS_RED(t) != IS_RED(c) && RANK(c) == RANK(t) - 1;
	}
	return false;
}

/* Whether the cards from index on can be picked up together: face up,
 * and (in the tableau) a run of alternating colours down. */
static bool can_lift(int p, int index) {
	const struct pile *pl = &ui.s.pile[p];
	if (index < 0 || index >= pl->n || !ui.s.up[pl->card[index]]) {
		return false;
	}
	if (p == WASTE || IS_FOUND(p)) {
		return index == pl->n - 1;
	}
	if (!IS_TABLE(p)) {
		return false;
	}
	for (int i = index; i + 1 < pl->n; i++) {
		int a = pl->card[i], b = pl->card[i + 1];
		if (IS_RED(a) == IS_RED(b) || RANK(b) != RANK(a) - 1) {
			return false;
		}
	}
	return true;
}

static void check_won(void);
static void maybe_finish(void);

static void move(int from, int index, int to) {
	begin_move();
	struct pile *src = &ui.s.pile[from];
	for (int i = index; i < src->n; i++) {
		push(to, src->card[i]);
	}
	src->n = index;
	if (from == WASTE && ui.s.fanned > 1) {
		ui.s.fanned--;
	}
	/* What a move uncovers turns over by itself. */
	if (IS_TABLE(from) && src->n > 0) {
		ui.s.up[src->card[src->n - 1]] = true;
	}
	check_won();
	maybe_finish();
	changed();
}

/* The stock: the next one or three to the waste, or, once it's empty,
 * the waste turned back over. */
static void draw_stock(void) {
	struct pile *stock = &ui.s.pile[STOCK], *waste = &ui.s.pile[WASTE];
	if (stock->n == 0 && waste->n == 0) {
		return;
	}
	begin_move();
	if (stock->n > 0) {
		int k = MIN(ui.draw, stock->n);
		for (int i = 0; i < k; i++) {
			int c = stock->card[--stock->n];
			ui.s.up[c] = true;
			push(WASTE, c);
		}
		ui.s.fanned = k;
	} else {
		while (waste->n > 0) {
			int c = waste->card[--waste->n];
			ui.s.up[c] = false;
			push(STOCK, c);
		}
		ui.s.fanned = 0;
	}
	changed();
}

/* A card (the top of the waste or a column) home, if it can go. */
static bool send_home(int p) {
	int c = top(p);
	if (c < 0 || !ui.s.up[c] || IS_FOUND(p)) {
		return false;
	}
	for (int f = FOUND0; f < TABLE0; f++) {
		if (fits_foundation(c, f)) {
			move(p, ui.s.pile[p].n - 1, f);
			return true;
		}
	}
	return false;
}

static void undo(void) {
	if (ui.undo->len == 0 || ui.won || ui.finishing) {
		return;
	}
	ui.s = g_array_index(ui.undo, struct state, ui.undo->len - 1);
	g_array_set_size(ui.undo, ui.undo->len - 1);
	changed();
}

/* ---- Finishing and winning ---------------------------------------------- */

/* Once the stock is spent and every card is face up, nothing's left to
 * decide: the cards go home by themselves, a few a second. */
static gboolean finish_step(void *data) {
	int best = -1;
	for (int p = WASTE; p < N_PILES; p++) {
		int c = top(p);
		if (c < 0 || IS_FOUND(p)) {
			continue;
		}
		for (int f = FOUND0; f < TABLE0; f++) {
			if (fits_foundation(c, f) && (best < 0 || RANK(c) < RANK(top(best)))) {
				best = p;
			}
		}
	}
	if (best < 0 || ui.won) {
		ui.finishing = 0;
		return G_SOURCE_REMOVE;
	}
	/* While ui.finishing is set, the move doesn't start another. */
	send_home(best);
	if (ui.won) {
		ui.finishing = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void maybe_finish(void) {
	if (ui.won || ui.finishing || ui.s.pile[STOCK].n > 0 ||
			ui.s.pile[WASTE].n > 0) {
		return;
	}
	for (int p = TABLE0; p < N_PILES; p++) {
		for (int i = 0; i < ui.s.pile[p].n; i++) {
			if (!ui.s.up[ui.s.pile[p].card[i]]) {
				return;
			}
		}
	}
	ui.finishing = g_timeout_add(90, finish_step, NULL);
}

static void paint_table(cairo_t *cr, int w, int h);
static void paint_card(cairo_t *cr, int c, int x, int y);
static void start_cascade(void);

static void check_won(void) {
	for (int f = FOUND0; f < TABLE0; f++) {
		if (ui.s.pile[f].n < 13) {
			return;
		}
	}
	ui.seconds = elapsed();
	ui.won = true;
	ui.stats.won++;
	ui.stats.streak++;
	ui.stats.best = MAX(ui.stats.best, ui.stats.streak);
	save_saved();
	start_cascade();
}

/* ---- Layout ------------------------------------------------------------- */

static void layout(int w, int h) {
	ui.w = w;
	ui.h = h;
	ui.x0 = MAX(MARGIN, (w - (7 * CW + 6 * GAP)) / 2);
	/* A tall column squeezes its face-up cards together to fit. */
	int room = h - INFO_H - MARGIN - (MARGIN + CH + ROW2) - CH;
	for (int col = 0; col < 7; col++) {
		const struct pile *pl = &ui.s.pile[TABLE0 + col];
		int down = 0, up = 0;
		for (int i = 0; i + 1 < pl->n; i++) {
			if (ui.s.up[pl->card[i]]) {
				up++;
			} else {
				down++;
			}
		}
		int fan = UP_FAN;
		if (up > 0 && down * DOWN_FAN + up * UP_FAN > room) {
			fan = MAX(MIN_FAN, (room - down * DOWN_FAN) / up);
		}
		ui.fan[col] = fan;
	}
}

static int pile_x(int p) {
	int col = p == STOCK ? 0 : p == WASTE ? 1 : IS_FOUND(p) ? 3 + p - FOUND0 :
		p - TABLE0;
	return ui.x0 + col * (CW + GAP);
}

static int pile_y(int p) {
	return IS_TABLE(p) ? MARGIN + CH + ROW2 : MARGIN;
}

/* How many of the waste's cards show, fanned: Draw Three shows the last
 * draw's. */
static int waste_showing(void) {
	int n = ui.s.pile[WASTE].n;
	return n == 0 ? 0 : ui.draw == 3 ? CLAMP(ui.s.fanned, 1, MIN(3, n)) : 1;
}

/* Where card index of pile p is drawn. */
static void card_pos(int p, int index, int *x, int *y) {
	*x = pile_x(p);
	*y = pile_y(p);
	if (p == WASTE) {
		int first = ui.s.pile[WASTE].n - waste_showing();
		if (index > first) {
			*x += (index - first) * WASTE_FAN;
		}
	} else if (IS_TABLE(p)) {
		const struct pile *pl = &ui.s.pile[p];
		for (int i = 0; i < index && i < pl->n; i++) {
			*y += ui.s.up[pl->card[i]] ? ui.fan[p - TABLE0] : DOWN_FAN;
		}
	}
}

/* The card under x, y: its pile and index; the index is -1 on an empty
 * pile's place. */
static bool card_at(double x, double y, int *pile, int *index) {
	for (int p = 0; p < N_PILES; p++) {
		const struct pile *pl = &ui.s.pile[p];
		for (int i = pl->n - 1; i >= 0; i--) {
			int cx, cy;
			card_pos(p, i, &cx, &cy);
			if (x >= cx && x < cx + CW && y >= cy && y < cy + CH) {
				*pile = p;
				*index = i;
				return true;
			}
			if (!IS_TABLE(p) && p != WASTE) {
				break; /* only the top of the others is there to hit */
			}
		}
		int px = pile_x(p), py = pile_y(p);
		if (pl->n == 0 && x >= px && x < px + CW && y >= py && y < py + CH) {
			*pile = p;
			*index = -1;
			return true;
		}
	}
	return false;
}

/* ---- Drawing cards ------------------------------------------------------ */

static void ink(cairo_t *cr, int c) {
	if (IS_RED(c)) {
		cairo_set_source_rgb(cr, RED_INK);
	} else {
		gem_black(cr);
	}
}

/* A suit's shape, s pixels tall, centred on cx, cy; upside down if flip.
 * Each part is filled on its own (parts drawn in opposite directions
 * would cancel where they overlap), without antialiasing, so it's pixels. */
static void suit_part_done(cairo_t *cr) {
	cairo_save(cr);
	cairo_identity_matrix(cr);
	cairo_fill(cr);
	cairo_restore(cr);
}

static void suit_circle(cairo_t *cr, double x, double y, double r) {
	cairo_new_path(cr);
	cairo_arc(cr, x, y, r, 0, 2 * G_PI);
	suit_part_done(cr);
}

static void suit_polygon(cairo_t *cr, const double *xy, int n) {
	cairo_new_path(cr);
	cairo_move_to(cr, xy[0], xy[1]);
	for (int i = 1; i < n; i++) {
		cairo_line_to(cr, xy[2 * i], xy[2 * i + 1]);
	}
	cairo_close_path(cr);
	suit_part_done(cr);
}

static void suit_shape(cairo_t *cr, int suit, double cx, double cy, double s,
		bool flip) {
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, s / 2.0, (flip ? -1 : 1) * s / 2.0);
	switch (suit) {
	case 1: { /* diamond */
		static const double d[] = { 0, -1, 0.72, 0, 0, 1, -0.72, 0 };
		suit_polygon(cr, d, 4);
		break;
	}
	case 2: { /* heart */
		static const double v[] = { -0.94, -0.25, 0.94, -0.25, 0, 1 };
		suit_circle(cr, -0.46, -0.42, 0.5);
		suit_circle(cr, 0.46, -0.42, 0.5);
		suit_polygon(cr, v, 3);
		break;
	}
	case 3: { /* spade: a heart upside down, on a stem */
		static const double v[] = { -0.94, 0.08, 0.94, 0.08, 0, -1 };
		static const double stem[] = { 0, 0.3, 0.38, 1, -0.38, 1 };
		suit_circle(cr, -0.46, 0.24, 0.5);
		suit_circle(cr, 0.46, 0.24, 0.5);
		suit_polygon(cr, v, 3);
		suit_polygon(cr, stem, 3);
		break;
	}
	default: { /* club: three leaves on a stem */
		static const double stem[] = { 0, 0, 0.36, 1, -0.36, 1 };
		suit_circle(cr, 0, -0.5, 0.42);
		suit_circle(cr, -0.5, 0.18, 0.42);
		suit_circle(cr, 0.5, 0.18, 0.42);
		suit_polygon(cr, stem, 3);
		break;
	}
	}
	cairo_restore(cr);
}

/* A card's shape: a white rectangle with its corners cut, outlined. */
static void card_blank(cairo_t *cr, int x, int y) {
	gem_white(cr);
	gem_fill(cr, x + 1, y, CW - 2, CH);
	gem_fill(cr, x, y + 1, CW, CH - 2);
	gem_black(cr);
	gem_fill(cr, x + 1, y, CW - 2, 1);
	gem_fill(cr, x + 1, y + CH - 1, CW - 2, 1);
	gem_fill(cr, x, y + 1, 1, CH - 2);
	gem_fill(cr, x + CW - 1, y + 1, 1, CH - 2);
}

static const char *rank_name(int c) {
	static const char *const names[] = { "A", "2", "3", "4", "5", "6", "7",
		"8", "9", "10", "J", "Q", "K" };
	return names[RANK(c) - 1];
}

/* The rank and a small suit, in the top-left corner. */
static void corner_index(cairo_t *cr, int c, int x, int y) {
	ink(cr, c);
	const char *r = rank_name(c);
	double rw = gem_text_width(cr, r);
	gem_text(cr, r, x + 4 + (16 - rw) / 2, y + 2, 16);
	suit_shape(cr, SUIT(c), x + 12, y + 28, 13, false);
}

/* Where the pips of 2 to 10 go: columns (0 left, 1 middle, 2 right), and
 * rows as fractions of the way down. */
static const struct { signed char n, col[10]; float row[10]; } pips[] = {
	{ 2, { 1, 1 }, { 0, 1 } },
	{ 3, { 1, 1, 1 }, { 0, .5f, 1 } },
	{ 4, { 0, 2, 0, 2 }, { 0, 0, 1, 1 } },
	{ 5, { 0, 2, 1, 0, 2 }, { 0, 0, .5f, 1, 1 } },
	{ 6, { 0, 2, 0, 2, 0, 2 }, { 0, 0, .5f, .5f, 1, 1 } },
	{ 7, { 0, 2, 1, 0, 2, 0, 2 }, { 0, 0, .25f, .5f, .5f, 1, 1 } },
	{ 8, { 0, 2, 1, 0, 2, 1, 0, 2 }, { 0, 0, .25f, .5f, .5f, .75f, 1, 1 } },
	{ 9, { 0, 2, 0, 2, 1, 0, 2, 0, 2 },
		{ 0, 0, .333f, .333f, .5f, .667f, .667f, 1, 1 } },
	{ 10, { 0, 2, 1, 0, 2, 0, 2, 1, 0, 2 },
		{ 0, 0, .167f, .333f, .333f, .667f, .667f, .833f, 1, 1 } },
};

static void paint_face(cairo_t *cr, int c, int x, int y) {
	int rank = RANK(c), suit = SUIT(c);
	ink(cr, c);
	if (rank == 1) {
		suit_shape(cr, suit, x + CW / 2.0, y + CH / 2.0, suit == 3 ? 60 : 48,
			false);
	} else if (rank <= 10) {
		const double colx[] = { x + 33, x + CW / 2.0, x + CW - 33 };
		const double top = y + 26, span = CH - 52;
		for (int i = 0; i < pips[rank - 2].n; i++) {
			float r = pips[rank - 2].row[i];
			suit_shape(cr, suit, colx[(int)pips[rank - 2].col[i]], top + r * span,
				20, r > .5f);
		}
	} else {
		/* A court card: a frame, and its letter in the ST font, five
		 * times over, between two pips. */
		int fx = x + 22, fy = y + 19, fw = CW - 44, fh = CH - 38;
		gem_black(cr);
		gem_frame(cr, fx, fy, fw, fh, 1);
		gem_frame(cr, fx + 2, fy + 2, fw - 4, fh - 4, 1);
		ink(cr, c);
		const char *r = rank_name(c);
		cairo_save(cr);
		cairo_translate(cr, fx + (fw - 5 * gem_text_width(cr, r)) / 2.0,
			fy + (fh - 80) / 2.0);
		cairo_scale(cr, 5, 5);
		gem_text(cr, r, 0, 0, 16);
		cairo_restore(cr);
		suit_shape(cr, suit, fx + 12, fy + 15, 14, false);
		suit_shape(cr, suit, fx + fw - 12, fy + fh - 15, 14, true);
	}
}

static void paint_card(cairo_t *cr, int c, int x, int y) {
	card_blank(cr, x, y);
	if (!ui.s.up[c]) {
		/* The back: a navy lattice, inside a thin white border. */
		static cairo_pattern_t *lattice;
		if (lattice == NULL) {
			static const char *const rows[] = {
				"#......#", ".#....#.", "..#..#..", "...##...",
				"...##...", "..#..#..", ".#....#.", "#......#",
			};
			cairo_surface_t *tile = cairo_image_surface_create(
				CAIRO_FORMAT_ARGB32, 8, 8);
			cairo_t *t = cairo_create(tile);
			cairo_set_source_rgb(t, NAVY);
			cairo_paint(t);
			cairo_set_source_rgb(t, 1, 1, 1);
			for (int ry = 0; ry < 8; ry++) {
				for (int rx = 0; rx < 8; rx++) {
					if (rows[ry][rx] == '#') {
						cairo_rectangle(t, rx, ry, 1, 1);
					}
				}
			}
			cairo_fill(t);
			cairo_destroy(t);
			lattice = cairo_pattern_create_for_surface(tile);
			cairo_surface_destroy(tile);
			cairo_pattern_set_extend(lattice, CAIRO_EXTEND_REPEAT);
			cairo_pattern_set_filter(lattice, CAIRO_FILTER_NEAREST);
		}
		cairo_save(cr);
		cairo_translate(cr, x + 3, y + 3);
		cairo_set_source(cr, lattice);
		cairo_rectangle(cr, 0, 0, CW - 6, CH - 6);
		cairo_fill(cr);
		cairo_restore(cr);
		return;
	}
	corner_index(cr, c, x, y);
	/* And again, upside down, in the bottom-right corner. */
	cairo_save(cr);
	cairo_translate(cr, 2 * x + CW, 2 * y + CH);
	cairo_scale(cr, -1, -1);
	corner_index(cr, c, x, y);
	cairo_restore(cr);
	paint_face(cr, c, x, y);
}

/* An empty pile's place: a darker outline on the table. */
static void paint_slot(cairo_t *cr, int p) {
	int x = pile_x(p), y = pile_y(p);
	cairo_set_source_rgb(cr, SLOT);
	gem_fill(cr, x + 1, y, CW - 2, 1);
	gem_fill(cr, x + 1, y + CH - 1, CW - 2, 1);
	gem_fill(cr, x, y + 1, 1, CH - 2);
	gem_fill(cr, x + CW - 1, y + 1, 1, CH - 2);
	if (p == STOCK && ui.s.pile[WASTE].n > 0) {
		/* Click to turn the waste back over: a ring. */
		cairo_set_line_width(cr, 3);
		cairo_arc(cr, x + CW / 2.0, y + CH / 2.0, 22, 0, 2 * G_PI);
		cairo_stroke(cr);
	} else if (IS_FOUND(p)) {
		double w = gem_text_width(cr, "A");
		gem_text(cr, "A", x + (CW - w) / 2, y + (CH - 16) / 2.0, 16);
	}
}

/* GEM's moving outline: dotted, black and white, to show on anything. */
static void outline(cairo_t *cr, int x, int y, int w, int h) {
	for (int i = 0; i < w; i++) {
		(i & 1) ? gem_black(cr) : gem_white(cr);
		gem_fill(cr, x + i, y, 1, 1);
		gem_fill(cr, x + i, y + h - 1, 1, 1);
	}
	for (int i = 0; i < h; i++) {
		(i & 1) ? gem_black(cr) : gem_white(cr);
		gem_fill(cr, x, y + i, 1, 1);
		gem_fill(cr, x + w - 1, y + i, 1, 1);
	}
}

static void paint_info(cairo_t *cr, int w, int h) {
	int y = h - INFO_H;
	gem_white(cr);
	gem_fill(cr, 0, y, w, INFO_H);
	gem_black(cr);
	gem_fill(cr, 0, y, w, 1);
	char *left = ui.won ? g_strdup_printf("Won in %d moves", ui.s.moves) :
		g_strdup_printf("Moves: %d", ui.s.moves);
	gem_text(cr, left, GEM_PAD, y + 1, INFO_H - 1);
	g_free(left);
	const char *mode = ui.draw == 3 ? "Draw Three" : "Draw One";
	gem_text(cr, mode, (w - gem_text_width(cr, mode)) / 2, y + 1, INFO_H - 1);
	int t = elapsed();
	char clock[32];
	g_snprintf(clock, sizeof clock, "Time: %d:%02d", t / 60, t % 60);
	gem_text(cr, clock, w - GEM_PAD - gem_text_width(cr, clock), y + 1,
		INFO_H - 1);
}

static void paint_table(cairo_t *cr, int w, int h) {
	layout(w, h);
	cairo_set_source_rgb(cr, TABLE);
	cairo_paint(cr);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	for (int p = 0; p < N_PILES; p++) {
		const struct pile *pl = &ui.s.pile[p];
		if (pl->n == 0) {
			paint_slot(cr, p);
			continue;
		}
		int first = 0;
		if (p == WASTE) {
			first = pl->n - waste_showing();
		} else if (!IS_TABLE(p)) {
			first = pl->n - 1;
		}
		/* The stock looks like a stack: an edge for every ten. */
		if (p == STOCK) {
			for (int e = MIN(3, (pl->n - 1) / 10); e > 0; e--) {
				card_blank(cr, pile_x(p) + 2 * e, pile_y(p) + e);
			}
		}
		for (int i = first; i < pl->n; i++) {
			int x, y;
			card_pos(p, i, &x, &y);
			paint_card(cr, pl->card[i], x, y);
		}
	}
}

static void paint(cairo_t *cr, int w, int h, void *data) {
	if (ui.cascade != NULL && cairo_image_surface_get_width(ui.cascade) == w &&
			cairo_image_surface_get_height(ui.cascade) == h) {
		cairo_set_source_surface(cr, ui.cascade, 0, 0);
		cairo_paint(cr);
	} else {
		paint_table(cr, w, h);
	}
	if (ui.dragging) {
		int x, y;
		card_pos(ui.from, ui.from_index, &x, &y);
		int lx, ly;
		card_pos(ui.from, ui.s.pile[ui.from].n - 1, &lx, &ly);
		outline(cr, x + (int)(ui.x - ui.press_x), y + (int)(ui.y - ui.press_y),
			CW + lx - x, CH + ly - y);
	}
	paint_info(cr, w, h);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, void *data) {
	gem_draw_pixelated(cr, w, h, paint, NULL);
}

/* ---- The cascade -------------------------------------------------------- */

static void won_answered(int button, void *data);

static void end_cascade(void) {
	if (ui.cascade_timer) {
		g_source_remove(ui.cascade_timer);
		ui.cascade_timer = 0;
	}
	if (ui.cascade != NULL) {
		g_clear_pointer(&ui.cascade, cairo_surface_destroy);
		ui.s = ui.before;
	}
	gtk_widget_queue_draw(ui.area);
	if (gem_alert_up(ui.host)) {
		return;
	}
	int t = ui.seconds;
	char *text = g_strdup_printf("You won!\n%d moves in %d:%02d.\nPlay again?",
		ui.s.moves, t / 60, t % 60);
	static const char *const buttons[] = { "Not Now", "Deal", NULL };
	gem_alert(ui.host, GEM_ALERT_NOTE, text, buttons, 1, 0, won_answered, NULL);
	g_free(text);
}

/* Each card from the foundations, kings first, thrown out, falling and
 * bouncing along the bottom until it's off the side, drawn where it is at
 * every step, so it leaves a trail. */
static gboolean cascade_step(void *data) {
	if (ui.cascade == NULL) {
		ui.cascade_timer = 0;
		return G_SOURCE_REMOVE;
	}
	int w = cairo_image_surface_get_width(ui.cascade);
	int h = cairo_image_surface_get_height(ui.cascade);
	cairo_t *cr = cairo_create(ui.cascade);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	gem_set_font(cr);
	for (int step = 0; step < 3; step++) {
		if (ui.flying < 0) {
			/* The next: round the foundations, from the kings down. */
			int f = -1;
			for (int tries = 0; tries < 4; tries++) {
				int p = FOUND0 + (ui.next_found + tries) % 4;
				if (ui.s.pile[p].n > 0) {
					f = p;
					break;
				}
			}
			if (f < 0) {
				cairo_destroy(cr);
				ui.cascade_timer = 0;
				end_cascade();
				return G_SOURCE_REMOVE;
			}
			ui.next_found = (f - FOUND0 + 1) % 4;
			ui.flying = ui.s.pile[f].card[--ui.s.pile[f].n];
			ui.fx = pile_x(f);
			ui.fy = pile_y(f);
			ui.vx = (g_random_boolean() ? 1 : -1) * g_random_double_range(2, 6);
			ui.vy = -g_random_double_range(0, 8);
			/* What's under it, now it's lifted. */
			if (ui.s.pile[f].n > 0) {
				paint_card(cr, top(f), pile_x(f), pile_y(f));
			} else {
				cairo_set_source_rgb(cr, TABLE);
				gem_fill(cr, pile_x(f), pile_y(f), CW, CH);
			}
		}
		ui.vy += 0.9;
		ui.fx += ui.vx;
		ui.fy += ui.vy;
		int floor = h - INFO_H - CH;
		if (ui.fy > floor) {
			ui.fy = floor;
			ui.vy = -ui.vy * 0.78;
		}
		paint_card(cr, ui.flying, (int)ui.fx, (int)ui.fy);
		if (ui.fx + CW < 0 || ui.fx > w) {
			ui.flying = -1;
		}
	}
	cairo_destroy(cr);
	gtk_widget_queue_draw(ui.area);
	return G_SOURCE_CONTINUE;
}

static void start_cascade(void) {
	int w = gtk_widget_get_width(ui.area), h = gtk_widget_get_height(ui.area);
	if (w <= 0 || h <= 0) {
		end_cascade();
		return;
	}
	ui.before = ui.s;
	ui.cascade = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(ui.cascade);
	gem_set_font(cr);
	paint_table(cr, w, h);
	cairo_destroy(cr);
	ui.flying = -1;
	ui.next_found = 0;
	ui.cascade_timer = g_timeout_add(16, cascade_step, NULL);
}

static void won_answered(int button, void *data) {
	if (button == 1) {
		new_game();
	}
}

/* ---- Input -------------------------------------------------------------- */

static bool busy(void) {
	return gem_alert_up(ui.host);
}

static void pressed(GtkGestureClick *g, int n, double x, double y, void *d) {
	if (busy()) {
		return;
	}
	if (ui.cascade != NULL) {
		end_cascade();
		return;
	}
	if (ui.won || ui.finishing) {
		return;
	}
	int p, i;
	ui.pressing = false;
	if (!card_at(x, y, &p, &i)) {
		return;
	}
	if (n == 2 && i >= 0 && i == ui.s.pile[p].n - 1 && p != STOCK) {
		send_home(p);
		return;
	}
	ui.press_x = ui.x = x;
	ui.press_y = ui.y = y;
	ui.from_stock = p == STOCK;
	if (ui.from_stock || can_lift(p, i)) {
		ui.pressing = true;
		ui.from = p;
		ui.from_index = i;
	}
}

static void motion(GtkEventControllerMotion *c, double x, double y, void *d) {
	if (!ui.pressing || ui.from_stock) {
		return;
	}
	ui.x = x;
	ui.y = y;
	if (!ui.dragging && (fabs(x - ui.press_x) > DRAG_START ||
			fabs(y - ui.press_y) > DRAG_START)) {
		ui.dragging = true;
	}
	if (ui.dragging) {
		gtk_widget_queue_draw(ui.area);
	}
}

/* Where the outline was let go: the pile it covers most that will take
 * the cards. */
static void drop(void) {
	int x, y, lx, ly;
	card_pos(ui.from, ui.from_index, &x, &y);
	card_pos(ui.from, ui.s.pile[ui.from].n - 1, &lx, &ly);
	double ox = x + ui.x - ui.press_x, oy = y + ui.y - ui.press_y;
	double ow = CW + lx - x, oh = CH + ly - y;
	int best = -1;
	double most = 0;
	for (int p = FOUND0; p < N_PILES; p++) {
		if (!can_move(ui.from, ui.from_index, p)) {
			continue;
		}
		int px = pile_x(p), py = pile_y(p), ph = CH;
		if (IS_TABLE(p) && ui.s.pile[p].n > 0) {
			int tx, ty;
			card_pos(p, ui.s.pile[p].n - 1, &tx, &ty);
			ph = ty - py + CH;
		}
		double iw = MIN(ox + ow, px + CW) - MAX(ox, px);
		double ih = MIN(oy + oh, py + ph) - MAX(oy, py);
		if (iw > 0 && ih > 0 && iw * ih > most) {
			most = iw * ih;
			best = p;
		}
	}
	if (best >= 0) {
		move(ui.from, ui.from_index, best);
	}
}

static void released(GtkGestureClick *g, int n, double x, double y, void *d) {
	if (!ui.pressing) {
		return;
	}
	ui.pressing = false;
	if (ui.from_stock) {
		int p, i;
		if (card_at(x, y, &p, &i) && p == STOCK) {
			draw_stock();
		}
	} else if (ui.dragging) {
		ui.dragging = false;
		drop();
	}
	gtk_widget_queue_draw(ui.area);
}

/* ---- Menus -------------------------------------------------------------- */

static void stats_answered(int button, void *data) {
	if (button == 1) {
		memset(&ui.stats, 0, sizeof(ui.stats));
		ui.counted = false;
		save_saved();
	}
}

static void show_stats(void) {
	int pct = ui.stats.played > 0 ? ui.stats.won * 100 / ui.stats.played : 0;
	char *text = g_strdup_printf("Games played: %d\nGames won: %d (%d%%)\n"
		"Winning streak: %d\nLongest streak: %d", ui.stats.played, ui.stats.won,
		pct, ui.stats.streak, ui.stats.best);
	static const char *const buttons[] = { "OK", "Reset", NULL };
	gem_alert(ui.host, GEM_ALERT_NOTE, text, buttons, 0, 0, stats_answered, NULL);
	g_free(text);
}

static void new_answered(int button, void *data) {
	if (button == 1) {
		new_game();
	}
}

/* A new deal; a game under way asks first, since it'll count as lost. */
static void ask_new(void) {
	if (busy()) {
		return;
	}
	if (ui.cascade != NULL) {
		end_cascade();
	}
	if (!ui.counted || ui.won) {
		new_game();
		return;
	}
	static const char *const buttons[] = { "Cancel", "Deal", NULL };
	gem_alert(ui.host, GEM_ALERT_QUESTION,
		"Deal a new game?\nThis one will count as lost.", buttons, 1, 0,
		new_answered, NULL);
}

static void build_menus(struct app_menu *m, void *data) {
	app_menu_add_menu(m, "Game");
	app_menu_add_item(m, ACT_NEW, "New Game", "^N", 0);
	app_menu_add_item(m, ACT_UNDO, "Undo", "^Z",
		ui.undo->len > 0 && !ui.won && !ui.finishing ? 0 : APP_MENU_DISABLED);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_STATS, "Statistics...", NULL, 0);
	app_menu_add_separator(m);
	app_menu_add_item(m, ACT_CLOSE, "Close", "^W", 0);
	app_menu_add_menu(m, "Options");
	app_menu_add_item(m, ACT_DRAW_ONE, "Draw One", NULL,
		ui.draw == 1 ? APP_MENU_CHECKED : 0);
	app_menu_add_item(m, ACT_DRAW_THREE, "Draw Three", NULL,
		ui.draw == 3 ? APP_MENU_CHECKED : 0);
}

static void menu_activate(uint32_t id, void *data) {
	if (busy()) {
		return;
	}
	switch (id) {
	case ACT_NEW:
		ask_new();
		break;
	case ACT_UNDO:
		undo();
		break;
	case ACT_STATS:
		show_stats();
		break;
	case ACT_CLOSE:
		gtk_window_close(GTK_WINDOW(ui.window));
		break;
	case ACT_DRAW_ONE:
	case ACT_DRAW_THREE:
		/* From the next draw on. */
		ui.draw = id == ACT_DRAW_THREE ? 3 : 1;
		save_saved();
		changed();
		break;
	}
}

static gboolean key_pressed(GtkEventControllerKey *c, guint keyval,
		guint code, GdkModifierType mods, void *data) {
	if (busy()) {
		return FALSE;
	}
	bool ctrl = mods & GDK_CONTROL_MASK;
	if (ui.cascade != NULL) {
		end_cascade();
		return TRUE;
	}
	if ((ctrl && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) ||
			keyval == GDK_KEY_F2) {
		ask_new();
	} else if (ctrl && (keyval == GDK_KEY_z || keyval == GDK_KEY_Z)) {
		undo();
	} else if (ctrl && (keyval == GDK_KEY_w || keyval == GDK_KEY_W)) {
		gtk_window_close(GTK_WINDOW(ui.window));
	} else if (keyval == GDK_KEY_space && !ui.won) {
		draw_stock();
	} else {
		return FALSE;
	}
	return TRUE;
}

/* The clock in the info line, while a game's under way. */
static gboolean tick(void *data) {
	if (ui.started != 0 && !ui.won && ui.cascade == NULL) {
		gtk_widget_queue_draw(ui.area);
	}
	return G_SOURCE_CONTINUE;
}

static void window_destroyed(GtkWidget *w, void *data) {
	abandon();
}

/* ---- Setup -------------------------------------------------------------- */

static void activate(GtkApplication *app, void *data) {
	if (ui.window != NULL) {
		gtk_window_present(GTK_WINDOW(ui.window));
		return;
	}
	gem_ui_load_css();
	load_saved();
	ui.undo = g_array_new(FALSE, FALSE, sizeof(struct state));
	deal();

	ui.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(ui.window), "Solitaire");
	gtk_window_set_default_size(GTK_WINDOW(ui.window), 880, 760);
	ui.host = GTK_OVERLAY(gtk_overlay_new());
	ui.area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ui.area), draw, NULL, NULL);
	gtk_overlay_set_child(ui.host, ui.area);
	gtk_window_set_child(GTK_WINDOW(ui.window), GTK_WIDGET(ui.host));

	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(pressed), NULL);
	g_signal_connect(click, "released", G_CALLBACK(released), NULL);
	gtk_widget_add_controller(ui.area, GTK_EVENT_CONTROLLER(click));
	GtkEventController *move_c = gtk_event_controller_motion_new();
	g_signal_connect(move_c, "motion", G_CALLBACK(motion), NULL);
	gtk_widget_add_controller(ui.area, move_c);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(ui.window, keys);
	g_signal_connect(ui.window, "destroy", G_CALLBACK(window_destroyed), NULL);

	ui.menu = app_menu_new(ui.window, build_menus, menu_activate, NULL);
	ui.tick = g_timeout_add_seconds(1, tick, NULL);
	gtk_window_present(GTK_WINDOW(ui.window));
}

int main(int argc, char *argv[]) {
	GtkApplication *app = gtk_application_new("org.gemwm.Solitaire",
		G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
