/*
 * gemwm-clock: the clock at the right of the menu bar, as a menu app.
 *
 *   gemwm-clock [--12h | --24h] [--seconds]
 *
 * Without options, [clock] in ~/.config/gemwm/config decides:
 *   mode = 24h | 12h | off
 *   seconds = yes | no
 * A click switches between 24-hour and 12-hour time; its tooltip is the
 * date.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "menu-app.h"

static bool h24 = true, seconds = false;

/* Arms the timer for the next whole minute (or second), on the wall clock.
 * CANCEL_ON_SET wakes us if the time jumps (resume, NTP, timezone), so we
 * re-arm instead of drifting. */
static void arm(int fd) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	long period = seconds ? 1 : 60;
	struct itimerspec spec = {
		.it_value = { .tv_sec = (now.tv_sec / period + 1) * period },
		.it_interval = { .tv_sec = period },
	};
	timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
		&spec, NULL);
}

static void show(void) {
	const char *format = h24 ? (seconds ? "%H:%M:%S" : "%H:%M") :
		(seconds ? "%l:%M:%S %p" : "%l:%M %p");
	char now[64], date[64], line[160];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	if (strftime(now, sizeof(now), format, &tm) > 0 &&
			strftime(date, sizeof(date), "%A %-d %B %Y", &tm) > 0) {
		/* No icon, no flags; %l pads single-digit hours with a space. */
		snprintf(line, sizeof(line), "\t%s\t\t%s", now + strspn(now, " "),
			date);
		app_show(line);
	}
}

int main(int argc, char *argv[]) {
	bool on = true;
	char value[32];
	if (app_config("clock", "mode", value, sizeof(value))) {
		on = strcmp(value, "off") != 0;
		h24 = strcmp(value, "12h") != 0;
	}
	if (app_config("clock", "seconds", value, sizeof(value))) {
		seconds = strcmp(value, "yes") == 0 || strcmp(value, "true") == 0;
	}
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--12h") == 0 || strcmp(argv[i], "--24h") == 0) {
			on = true;
			h24 = strcmp(argv[i], "--24h") == 0;
		} else if (strcmp(argv[i], "--seconds") == 0) {
			seconds = true;
		} else {
			fprintf(stderr, "usage: gemwm-clock [--12h | --24h] [--seconds]\n");
			return 2;
		}
	}

	int fd = -1;
	if (on) {
		fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
		arm(fd);
		show();
	} else {
		app_show("");
	}
	int button;
	for (;;) {
		switch (app_wait(fd, &button)) {
		case APP_TIMER: {
			uint64_t expirations;
			if (read(fd, &expirations, sizeof(expirations)) < 0 &&
					errno == ECANCELED) {
				arm(fd);
			}
			show();
			break;
		}
		case APP_CLICK:
			if (on) {
				h24 = !h24;
				show();
			}
			break;
		case APP_EXIT:
			return 0;
		}
	}
}
