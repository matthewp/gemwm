/*
 * The .puz format (Across Lite): a fixed header, then the solution and the
 * player's grid as one byte per cell in reading order ('.' is a black
 * square, '-' an empty one), then NUL-terminated strings in Windows-1252:
 * title, author, copyright, the clues, notes. The clues come in the order
 * the numbers are handed out, across before down for a cell that starts
 * both, so the numbering has to be worked out to know which is which.
 */
#include <string.h>
#include "puz.h"

#define MAGIC "ACROSS&DOWN"
#define OFFSET_MAGIC 0x02
#define OFFSET_WIDTH 0x2c
#define OFFSET_HEIGHT 0x2d
#define OFFSET_CLUES 0x2e
#define OFFSET_SCRAMBLED 0x32
#define OFFSET_BOARD 0x34

static void clue_free(void *data) {
	struct clue *c = data;
	g_free(c->text);
	g_free(c);
}

void puz_free(struct puzzle *p) {
	if (p == NULL) {
		return;
	}
	g_free(p->title);
	g_free(p->author);
	g_free(p->copyright);
	g_free(p->solution);
	g_free(p->grid);
	g_free(p->numbers);
	g_clear_pointer(&p->across, g_ptr_array_unref);
	g_clear_pointer(&p->down, g_ptr_array_unref);
	g_free(p);
}

static char *to_utf8(const char *s) {
	char *u = g_convert(s, -1, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
	return u != NULL ? u : g_utf8_make_valid(s, -1);
}

struct puzzle *puz_load(const char *path, GError **error) {
	char *data;
	gsize len;
	if (!g_file_get_contents(path, &data, &len, error)) {
		return NULL;
	}
	const unsigned char *b = (const unsigned char *)data;
	if (len < OFFSET_BOARD || memcmp(data + OFFSET_MAGIC, MAGIC,
			sizeof(MAGIC)) != 0) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			"%s isn't a crossword (.puz) file", path);
		g_free(data);
		return NULL;
	}
	int w = b[OFFSET_WIDTH], h = b[OFFSET_HEIGHT];
	int n_clues = b[OFFSET_CLUES] | b[OFFSET_CLUES + 1] << 8;
	size_t cells = (size_t)w * h;
	if (w == 0 || h == 0 || len < OFFSET_BOARD + 2 * cells) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
			"%s is cut short", path);
		g_free(data);
		return NULL;
	}

	struct puzzle *p = g_new0(struct puzzle, 1);
	p->width = w;
	p->height = h;
	p->scrambled = (b[OFFSET_SCRAMBLED] | b[OFFSET_SCRAMBLED + 1] << 8) != 0;
	p->solution = g_strndup(data + OFFSET_BOARD, cells);
	p->grid = g_strndup(data + OFFSET_BOARD + cells, cells);
	for (size_t i = 0; i < cells; i++) {
		char c = p->grid[i];
		p->grid[i] = p->solution[i] == '.' ? '.' :
			g_ascii_isalpha(c) ? g_ascii_toupper(c) : ' ';
	}

	/* The strings, each checked to be inside the file. */
	const char *s = data + OFFSET_BOARD + 2 * cells, *end = data + len;
	char *strings[3 + 1024] = { NULL };
	int n_strings = MIN(3 + n_clues, (int)G_N_ELEMENTS(strings));
	for (int i = 0; i < n_strings && s < end; i++) {
		const char *nul = memchr(s, '\0', end - s);
		if (nul == NULL) {
			break;
		}
		strings[i] = to_utf8(s);
		s = nul + 1;
	}
	p->title = strings[0] ? strings[0] : g_strdup("Untitled");
	p->author = strings[1] ? strings[1] : g_strdup("");
	/* Some say "by X" already; we add the "by" ourselves. */
	if (g_ascii_strncasecmp(p->author, "by ", 3) == 0) {
		memmove(p->author, p->author + 3, strlen(p->author + 3) + 1);
	}
	p->copyright = strings[2] ? strings[2] : g_strdup("");

	/* Number the grid, handing out the clues as we go. */
	p->numbers = g_new0(int, cells);
	p->across = g_ptr_array_new_with_free_func(clue_free);
	p->down = g_ptr_array_new_with_free_func(clue_free);
	int number = 0, next = 3;
	for (int r = 0; r < h; r++) {
		for (int c = 0; c < w; c++) {
			if (puz_black(p, r, c)) {
				continue;
			}
			bool across = (c == 0 || puz_black(p, r, c - 1)) &&
				c + 1 < w && !puz_black(p, r, c + 1);
			bool down = (r == 0 || puz_black(p, r - 1, c)) &&
				r + 1 < h && !puz_black(p, r + 1, c);
			if (!across && !down) {
				continue;
			}
			p->numbers[r * w + c] = ++number;
			for (int d = 0; d < 2; d++) {
				bool is_across = d == 0;
				if (is_across ? !across : !down) {
					continue;
				}
				struct clue *clue = g_new0(struct clue, 1);
				clue->number = number;
				clue->row = r;
				clue->col = c;
				clue->text = next < n_strings && strings[next] ?
					g_steal_pointer(&strings[next]) : g_strdup("");
				next++;
				int len = 0;
				while (is_across ? c + len < w && !puz_black(p, r, c + len) :
						r + len < h && !puz_black(p, r + len, c)) {
					len++;
				}
				clue->length = len;
				g_ptr_array_add(is_across ? p->across : p->down, clue);
			}
		}
	}
	for (int i = 3; i < (int)G_N_ELEMENTS(strings); i++) {
		g_free(strings[i]);
	}
	g_free(data);
	return p;
}

struct clue *puz_clue_at(const struct puzzle *p, int row, int col, bool across) {
	if (puz_black(p, row, col)) {
		return NULL;
	}
	/* Back to the start of the word, then find the clue that starts there. */
	while (across ? col > 0 && !puz_black(p, row, col - 1) :
			row > 0 && !puz_black(p, row - 1, col)) {
		across ? col-- : row--;
	}
	GPtrArray *list = across ? p->across : p->down;
	for (guint i = 0; i < list->len; i++) {
		struct clue *c = g_ptr_array_index(list, i);
		if (c->row == row && c->col == col) {
			return c;
		}
	}
	return NULL;
}
