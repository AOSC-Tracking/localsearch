/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008, Nokia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

#include <libtracker-common/tracker-common.h>
#include <libtracker-db/tracker-db.h>
#include <libtracker-miner/tracker-miner-manager.h>
#include <libtracker-miner/tracker-crawler.h>

static gboolean     should_kill;
static gboolean     should_terminate;
static gboolean     hard_reset;
static gboolean     soft_reset;
static gboolean     remove_config;
static gboolean     remove_thumbnails;
static gboolean     start;
static gboolean     print_version;

static GOptionEntry entries[] = {
	{ "kill", 'k', 0, G_OPTION_ARG_NONE, &should_kill,
	  N_("Use SIGKILL to stop all tracker processes found - guarantees death :)"),
	  NULL },
	{ "terminate", 't', 0, G_OPTION_ARG_NONE, &should_terminate,
	  N_("Use SIGTERM to stop all tracker processes found"),
	  NULL 
	},
	{ "hard-reset", 'r', 0, G_OPTION_ARG_NONE, &hard_reset,
	  N_("Kill all Tracker processes and remove all databases"),
	  NULL },
	{ "soft-reset", 'e', 0, G_OPTION_ARG_NONE, &soft_reset,
	  N_("Same as --hard-reset but the backup & journal are restored after restart"),
	  NULL },
	{ "remove-config", 'c', 0, G_OPTION_ARG_NONE, &remove_config,
	  N_("Remove all configuration files so they are re-generated on next start"),
	  NULL },
	{ "remove-thumbnails", 'h', 0, G_OPTION_ARG_NONE, &remove_thumbnails,
	  N_("Remove all thumbnail files so they are re-generated"),
	  NULL },
	{ "start", 's', 0, G_OPTION_ARG_NONE, &start,
	  N_("Starts miners (which indirectly starts tracker-store too)"),
	  NULL },
	{ "version", 'V', 0, G_OPTION_ARG_NONE, &print_version,
	  N_("Print version"),
	  NULL },
	{ NULL }
};

static GSList *
get_pids (void)
{
	GError *error = NULL;
	GDir *dir;
	GSList *pids = NULL;
	const gchar *name;

	dir = g_dir_open ("/proc", 0, &error);
	if (error) {
		g_printerr ("%s, %s\n",
			    _("Could not open /proc"),
			    error ? error->message : _("no error given"));
		g_clear_error (&error);
		return NULL;
	}

	while ((name = g_dir_read_name (dir)) != NULL) { 
		gchar c;
		gboolean is_pid = TRUE;

		for (c = *name; c && c != ':' && is_pid; c++) {		
			is_pid &= g_ascii_isdigit (c);
		}

		if (!is_pid) {
			continue;
		}

		pids = g_slist_prepend (pids, g_strdup (name));
	}

	g_dir_close (dir);

	return g_slist_reverse (pids);
}

static void
log_handler (const gchar    *domain,
	     GLogLevelFlags  log_level,
	     const gchar    *message,
	     gpointer	     user_data)
{
	switch (log_level) {
	case G_LOG_LEVEL_WARNING:
	case G_LOG_LEVEL_CRITICAL:
	case G_LOG_LEVEL_ERROR:
	case G_LOG_FLAG_RECURSION:
	case G_LOG_FLAG_FATAL:
		g_fprintf (stderr, "%s\n", message);
		fflush (stderr);
		break;
	case G_LOG_LEVEL_MESSAGE:
	case G_LOG_LEVEL_INFO:
	case G_LOG_LEVEL_DEBUG:
	case G_LOG_LEVEL_MASK:
	default:
		g_fprintf (stdout, "%s\n", message);
		fflush (stdout);
		break;
	}	
}

static gboolean
crawler_check_file_cb (TrackerCrawler *crawler,
		       GFile          *file,
		       gpointer        user_data)
{
	const gchar **suffix;
	gchar *path;
	gboolean should_remove;

	suffix = user_data;
	path = g_file_get_path (file);

	if (suffix) {
		should_remove = g_str_has_suffix (path, *suffix);
	} else {
		should_remove = TRUE;
	}

	if (!should_remove) {
		g_free (path);
		return FALSE;
	}

	/* Remove file */
	if (g_unlink (path) == 0) {
		g_print ("  %s\n", path);
	}

	g_free (path);

	return should_remove;
}

static void
crawler_finished_cb (TrackerCrawler *crawler,
		     GQueue         *found,
		     gboolean        was_interrupted,
		     guint           directories_found,
		     guint           directories_ignored,
		     guint           files_found,
		     guint           files_ignored,
		     gpointer        user_data)
{
	g_main_loop_quit (user_data);
}

int
main (int argc, char **argv)
{
	GOptionContext *context;
	GError *error = NULL;
	GSList *pids;
	GSList *l;
	gchar  *str;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	g_type_init ();
	
	if (!g_thread_supported ()) {
		g_thread_init (NULL);
	}

	/* Translators: this messagge will apper immediately after the	*/
	/* usage string - Usage: COMMAND [OPTION]... <THIS_MESSAGE>	*/
	context = g_option_context_new (_(" - Manage Tracker processes and data"));
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	if (print_version) {
		g_print ("%s\n", PACKAGE_STRING);
		return EXIT_SUCCESS;
	}

	if (should_kill && should_terminate) {
		g_printerr ("%s\n",
			    _("You can not use the --kill and --terminate arguments together"));
		return EXIT_FAILURE;
	} else if ((hard_reset || soft_reset) && should_terminate) {
		g_printerr ("%s\n",
			    _("You can not use the --terminate with --hard-reset or --soft-reset, --kill is implied"));
		return EXIT_FAILURE;
	} else if (hard_reset && soft_reset) {
		g_printerr ("%s\n",
			    _("You can not use the --hard-reset and --soft-reset arguments together"));
		return EXIT_FAILURE;
	}

	if (hard_reset || soft_reset) {
		/* Imply --kill */
		should_kill = TRUE;
	}

	/* Unless we are stopping processes or listing processes,
	 * don't iterate them.
	 */
	if (should_kill || should_terminate ||
	    (!start && !remove_config && !remove_thumbnails)) {
		pids = get_pids ();
		str = g_strdup_printf (tracker_dngettext (NULL,
							  "Found %d PID…", 
							  "Found %d PIDs…",
							  g_slist_length (pids)),
				       g_slist_length (pids));
		g_print ("%s\n", str);
		g_free (str);
		
		for (l = pids; l; l = l->next) {
			gchar *filename;
			gchar *contents = NULL;
			gchar **strv;
			
			filename = g_build_filename ("/proc", l->data, "cmdline", NULL);
			if (!g_file_get_contents (filename, &contents, NULL, &error)) {
				str = g_strdup_printf (_("Could not open '%s'"), filename);
				g_printerr ("%s, %s\n", 
					    str,
					    error ? error->message : _("no error given"));
				g_free (str);
				g_clear_error (&error);
				g_free (contents);
				g_free (filename);
				
				continue;
			}
			
			strv = g_strsplit (contents, "^@", 2);
			if (strv && strv[0]) {
				gchar *basename;
				
				basename = g_path_get_basename (strv[0]);
				if (g_str_has_prefix (basename, "tracker") == TRUE &&
				    g_str_has_suffix (basename, "-control") == FALSE) {
					pid_t pid;
					
					pid = atoi (l->data);
					str = g_strdup_printf (_("Found process ID %d for '%s'"), pid, basename);
					g_print ("%s\n", str);
					g_free (str);
					
					if (should_terminate) {
						if (kill (pid, SIGTERM) == -1) {
							const gchar *errstr = g_strerror (errno);
							
							str = g_strdup_printf (_("Could not terminate process %d"), pid);
							g_printerr ("  %s, %s\n", 
								    str,
								    errstr ? errstr : _("no error given"));
							g_free (str);
						} else {
							str = g_strdup_printf (_("Terminated process %d"), pid);
							g_print ("  %s\n", str);
							g_free (str);
						}
					} else if (should_kill) {
						if (kill (pid, SIGKILL) == -1) {
							const gchar *errstr = g_strerror (errno);
							
							str = g_strdup_printf (_("Could not kill process %d"), pid);
							g_printerr ("  %s, %s\n", 
								    str,
								    errstr ? errstr : _("no error given"));
							g_free (str);
						} else {
							str = g_strdup_printf (_("Killed process %d"), pid);
							g_print ("  %s\n", str);
							g_free (str);
						}
					}
				}
				
				g_free (basename);
			}
			
			g_strfreev (strv);
			g_free (contents);
			g_free (filename);
		}

		g_slist_foreach (pids, (GFunc) g_free, NULL);
		g_slist_free (pids);
	}

	if (hard_reset || soft_reset) {
		guint log_handler_id;

		/* Set log handler for library messages */
		log_handler_id = g_log_set_handler (NULL,
						    G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL,
						    log_handler,
						    NULL);
		
		g_log_set_default_handler (log_handler, NULL);

		/* Clean up */
		if (!tracker_db_manager_init (TRACKER_DB_MANAGER_REMOVE_ALL, NULL, FALSE, NULL)) {
			return EXIT_FAILURE;
		}

		tracker_db_manager_remove_all (hard_reset);
		tracker_db_manager_shutdown ();

		/* Unset log handler */
		g_log_remove_handler (NULL, log_handler_id);
	}

	if (remove_config) {
		GMainLoop *main_loop;
		GFile *file;
		TrackerCrawler *crawler;
		const gchar *suffix = ".cfg";
		const gchar *home_dir;
		gchar *path;
		
		crawler = tracker_crawler_new ();
		main_loop = g_main_loop_new (NULL, FALSE);
		
		g_signal_connect (crawler, "check-file",
				  G_CALLBACK (crawler_check_file_cb),
				  &suffix);
		g_signal_connect (crawler, "finished",
				  G_CALLBACK (crawler_finished_cb),
				  main_loop);

		/* Go through service files */
		home_dir = g_getenv ("HOME");

		if (!home_dir) {
			home_dir = g_get_home_dir ();
		}

		path = g_build_path (G_DIR_SEPARATOR_S, home_dir, ".config", "tracker", NULL);
		file = g_file_new_for_path (path);
		g_free (path);

		g_print ("%s\n", _("Removing configuration files…"));

		tracker_crawler_start (crawler, file, FALSE);
		g_object_unref (file);
		
		g_main_loop_run (main_loop);
		g_object_unref (crawler);
	}

	if (remove_thumbnails) {
		GMainLoop *main_loop;
		GFile *file;
		TrackerCrawler *crawler;
		const gchar *home_dir;
		gchar *path;
		
		crawler = tracker_crawler_new ();
		main_loop = g_main_loop_new (NULL, FALSE);
		
		g_signal_connect (crawler, "check-file",
				  G_CALLBACK (crawler_check_file_cb),
				  NULL);
		g_signal_connect (crawler, "finished",
				  G_CALLBACK (crawler_finished_cb),
				  main_loop);

		/* Go through service files */
		home_dir = g_getenv ("HOME");

		if (!home_dir) {
			home_dir = g_get_home_dir ();
		}

		path = g_build_path (G_DIR_SEPARATOR_S, home_dir, ".thumbnails", NULL);
		file = g_file_new_for_path (path);
		g_free (path);

		g_print ("%s\n", _("Removing thumbnails files…"));

		tracker_crawler_start (crawler, file, TRUE);
		g_object_unref (file);
		
		g_main_loop_run (main_loop);
		g_object_unref (crawler);
	}

	if (start) {
		TrackerMinerManager *manager;
		GSList *miners, *l;

		manager = tracker_miner_manager_new ();
		miners = tracker_miner_manager_get_available (manager);
		
		g_print ("%s\n", _("Starting miners…"));

		/* Get the status of all miners, this will start all
		 * miners not already running.
		 */

		for (l = miners; l; l = l->next) {
			const gchar *display_name;
			gdouble progress = 0.0;

			display_name = tracker_miner_manager_get_display_name (manager, l->data);

			if (!tracker_miner_manager_get_status (manager, l->data, NULL, &progress)) {
				g_printerr ("  %s: %s (%s)\n", 
					    _("Failed"),
					    display_name,
					    _("Could not get miner status"));
			} else {
				g_print ("  %s: %s (%3.0f%%)\n", 
					 _("Done"),
					 display_name, 
					 progress * 100);
			}

			g_free (l->data);
		}

		g_slist_free (miners);
	}

	return EXIT_SUCCESS;
}
