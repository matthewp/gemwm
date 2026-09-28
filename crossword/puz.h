/*
 * Across Lite .puz crossword files: the grid, its solution and clues.
 */
#ifndef GEMWM_PUZ_H
#define GEMWM_PUZ_H

#include <glib.h>
#include <stdbool.h>

struct clue {
	int number;
	char *text;    /* UTF-8 */
	int row, col;  /* its first cell */
	int length;
};

struct puzzle {
	int width, height;
	char *title, *author, *copyright; /* UTF-8 */
	char *solution;  /* width * height letters, '.' for black squares */
	char *grid;      /* the player's letters: ' ' for empty, '.' black */
	int *numbers;    /* the number printed in each cell, 0 for none */
	GPtrArray *across, *down; /* struct clue, in number order */
	bool scrambled;  /* the solution is locked: it can't be checked */
};

/* Reads a .puz file; NULL, with error set, if it isn't one. */
struct puzzle *puz_load(const char *path, GError **error);
void puz_free(struct puzzle *p);

static inline bool puz_black(const struct puzzle *p, int row, int col) {
	return p->solution[row * p->width + col] == '.';
}

/* The clue running through a cell, across or down; NULL for none. */
struct clue *puz_clue_at(const struct puzzle *p, int row, int col, bool across);

#endif
