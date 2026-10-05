/*
 * Senders' logos, for views that show who mail is from (the newsletter
 * shelf). Found once per sending domain, and kept in
 * ~/.cache/gemmail/logos:
 *
 *   1. BIMI: the logo the domain publishes for its mail, in DNS
 *      (default._bimi.<domain>), an SVG;
 *   2. its website's icon (apple-touch-icon.png, favicon.ico);
 *   3. else none, and the view draws a badge.
 *
 * Only the domain is looked up, never anything from a message, so nobody
 * learns which mail you've opened. A domain with no logo is asked again
 * after a week.
 */
#ifndef GEMWM_MAIL_LOGOS_H
#define GEMWM_MAIL_LOGOS_H

#include <cairo.h>

/* ready is called (on the main thread) each time a logo has been found,
 * or found not to be there, so views can draw again. */
void logos_init(void (*ready)(void *data), void *data);

/* The logo for an address's domain, LOGO_SIZE square (the logo centred in
 * it, its own background kept), or NULL: not found yet (it's being looked
 * for), or there isn't one. Owned by the cache. */
#define LOGO_SIZE 64
cairo_surface_t *logo_for(const char *address);

#endif
