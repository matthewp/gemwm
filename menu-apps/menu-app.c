#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "menu-app.h"

static char in[256]; /* what the bar sent, not yet a whole line */
static size_t in_len;

/* The next whole line from the bar, if one has arrived. */
static bool next_line(char *line, size_t n) {
	char *nl = memchr(in, '\n', in_len);
	if (nl == NULL) {
		return false;
	}
	size_t len = (size_t)(nl - in);
	snprintf(line, n, "%.*s", (int)len, in);
	memmove(in, nl + 1, in_len - len - 1);
	in_len -= len + 1;
	return true;
}

enum app_event app_wait(int timer_fd, int *button) {
	for (;;) {
		char line[64];
		while (next_line(line, sizeof(line))) {
			if (sscanf(line, "click %d", button) == 1) {
				return APP_CLICK;
			}
		}
		struct pollfd fds[2] = {
			{ .fd = STDIN_FILENO, .events = POLLIN },
			{ .fd = timer_fd, .events = POLLIN },
		};
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			return APP_EXIT;
		}
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (in_len == sizeof(in)) {
				in_len = 0; /* nonsense from the bar: drop it */
			}
			ssize_t n = read(STDIN_FILENO, in + in_len, sizeof(in) - in_len);
			if (n <= 0 && !(n < 0 && errno == EINTR)) {
				return APP_EXIT;
			}
			in_len += n > 0 ? (size_t)n : 0;
			continue;
		}
		if (fds[1].revents & POLLIN) {
			return APP_TIMER;
		}
	}
}

void app_show(const char *line) {
	static char *shown;
	if (shown != NULL && strcmp(shown, line) == 0) {
		return;
	}
	free(shown);
	shown = strdup(line);
	printf("%s\n", line);
	fflush(stdout);
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			end[-1] == '\n' || end[-1] == '\r')) {
		*--end = '\0';
	}
	return s;
}

bool app_config(const char *section, const char *key, char *out, size_t n) {
	char path[4096];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg != NULL && xdg[0] != '\0') {
		snprintf(path, sizeof(path), "%s/gemwm/config", xdg);
	} else {
		snprintf(path, sizeof(path), "%s/.config/gemwm/config",
			home ? home : "");
	}
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	char header[128];
	snprintf(header, sizeof(header), "[%s]", section);
	char *line = NULL;
	size_t cap = 0;
	bool in_section = false, found = false;
	while (getline(&line, &cap, f) != -1) {
		char *s = trim(line);
		if (s[0] == '[') {
			in_section = strncmp(s, header, strlen(header)) == 0;
			continue;
		}
		char *eq = strchr(s, '=');
		if (!in_section || s[0] == '#' || eq == NULL) {
			continue;
		}
		*eq = '\0';
		char *value = trim(eq + 1);
		value[strcspn(value, " \t#")] = '\0'; /* a trailing # comment */
		if (strcmp(trim(s), key) == 0) {
			snprintf(out, n, "%s", value);
			found = true;
		}
	}
	free(line);
	fclose(f);
	return found;
}
