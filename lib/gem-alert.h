/*
 * A GEM alert box over a window, as GEM's form_alert drew it: an icon, the
 * message, and a row of buttons of one width at the right, the default
 * with a thicker border. It holds the window while it's up: the rest takes
 * no input. Return answers with the default, Escape with the cancel
 * button.
 *
 * host is a GtkOverlay holding the window's content; the alert goes over
 * it. done gets the index of the button pressed.
 */
#ifndef GEMWM_GEM_ALERT_H
#define GEMWM_GEM_ALERT_H

#include <gtk/gtk.h>

enum gem_alert_icon {
	GEM_ALERT_NONE,
	GEM_ALERT_NOTE,      /* ! */
	GEM_ALERT_QUESTION,  /* ? */
	GEM_ALERT_STOP,
};

typedef void (*gem_alert_fn)(int button, void *data);

/* buttons is NULL-terminated; def and cancel index it (cancel -1: Escape
 * does nothing). */
void gem_alert(GtkOverlay *host, enum gem_alert_icon icon, const char *text,
	const char *const *buttons, int def, int cancel, gem_alert_fn done,
	void *data);
/* Whether host has an alert (or another held box, see gem_hold) up. */
bool gem_alert_up(GtkOverlay *host);

/* For other boxes that hold a window the same way (the item selector):
 * box goes over host, centred, and everything else waits until
 * gem_release. */
void gem_hold(GtkOverlay *host, GtkWidget *box);
void gem_release(GtkOverlay *host, GtkWidget *box);

#endif
