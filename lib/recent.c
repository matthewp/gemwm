/*
 * Recently opened documents: see recent.h.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "recent.h"

#define KEEP 20 /* kept, so a few gone still leaves enough to show */
#define LABEL_MAX 40

static char *list_path(const char *app) {
	char *name = g_strconcat(app, "-recent", NULL);
	char *path = g_build_filename(g_get_user_state_dir(), "gemwm", name, NULL);
	g_free(name);
	return path;
}

/* All of them, newest first, there or not. */
static char **read_all(const char *app) {
	char *path = list_path(app), *text = NULL;
	char **lines = g_file_get_contents(path, &text, NULL, NULL) ?
		g_strsplit(text, "\n", -1) : g_new0(char *, 1);
	g_free(text);
	g_free(path);
	return lines;
}

static void write_all(const char *app, GPtrArray *paths) {
	char *path = list_path(app);
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	GString *s = g_string_new(NULL);
	for (guint i = 0; i < paths->len && i < KEEP; i++) {
		g_string_append_printf(s, "%s\n", (char *)paths->pdata[i]);
	}
	g_file_set_contents_full(path, s->str, -1, G_FILE_SET_CONTENTS_CONSISTENT,
		0600, NULL);
	g_string_free(s, TRUE);
	g_free(dir);
	g_free(path);
}

/* What recent_list read last, and for which application. */
static struct {
	char *app;
	char **paths;
	int max;
} cache;

static void forget(void) {
	g_clear_pointer(&cache.app, g_free);
	g_clear_pointer(&cache.paths, g_strfreev);
}

void recent_add(const char *app, const char *given) {
	if (given == NULL || strchr(given, '\n') != NULL) {
		return;
	}
	char *path = g_canonicalize_filename(given, NULL);
	forget();
	char **old = read_all(app);
	GPtrArray *paths = g_ptr_array_new();
	g_ptr_array_add(paths, path);
	for (int i = 0; old[i] != NULL; i++) {
		if (old[i][0] != '\0' && strcmp(old[i], path) != 0) {
			g_ptr_array_add(paths, old[i]);
		}
	}
	write_all(app, paths);
	g_ptr_array_free(paths, TRUE);
	g_strfreev(old);
	g_free(path);
}

char **recent_list(const char *app, int max) {
	if (cache.app != NULL && strcmp(cache.app, app) == 0 && cache.max == max) {
		return g_strdupv(cache.paths);
	}
	char **all = read_all(app);
	GPtrArray *out = g_ptr_array_new();
	for (int i = 0; all[i] != NULL && (int)out->len < max; i++) {
		if (all[i][0] != '\0' && g_file_test(all[i], G_FILE_TEST_IS_REGULAR)) {
			g_ptr_array_add(out, g_strdup(all[i]));
		}
	}
	g_ptr_array_add(out, NULL);
	g_strfreev(all);
	forget();
	cache.app = g_strdup(app);
	cache.paths = (char **)g_ptr_array_free(out, FALSE);
	cache.max = max;
	return g_strdupv(cache.paths);
}

void recent_clear(const char *app) {
	forget();
	char *path = list_path(app);
	g_unlink(path);
	g_free(path);
}

/* Its name, and its folder's too if another shown has the same name; long
 * ones shortened in the middle. */
static char *label_for(char **paths, int i) {
	char *base = g_path_get_basename(paths[i]);
	bool twin = false;
	for (int k = 0; paths[k] != NULL; k++) {
		char *other = g_path_get_basename(paths[k]);
		twin |= k != i && strcmp(other, base) == 0;
		g_free(other);
	}
	char *label = base;
	if (twin) {
		char *dir = g_path_get_dirname(paths[i]);
		char *parent = g_path_get_basename(dir);
		label = g_strdup_printf("%s (%s)", base, parent);
		g_free(parent);
		g_free(dir);
		g_free(base);
	}
	glong n = g_utf8_strlen(label, -1);
	if (n > LABEL_MAX) {
		glong keep = (LABEL_MAX - 3) / 2;
		char *head = g_utf8_substring(label, 0, keep);
		char *tail = g_utf8_substring(label, n - keep, n);
		char *shorter = g_strdup_printf("%s...%s", head, tail);
		g_free(head);
		g_free(tail);
		g_free(label);
		label = shorter;
	}
	return label;
}

char **recent_submenu(struct app_menu *m, const char *app, uint32_t first_id,
		uint32_t clear_id) {
	char **paths = recent_list(app, RECENT_SHOWN);
	app_menu_add_submenu(m, "Open Recent",
		paths[0] != NULL ? 0 : APP_MENU_DISABLED);
	for (int i = 0; paths[i] != NULL; i++) {
		char *label = label_for(paths, i);
		app_menu_add_item(m, first_id + i, label, NULL, 0);
		g_free(label);
	}
	app_menu_add_separator(m);
	app_menu_add_item(m, clear_id, "Clear Menu", NULL, 0);
	app_menu_end_submenu(m);
	return paths;
}
