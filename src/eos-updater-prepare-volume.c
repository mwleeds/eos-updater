/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright © 2016 Kinvolk GmbH
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
 */

#include <glib.h>
#include <locale.h>
#include <ostree.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "eos-prepare-usb-update.h"

/* main() exit codes. */
enum
{
  EXIT_OK = EXIT_SUCCESS,
  EXIT_FAILED = 1,
  EXIT_INVALID_ARGUMENTS = 2,
  EXIT_RUN_AS_ROOT = 3,
};

static int
fail (gboolean     quiet,
      gint         exit_status,
      const gchar *error_message,
      ...) G_GNUC_PRINTF (3, 4);

static int
fail (gboolean     quiet,
      gint         exit_status,
      const gchar *error_message,
      ...)
{
  va_list ap;
  g_autofree gchar *formatted_message = NULL;

  g_return_val_if_fail (exit_status > 0, EXIT_FAILED);

  if (quiet)
    return exit_status;

  /* Format the arguments. */
  va_start (ap, error_message);
  formatted_message = g_strdup_vprintf (error_message, ap);
  va_end (ap);

  /* Include the usage. */
  g_printerr ("%s: %s\n", g_get_prgname (), formatted_message);

  return exit_status;
}

static int
usage (GOptionContext *context,
       gboolean        quiet,
       const gchar    *error_message,
       ...) G_GNUC_PRINTF (3, 4);

static int
usage (GOptionContext *context,
       gboolean        quiet,
       const gchar    *error_message,
       ...)
{
  va_list ap;
  g_autofree gchar *formatted_message = NULL;
  g_autofree gchar *help = NULL;

  if (quiet)
    return EXIT_INVALID_ARGUMENTS;

  /* Format the arguments. */
  va_start (ap, error_message);
  formatted_message = g_strdup_vprintf (error_message, ap);
  va_end (ap);

  /* Include the usage. */
  help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s: %s\n\n%s\n", g_get_prgname (), formatted_message, help);

  return EXIT_INVALID_ARGUMENTS;
}

int
main (int argc,
      char **argv)
{
  gboolean quiet = FALSE;
  g_auto(GStrv) remaining = NULL;
  g_autoptr(GOptionContext) context = g_option_context_new ("— Endless OS Updater USB Drive Preparation Tool");
  GOptionEntry entries[] =
    {
      { "quiet", 'q', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &quiet, "Do not print anything; check exit status for success", NULL },
      { G_OPTION_REMAINING, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME_ARRAY,
        &remaining, "Path to the USB drive to prepare", "VOLUME-PATH" },
      { NULL }
    };
  g_autoptr(GError) error = NULL;
  const gchar *raw_usb_path = NULL;
  g_autoptr(GFile) usb_path = NULL;
  g_autoptr(OstreeSysroot) sysroot = NULL;
  g_autoptr(OstreeAsyncProgress) progress = NULL;
  gboolean lock_acquired = FALSE;

  setlocale (LC_ALL, "");

  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_set_summary (context,
                                "Prepare a USB drive with a copy of the local "
                                "OSTree repository, so it can be used to "
                                "update other machines offline. The repository "
                                "copy will be put in the eos-update directory "
                                "on the USB drive; other files will not be "
                                "affected.");

  if (!g_option_context_parse (context,
                               &argc,
                               &argv,
                               &error))
    {
      return usage (context, quiet, "Failed to parse options: %s",
                    error->message);
    }

  /* We need to be root in order to read all the files in the OSTree repo
   * (unless we’re running the unit tests). */
  if (geteuid () != 0 &&
      g_getenv ("EOS_UPDATER_TEST_UPDATER_DEPLOYMENT_FALLBACK") == NULL)
    {
      return fail (quiet, EXIT_RUN_AS_ROOT, "Must be run as root");
    }

  if (argc != 1 || remaining == NULL || g_strv_length (remaining) != 1)
    {
      return usage (context, quiet,
                    "Expected exactly one path to the USB drive");
    }

  raw_usb_path = remaining[0];
  usb_path = g_file_new_for_commandline_arg (raw_usb_path);
  if (!g_file_query_exists (usb_path, NULL))
    {
      return fail (quiet, EXIT_INVALID_ARGUMENTS, "Path ‘%s’ does not exist",
                   raw_usb_path);
    }

  sysroot = ostree_sysroot_new_default ();

  /* Lock the sysroot so it can’t be updated while we’re pulling from it. The
   * lock is automatically released when we finalise the sysroot. */
  ostree_sysroot_try_lock (sysroot, &lock_acquired, NULL);

  if (!lock_acquired)
    {
      if (!quiet)
        g_print ("Waiting for lock on sysroot…\n");

      if (!ostree_sysroot_lock (sysroot, &error))
        return fail (quiet, EXIT_FAILED, "Failed to lock sysroot: %s",
                     error->message);
    }

  if (!ostree_sysroot_load (sysroot, NULL, &error))
    {
      return fail (quiet, EXIT_FAILED, "Failed to load sysroot: %s",
                   error->message);
    }

  if (!quiet)
    progress = ostree_async_progress_new_and_connect (ostree_repo_pull_default_console_progress_changed,
                                                      /* just whatever that is not NULL, the function
                                                       * above early-quits if user-data is NULL
                                                       */
                                                      main);

  if (!eos_updater_prepare_volume_from_sysroot (sysroot,
                                                usb_path,
                                                progress,
                                                NULL,
                                                &error))
    {
      return fail (quiet, EXIT_FAILED, "Failed to prepare the update: %s",
                   error->message);
    }

  return EXIT_OK;
}
