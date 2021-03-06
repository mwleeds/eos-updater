/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2016 Kinvolk GmbH
 * Copyright © 2017 Endless Mobile, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors:
 *  - Krzesimir Nowak <krzesimir@kinvolk.io>
 *  - Philip Withnall <withnall@endlessm.com>
 */

#include <gio/gio.h>
#include <glib.h>
#include <libeos-updater-util/avahi-service-file.h>
#include <libeos-updater-util/util.h>
#include <string.h>

#ifndef EOS_AVAHI_PORT
#error "EOS_AVAHI_PORT is not defined"
#endif

const gchar * const EOS_UPDATER_AVAHI_SERVICE_TYPE = "_eos_updater._tcp";

const gchar * const eos_avahi_v1_ostree_path = "eos_ostree_path";
const gchar * const eos_avahi_v1_head_commit_timestamp = "eos_head_commit_timestamp";

static gchar *
txt_records_to_string (const gchar **txt_records)
{
  g_autoptr(GString) str = NULL;
  gsize idx;

  str = g_string_new ("");

  for (idx = 0; txt_records[idx] != NULL; idx++)
    {
      g_autofree gchar *record_escaped = g_markup_escape_text (txt_records[idx], -1);
      g_string_append_printf (str, "    <txt-record>%s</txt-record>\n",
                              record_escaped);
    }

  return g_string_free (g_steal_pointer (&str), FALSE);
}

static GBytes *
generate_from_avahi_service_template (const gchar *type,
                                      guint16 port,
                                      const gchar *txt_version,
                                      const gchar **txt_records)
{
  g_autofree gchar *service_group = NULL;
  gsize service_group_len;  /* bytes, not including trailing nul */
  g_autofree gchar *txt_records_str = txt_records_to_string (txt_records);
  g_autofree gchar *type_escaped = g_markup_escape_text (type, -1);
  g_autofree gchar *txt_version_escaped = g_markup_escape_text (txt_version, -1);

  service_group = g_strdup_printf (
      "<service-group>\n"
      "  <name replace-wildcards=\"yes\">EOS update service on %%h</name>\n"
      "  <service>\n"
      "    <type>%s</type>\n"
      "    <port>%" G_GUINT16_FORMAT "</port>\n"
      "    <txt-record>eos_txt_version=%s</txt-record>\n"
      "%s"
      "  </service>\n"
      "</service-group>\n",
      type_escaped, port, txt_version_escaped, txt_records_str);
  service_group_len = strlen (service_group);

  return g_bytes_new_take (g_steal_pointer (&service_group), service_group_len);
}

static gboolean
generate_avahi_service_template_to_file (GFile *path,
                                         const gchar *txt_version,
                                         const gchar **txt_records,
                                         GCancellable *cancellable,
                                         GError **error)
{
  g_autoptr(GBytes) contents = NULL;
  gconstpointer raw;
  gsize raw_len;

  contents = generate_from_avahi_service_template (EOS_UPDATER_AVAHI_SERVICE_TYPE,
                                                   EOS_AVAHI_PORT,
                                                   txt_version,
                                                   txt_records);
  raw = g_bytes_get_data (contents, &raw_len);
  return g_file_replace_contents (path,
                                  raw,
                                  raw_len,
                                  NULL,  /* old ETag */
                                  FALSE,  /* make backup */
                                  G_FILE_CREATE_NONE,
                                  NULL,  /* new ETag */
                                  cancellable,
                                  error);
}

static gchar *
txt_record (const gchar *key,
            const gchar *value)
{
  return g_strdup_printf ("%s=%s", key, value);
}

static gboolean
generate_v1_service_file (const gchar *ostree_path,
                          GDateTime *head_commit_timestamp,
                          GFile *service_file,
                          GCancellable *cancellable,
                          GError **error)
{
  g_autoptr(GPtrArray) txt_records = NULL;
  g_autofree gchar *timestamp_str = NULL;

  timestamp_str = g_date_time_format (head_commit_timestamp, "%s");
  txt_records = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (txt_records, txt_record (eos_avahi_v1_ostree_path,
                                            ostree_path));
  g_ptr_array_add (txt_records, txt_record (eos_avahi_v1_head_commit_timestamp,
                                            timestamp_str));

  g_ptr_array_add (txt_records, NULL);
  return generate_avahi_service_template_to_file (service_file,
                                                  "1",
                                                  (const gchar **)txt_records->pdata,
                                                  cancellable,
                                                  error);
}

/**
 * eos_avahi_service_file_get_directory:
 *
 * Get the path of the directory where Avahi will look for `.service` files
 * advertising DNS-SD services. The directory might not have a trailing slash.
 *
 * This may be overridden by specifying the
 * `EOS_UPDATER_TEST_UPDATER_AVAHI_SERVICES_DIR` environment variable. This is
 * intended for testing only.
 *
 * Returns: service file directory
 */
const gchar *
eos_avahi_service_file_get_directory (void)
{
  return eos_updater_get_envvar_or ("EOS_UPDATER_TEST_UPDATER_AVAHI_SERVICES_DIR",
                                    SYSCONFDIR "/avahi/services");
}

static GFile *
get_service_file (const gchar *avahi_service_directory)
{
  g_autofree gchar *service_file_path = NULL;

  service_file_path = g_build_filename (avahi_service_directory,
                                        "eos-updater.service", NULL);
  return g_file_new_for_path (service_file_path);
}

/**
 * eos_avahi_service_file_generate:
 * @avahi_service_directory: path to the directory containing `.service` files
 * @ostree_path: OSTree path of the commit to advertise
 * @head_commit_timestamp: (transfer none): timestamp of the commit to
 *    advertise
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Create a `.service` file in @avahi_service_directory for the updater. This
 * instructs Avahi to advertise a DNS-SD service for the updater, with TXT
 * records indicating this machine has the refs for @ostree_path available with
 * a commit at @head_commit_timestamp.
 *
 * The latest version of the DNS-SD record structure will be used, and a
 * version record will be added if appropriate.
 *
 * If the `.service` file already exists, it will be atomically replaced. If the
 * @avahi_service_directory does not exist, or is not writeable, an error will
 * be returned. If an error is returned, the old file will remain in place (if
 * it exists), unmodified.
 *
 * @ostree_path should have the same format as returned by
 * eos_updater_get_ostree_path().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean
eos_avahi_service_file_generate (const gchar *avahi_service_directory,
                                 const gchar *ostree_path,
                                 GDateTime *head_commit_timestamp,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autoptr(GFile) service_file = NULL;

  g_return_val_if_fail (avahi_service_directory != NULL, FALSE);
  g_return_val_if_fail (ostree_path != NULL, FALSE);
  g_return_val_if_fail (head_commit_timestamp != NULL, FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  service_file = get_service_file (avahi_service_directory);

  return generate_v1_service_file (ostree_path, head_commit_timestamp,
                                   service_file, cancellable, error);
}

/**
 * eos_avahi_service_file_delete:
 * @avahi_service_directory: path to the directory containing `.service` files
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Delete the updater’s `.service` file from the @avahi_service_directory. This
 * has the same semantics as g_file_delete(); except if no `.service` file
 * exists, or if @avahi_service_directory does not exist, %TRUE is returned.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean
eos_avahi_service_file_delete (const gchar   *avahi_service_directory,
                               GCancellable  *cancellable,
                               GError       **error)
{
  g_autoptr(GFile) service_file = NULL;
  g_autoptr(GError) local_error = NULL;

  g_return_val_if_fail (avahi_service_directory != NULL, FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  service_file = get_service_file (avahi_service_directory);

  if (!g_file_delete (service_file, cancellable, &local_error) &&
      !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}
