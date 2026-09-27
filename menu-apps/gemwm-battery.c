/*
 * gemwm-battery: the battery at the right of the menu bar, as a menu app.
 *
 * A 1-bit battery filled to the charge, with a lightning bolt over it on
 * mains power, then the percentage, inverted when it's 10% or less. It
 * shows nothing on machines without a battery, or with [battery] mode = off
 * in ~/.config/gemwm/config.
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "menu-app.h"

#define LOW 10    /* percent at which the charge is shown inverted */
#define PERIOD 5  /* seconds between checks, so plugging in shows promptly */

struct battery {
	bool present, plugged;
	int percent;
};

/* The first line of a sysfs attribute, or "" if it can't be read. */
static void read_attr(const char *dir, const char *name, char *out, size_t n) {
	char path[512];
	snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", dir, name);
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return;
	}
	if (fgets(out, (int)n, f) == NULL) {
		out[0] = '\0';
	}
	out[strcspn(out, "\n")] = '\0';
	fclose(f);
}

static long read_long(const char *dir, const char *name) {
	char v[32];
	read_attr(dir, name, v, sizeof(v));
	return v[0] != '\0' ? atol(v) : -1;
}

/* Reads the system's batteries from sysfs (a laptop's own; not a mouse's or
 * a phone's). With two, the charge is their combined one. */
static struct battery battery_read(void) {
	struct battery b = { 0 };
	long now = 0, full = 0, percents = 0, count = 0;
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (d != NULL && (e = readdir(d)) != NULL) {
		char v[32];
		if (e->d_name[0] == '.') {
			continue;
		}
		read_attr(e->d_name, "type", v, sizeof(v));
		if (strcmp(v, "Battery") != 0) {
			continue;
		}
		read_attr(e->d_name, "scope", v, sizeof(v));
		if (strcmp(v, "Device") == 0 || read_long(e->d_name, "present") == 0) {
			continue;
		}
		b.present = true;
		read_attr(e->d_name, "status", v, sizeof(v));
		/* "Not charging" is plugged in but held, e.g. by a charge limit. */
		if (strcmp(v, "Charging") == 0 || strcmp(v, "Full") == 0 ||
				strcmp(v, "Not charging") == 0) {
			b.plugged = true;
		}
		long n = read_long(e->d_name, "energy_now");
		long f = read_long(e->d_name, "energy_full");
		if (n < 0 || f <= 0) {
			n = read_long(e->d_name, "charge_now");
			f = read_long(e->d_name, "charge_full");
		}
		if (n >= 0 && f > 0) {
			now += n;
			full += f;
		}
		long c = read_long(e->d_name, "capacity");
		if (c >= 0) {
			percents += c;
			count++;
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	/* The kernel's capacity for one battery; our own sum for several. */
	int percent = count == 1 ? (int)percents :
		full > 0 ? (int)((now * 100 + full / 2) / full) :
		count > 0 ? (int)(percents / count) : 0;
	b.percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	return b;
}

enum { BODY_W = 18, BODY_H = 10, NUB_W = 2, NUB_H = 4, IN_W = 14, IN_H = 6,
	W = BODY_W + NUB_W, H = BODY_H };

/* The icon as the bar's "bitmap:WxH:hex": rows of bits, first pixel in the
 * top bit, 1 for black. */
static void icon(const struct battery *b, char *out, size_t n) {
	static const char *bolt[IN_H] = {
		"....##.",
		"...##..",
		"..#####",
		"#####..",
		"..##...",
		".##....",
	};
	bool px[H][W] = { 0 };
	for (int x = 0; x < BODY_W; x++) {
		px[0][x] = px[BODY_H - 1][x] = true;
	}
	for (int y = 0; y < BODY_H; y++) {
		px[y][0] = px[y][BODY_W - 1] = true;
	}
	for (int y = (BODY_H - NUB_H) / 2; y < (BODY_H + NUB_H) / 2; y++) {
		for (int x = BODY_W; x < W; x++) {
			px[y][x] = true;
		}
	}
	int fill = (IN_W * b->percent + 50) / 100;
	if (fill == 0 && b->percent > 0) {
		fill = 1;
	}
	for (int y = 0; y < IN_H; y++) {
		for (int x = 0; x < fill; x++) {
			px[2 + y][2 + x] = true;
		}
	}
	if (b->plugged) {
		/* Drawn in the opposite colour to what's under it. */
		int bx = 2 + (IN_W - 7) / 2;
		for (int y = 0; y < IN_H; y++) {
			for (int x = 0; x < 7; x++) {
				if (bolt[y][x] == '#') {
					px[2 + y][bx + x] = !px[2 + y][bx + x];
				}
			}
		}
	}

	int len = snprintf(out, n, "bitmap:%dx%d:", W, H);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x += 8) {
			unsigned byte = 0;
			for (int bit = 0; bit < 8; bit++) {
				if (x + bit < W && px[y][x + bit]) {
					byte |= 0x80 >> bit;
				}
			}
			len += snprintf(out + len, n - len, "%02x", byte);
		}
	}
}

static void show(void) {
	struct battery b = battery_read();
	if (!b.present) {
		app_show("");
		return;
	}
	char line[256];
	icon(&b, line, sizeof(line));
	size_t len = strlen(line);
	snprintf(line + len, sizeof(line) - len, "\t%d%%%s", b.percent,
		!b.plugged && b.percent <= LOW ? "\tinverse" : "");
	app_show(line);
}

int main(void) {
	char mode[16];
	if (app_config("battery", "mode", mode, sizeof(mode)) &&
			strcmp(mode, "off") == 0) {
		app_show("");
		int button;
		while (app_wait(-1, &button) != APP_EXIT) {
		}
		return 0;
	}
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	struct itimerspec spec = {
		.it_value = { .tv_sec = PERIOD },
		.it_interval = { .tv_sec = PERIOD },
	};
	timerfd_settime(fd, 0, &spec, NULL);
	show();
	int button;
	for (;;) {
		switch (app_wait(fd, &button)) {
		case APP_TIMER: {
			uint64_t expirations;
			if (read(fd, &expirations, sizeof(expirations)) > 0) {
				show();
			}
			break;
		}
		case APP_CLICK:
			break;
		case APP_EXIT:
			return 0;
		}
	}
}
