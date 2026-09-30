#include "settings.h"

#include <glib/gstdio.h>

static char *settings_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "gemwrite", "settings.ini", NULL);
}

static GKeyFile *load(void)
{
  GKeyFile *kf = g_key_file_new();
  g_autofree char *path = settings_path();
  g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);   /* keep other keys if any */
  return kf;
}

static void save(GKeyFile *kf)
{
  g_autofree char *path = settings_path();
  g_autofree char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  g_autoptr(GError) err = NULL;
  if (!g_key_file_save_to_file(kf, path, &err))
    g_warning("Could not save settings to %s: %s", path, err->message);
}

gboolean wp_settings_get_bool(const char *key, gboolean def)
{
  g_autoptr(GKeyFile) kf = load();
  if (!g_key_file_has_key(kf, "general", key, NULL)) return def;
  return g_key_file_get_boolean(kf, "general", key, NULL);
}

void wp_settings_set_bool(const char *key, gboolean on)
{
  g_autoptr(GKeyFile) kf = load();
  g_key_file_set_boolean(kf, "general", key, on);
  save(kf);
}

int wp_settings_get_int(const char *key, int def)
{
  g_autoptr(GKeyFile) kf = load();
  if (!g_key_file_has_key(kf, "general", key, NULL)) return def;
  return g_key_file_get_integer(kf, "general", key, NULL);
}

void wp_settings_set_int(const char *key, int value)
{
  g_autoptr(GKeyFile) kf = load();
  g_key_file_set_integer(kf, "general", key, value);
  save(kf);
}
