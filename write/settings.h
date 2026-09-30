#pragma once

#include <glib.h>

/* Preferences: a tiny key file under the user's config directory
 * (~/.config/gemwrite/settings.ini, group [general]). Reads fall back to the
 * given default when the file or key is missing; writes keep other keys. */

gboolean wp_settings_get_bool(const char *key, gboolean def);
void     wp_settings_set_bool(const char *key, gboolean on);
int      wp_settings_get_int(const char *key, int def);
void     wp_settings_set_int(const char *key, int value);
