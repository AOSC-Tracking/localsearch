/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <glib/gi18n.h>

#if defined(HAVE_GSTREAMER)
#include <gst/gst.h>
#endif

#include <libtracker-miners-common/tracker-common.h>

#include "tracker-config.h"
#include "tracker-controller.h"
#include "tracker-miner-files.h"
#include "tracker-files-interface.h"

#include <tinysparql.h>

#define ABOUT	  \
	"Tracker " PACKAGE_VERSION "\n"

#define LICENSE	  \
	"This program is free software and comes without any warranty.\n" \
	"It is licensed under version 2 or later of the General Public " \
	"License which can be viewed at:\n" \
	"\n" \
	"  http://www.gnu.org/licenses/gpl.txt\n"

#define SECONDS_PER_DAY 60 * 60 * 24

#define DBUS_NAME_SUFFIX "LocalSearch3"
#define LEGACY_DBUS_NAME_SUFFIX "Tracker3.Miner.Files"
#define DBUS_PATH "/org/freedesktop/Tracker3/Miner/Files"

#define LAST_CRAWL_FILENAME "last-crawl.txt"

static GMainLoop *main_loop;
static guint cleanup_id;

static gint initial_sleep = -1;
static gboolean no_daemon;
static gchar *eligible;
static gboolean version;
static guint miners_timeout_id = 0;
static gboolean do_crawling = FALSE;
static gchar *domain_ontology_name = NULL;
static gboolean dry_run = FALSE;

static gboolean slept = TRUE;
static gboolean graphs_ready = FALSE;
static gboolean corrupted = FALSE;

static GOptionEntry entries[] = {
	{ "initial-sleep", 's', 0,
	  G_OPTION_ARG_INT, &initial_sleep,
	  N_("Initial sleep time in seconds, "
	     "0->1000 (default=15)"),
	  NULL },
	{ "no-daemon", 'n', 0,
	  G_OPTION_ARG_NONE, &no_daemon,
	  N_("Runs until all configured locations are indexed and then exits"),
	  NULL },
	{ "eligible", 'e', 0,
	  G_OPTION_ARG_FILENAME, &eligible,
	  N_("Checks if FILE is eligible for being mined based on configuration"),
	  N_("FILE") },
	{ "domain-ontology", 'd', 0,
	  G_OPTION_ARG_STRING, &domain_ontology_name,
	  N_("Runs for a specific domain ontology"),
	  NULL },
	{ "dry-run", 'r', 0,
	  G_OPTION_ARG_NONE, &dry_run,
	  N_("Avoids changes in the filesystem"),
	  NULL },
	{ "version", 'V', 0,
	  G_OPTION_ARG_NONE, &version,
	  N_("Displays version information"),
	  NULL },
	{ NULL }
};

typedef struct {
	TrackerSparqlConnection *sparql_conn;
	GDBusConnection *dbus_conn;
	GMainLoop *main_loop;
	GMutex mutex;
	GCond cond;
	GThread *thread;
	gboolean initialized;
	GError *error;
} EndpointThreadData;

static EndpointThreadData *endpoint_thread_data = NULL;

static void
log_option_values (TrackerConfig *config)
{
#ifdef G_ENABLE_DEBUG
	if (TRACKER_DEBUG_CHECK (CONFIG)) {
		g_message ("General options:");
		g_message ("  Initial Sleep  ........................  %d",
		           tracker_config_get_initial_sleep (config));

		g_message ("Indexer options:");
		g_message ("  Throttle level  .......................  %d",
		           tracker_config_get_throttle (config));
		g_message ("  Indexing while on battery  ............  %s (first time only = %s)",
		           tracker_config_get_index_on_battery (config) ? "yes" : "no",
		           tracker_config_get_index_on_battery_first_time (config) ? "yes" : "no");

		if (tracker_config_get_low_disk_space_limit (config) == -1) {
			g_message ("  Low disk space limit  .................  Disabled");
		} else {
			g_message ("  Low disk space limit  .................  %d%%",
			           tracker_config_get_low_disk_space_limit (config));
		}
	}
#endif
}

static GFile *
get_cache_dir (TrackerDomainOntology *domain_ontology)
{
	GFile *cache;

	cache = tracker_domain_ontology_get_cache (domain_ontology);
	return g_file_get_child (cache, "files");
}

static gchar *
get_last_crawl_filename (TrackerMinerFiles *mf)
{
	g_autoptr (GFile) file = NULL, child = NULL;
	TrackerDomainOntology *domain_ontology;

	g_object_get (G_OBJECT (mf), "domain-ontology", &domain_ontology, NULL);
	file = get_cache_dir (domain_ontology);
	child = g_file_get_child (file, LAST_CRAWL_FILENAME);
	tracker_domain_ontology_unref (domain_ontology);

	return g_file_get_path (child);
}

guint64
get_last_crawl_done (TrackerMinerFiles *mf)
{
	g_autofree gchar *filename = NULL, *content = NULL;
	guint64 then;

	filename = get_last_crawl_filename (mf);

	if (!g_file_get_contents (filename, &content, NULL, NULL)) {
		g_info ("  No previous timestamp, crawling forced");
		return 0;
	}

	then = g_ascii_strtoull (content, NULL, 10);

	return then;
}

void
set_last_crawl_done (TrackerMinerFiles *mf,
                     gboolean           done)
{
	g_autofree gchar *filename = NULL;

	filename = get_last_crawl_filename (mf);

	if (done) {
		g_autoptr (GError) error = NULL;
		g_autofree gchar *content = NULL;

		content = g_strdup_printf ("%" G_GUINT64_FORMAT, (guint64) time (NULL));
		g_info ("  Updating last crawl file:'%s'", filename);
		/* Create/update time stamp file */
		if (!g_file_set_contents (filename, content, -1, &error)) {
			g_warning ("  Could not create/overwrite file:'%s' failed, %s",
			           filename,
			           error->message);
		} else {
			g_info ("  Last crawl file:'%s' updated", filename);
		}
	} else {
		g_info ("  Crawl not done yet, doesn't update last crawl file.");
	}
}

static gboolean
signal_handler (gpointer user_data)
{
	int signo = GPOINTER_TO_INT (user_data);

	static gboolean in_loop = FALSE;

	/* Die if we get re-entrant signals handler calls */
	if (in_loop) {
		_exit (EXIT_FAILURE);
	}

	switch (signo) {
	case SIGTERM:
	case SIGINT:
		in_loop = TRUE;
		g_main_loop_quit (main_loop);

		/* Fall through */
	default:
		if (g_strsignal (signo)) {
			g_debug ("Received signal:%d->'%s'",
			         signo,
			         g_strsignal (signo));
		}
		break;
	}

	return G_SOURCE_CONTINUE;
}

static void
initialize_signal_handler (void)
{
#ifndef G_OS_WIN32
	g_unix_signal_add (SIGTERM, signal_handler, GINT_TO_POINTER (SIGTERM));
	g_unix_signal_add (SIGINT, signal_handler, GINT_TO_POINTER (SIGINT));
#endif /* G_OS_WIN32 */
}

static void
initialize_priority_and_scheduling (void)
{
	/* Set CPU priority */
	tracker_sched_idle ();

	/* Set disk IO priority and scheduling */
	tracker_ioprio_init ();

	/* Set process priority:
	 * The nice() function uses attribute "warn_unused_result" and
	 * so complains if we do not check its returned value. But it
	 * seems that since glibc 2.2.4, nice() can return -1 on a
	 * successful call so we have to check value of errno too.
	 * Stupid...
	 */

	TRACKER_NOTE (CONFIG, g_message ("Setting priority nice level to 19"));

	errno = 0;
	if (nice (19) == -1 && errno != 0) {
		const gchar *str = g_strerror (errno);

		g_message ("Couldn't set nice value to 19, %s",
		           str ? str : "no error given");
	}
}

static void
raise_file_descriptor_limit (void)
{
	struct rlimit rl;

	if (getrlimit (RLIMIT_NOFILE, &rl) != 0)
		return;

	rl.rlim_cur = rl.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
		g_warning ("Failed to increase file descriptor limit: %m");
}

static gboolean
should_crawl (TrackerMinerFiles *miner_files,
              TrackerConfig     *config)
{
	gint crawling_interval;

	crawling_interval = tracker_config_get_crawling_interval (config);

	TRACKER_NOTE (CONFIG, g_message ("Checking whether to crawl file system based on configured crawling interval:"));

	if (crawling_interval == -2) {
		TRACKER_NOTE (CONFIG, g_message ("  Disabled"));
		return FALSE;
	} else if (crawling_interval == -1) {
		TRACKER_NOTE (CONFIG, g_message ("  Maybe (depends on a clean last shutdown)"));
		return TRUE;
	} else if (crawling_interval == 0) {
		TRACKER_NOTE (CONFIG, g_message ("  Forced"));
		return TRUE;
	} else {
		guint64 then, now;

		then = get_last_crawl_done (miner_files);

		if (then < 1) {
			return TRUE;
		}

		now = (guint64) time (NULL);

		if (now < then + (crawling_interval * SECONDS_PER_DAY)) {
			TRACKER_NOTE (CONFIG, g_message ("  Postponed"));
			return FALSE;
		} else {
			TRACKER_NOTE (CONFIG, g_message ("  (More than) %d days after last crawling, enabled", crawling_interval));
			return TRUE;
		}
	}
}

static void
miner_do_start (TrackerMiner *miner)
{
	if (!tracker_miner_is_started (miner)) {
		g_debug ("Starting filesystem miner...");
		tracker_miner_start (miner);
	}
}

static void
miner_maybe_start (TrackerMiner *miner)
{
	if (!slept || !graphs_ready)
		return;

	miner_do_start (miner);
}

static gboolean
miner_start_idle_cb (gpointer data)
{
	TrackerMiner *miner = data;

	miners_timeout_id = 0;
	slept = TRUE;
	miner_maybe_start (miner);
	return G_SOURCE_REMOVE;
}

static void
miner_start (TrackerMiner  *miner,
             TrackerConfig *config)
{
	gint initial_sleep;

	/* If requesting to run as no-daemon, start right away */
	if (no_daemon) {
		miner_maybe_start (miner);
		return;
	}

	/* If no need to initially sleep, start right away */
	initial_sleep = tracker_config_get_initial_sleep (config);

	if (initial_sleep <= 0) {
		miner_maybe_start (miner);
		return;
	}

	slept = FALSE;
	g_debug ("Performing initial sleep of %d seconds",
	         initial_sleep);
	miners_timeout_id = g_timeout_add_seconds (initial_sleep,
	                                           miner_start_idle_cb,
	                                           miner);
}

static void
release_heap_memory (void)
{
#ifdef HAVE_MALLOC_TRIM
	malloc_trim (0);
#else
	g_debug ("release_heap_memory(): Doing nothing as malloc_trim() is not available on this platform.");
#endif
}

static gboolean
cleanup_cb (gpointer user_data)
{
	release_heap_memory ();

	cleanup_id = 0;

	return G_SOURCE_REMOVE;
}

static void
start_cleanup_timeout (void)
{
	if (cleanup_id == 0)
		cleanup_id = g_timeout_add_seconds (30, cleanup_cb, NULL);
}

static void
stop_cleanup_timeout (void)
{
	g_clear_handle_id (&cleanup_id, g_source_remove);
}

#if GLIB_CHECK_VERSION (2, 64, 0)
static void
on_low_memory (GMemoryMonitor            *monitor,
               GMemoryMonitorWarningLevel level,
               gpointer                   user_data)
{
	if (level > G_MEMORY_MONITOR_WARNING_LEVEL_LOW)
		release_heap_memory ();
}
#endif

static void
miner_started_cb (TrackerMinerFS *fs)
{
	stop_cleanup_timeout ();
}

static void
miner_finished_cb (TrackerMinerFS *fs,
                   gdouble         seconds_elapsed,
                   guint           total_directories_found,
                   guint           total_directories_ignored,
                   guint           total_files_found,
                   guint           total_files_ignored,
                   guint           changes,
                   gpointer        user_data)
{
	g_info ("Finished mining in %f seconds. Changes processed: %d, inspected directories:%d, inspected files:%d",
	        seconds_elapsed,
	        changes,
	        total_directories_found,
	        total_files_found);

	if (do_crawling && !dry_run) {
		set_last_crawl_done (TRACKER_MINER_FILES (fs), TRUE);
	}

	/* We're not sticking around for file updates, so stop
	 * the mainloop and exit.
	 */
	if (no_daemon && main_loop) {
		/* FIXME: wait for extractor to finish */
		g_main_loop_quit (main_loop);
	}
}

static void
miner_status_cb (TrackerMinerFS *fs)
{
	g_autofree gchar *status = NULL;

	g_object_get (G_OBJECT (fs), "status", &status, NULL);
	if (g_strcmp0 (status, "Idle") == 0)
		start_cleanup_timeout ();
	else
		stop_cleanup_timeout ();
}

static void
miner_corrupt_cb (TrackerMinerFS *fs)
{
	g_warning ("Database corruption detected, bailing out");
	corrupted = TRUE;
	g_main_loop_quit (main_loop);
}

static void
graphs_created_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
	TrackerMiner *miner = user_data;

	tracker_sparql_connection_update_finish (TRACKER_SPARQL_CONNECTION (source),
	                                         res, NULL);
	graphs_ready = TRUE;
	miner_maybe_start (miner);
}

static gint
check_eligible (TrackerIndexingTree *indexing_tree,
                TrackerStorage      *storage)
{
	TrackerController *controller;
	GFile *file;
	GFileInfo *info;
	GError *error = NULL;
	gchar *path;
	gboolean exists = TRUE;
	gboolean indexable;
	gboolean parents_indexable = TRUE;
	gboolean is_dir;

	controller = tracker_controller_new (indexing_tree, storage, NULL);

	/* Start check */
	file = g_file_new_for_commandline_arg (eligible);
	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
	                          G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL,
	                          &error);

	if (error) {
		if (error->code == G_IO_ERROR_NOT_FOUND) {
			exists = FALSE;
		}

		g_error_free (error);
	}

	if (info) {
		is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
	} else {
		/* Assume not a dir */
		is_dir = FALSE;
	}

	path = g_file_get_path (file);

	g_print (exists ?
	         _("Data object “%s” currently exists") :
	         _("Data object “%s” currently does not exist"),
	         path);

	g_print ("\n");

	indexable = tracker_indexing_tree_file_is_indexable (indexing_tree, file, info);

	if (!indexable) {
		if (is_dir &&
		    tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_DIRECTORY, file)) {
			g_print ("  %s\n", _("Directory is NOT eligible to be indexed (based on filters)"));

		} else if (!is_dir &&
		           tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_FILE, file)) {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (based on filters)"));
		} else if (tracker_file_is_hidden (file) &&
		           tracker_indexing_tree_get_filter_hidden (indexing_tree)) {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (hidden file)"));
		} else {
			g_print ("  %s\n", _("File is NOT eligible to be indexed (not an indexed folder)"));
		}
	}

	if (indexable) {
		GFile *root, *parent;
		GList *files = NULL, *l;

		root = tracker_indexing_tree_get_root (indexing_tree, file, NULL);
		parent = file;

		/* Still, a parent folder might be filtered out, figure it out */
		while (parent && !g_file_equal (parent, root)) {
			parent = g_file_get_parent (parent);
			files = g_list_prepend (files, parent);
		}

		for (l = files; l; l = l->next) {
			gchar *dir_path;

			dir_path = g_file_get_path (l->data);

			if (is_dir &&
			    tracker_indexing_tree_file_matches_filter (indexing_tree, TRACKER_FILTER_DIRECTORY, l->data)) {
				g_print (_("Parent directory “%s” is NOT eligible to be indexed (based on filters)"),
				         dir_path);
				g_print ("\n");
				parents_indexable = FALSE;
			} else if (tracker_file_is_hidden (l->data) &&
			           tracker_indexing_tree_get_filter_hidden (indexing_tree)) {
				g_print (_("Parent directory “%s” is NOT eligible to be indexed (hidden file)"),
				         dir_path);
				g_print ("\n");
				parents_indexable = FALSE;
			} else {
				if (!tracker_indexing_tree_parent_is_indexable (indexing_tree, l->data)) {
					g_print (_("Parent directory “%s” is NOT eligible to be indexed (based on content filters)"),
					         dir_path);
					g_print ("\n");
					parents_indexable = FALSE;
				}
			}

			g_free (dir_path);

			if (!parents_indexable)
				break;
		}

		g_list_free_full (files, g_object_unref);
	}

	if (indexable && parents_indexable) {
		g_print ("  %s\n",
		         is_dir ?
		         _("Directory is eligible to be indexed") :
		         _("File is eligible to be indexed"));
	}

	g_free (path);
	g_object_unref (file);
	g_object_unref (info);
	g_object_unref (controller);

	return (indexable && parents_indexable) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void
on_domain_vanished (GDBusConnection *connection,
                    const gchar     *name,
                    gpointer         user_data)
{
	GMainLoop *loop = user_data;
	g_message ("Domain %s vanished: quitting now.", name);
	g_main_loop_quit (loop);
}

TrackerSparqlConnectionFlags
get_fts_connection_flags (void)
{
	TrackerSparqlConnectionFlags flags = 0;
	TrackerFTSConfig *fts_config;

	fts_config = tracker_fts_config_new ();

	if (tracker_fts_config_get_enable_stemmer (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_STEMMER;
	if (tracker_fts_config_get_enable_unaccent (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_ENABLE_UNACCENT;
	if (tracker_fts_config_get_ignore_numbers (fts_config))
		flags |= TRACKER_SPARQL_CONNECTION_FLAGS_FTS_IGNORE_NUMBERS;

	g_object_unref (fts_config);

	return flags;
}

gpointer
endpoint_thread_func (gpointer user_data)
{
	EndpointThreadData *data = user_data;
	TrackerEndpointDBus *endpoint;
	GMainContext *main_context;

	main_context = g_main_context_new ();
	g_main_context_push_thread_default (main_context);

	endpoint = tracker_endpoint_dbus_new (data->sparql_conn,
	                                      data->dbus_conn,
	                                      NULL,
	                                      NULL,
	                                      &data->error);

	data->main_loop = g_main_loop_new (main_context, FALSE);

	g_mutex_lock (&data->mutex);
	data->initialized = TRUE;
	g_cond_signal (&data->cond);
	g_mutex_unlock (&data->mutex);

	if (!data->error)
		g_main_loop_run (data->main_loop);

	g_main_context_pop_thread_default (main_context);
	g_main_loop_unref (data->main_loop);
	g_main_context_unref (main_context);
	g_clear_object (&endpoint);

	return NULL;
}

gboolean
start_endpoint_thread (TrackerSparqlConnection  *conn,
                       GDBusConnection          *dbus_conn,
                       GError                  **error)
{
	g_assert (endpoint_thread_data == NULL);

	endpoint_thread_data = g_new0 (EndpointThreadData, 1);
	g_mutex_init (&endpoint_thread_data->mutex);
	g_cond_init (&endpoint_thread_data->cond);
	endpoint_thread_data->sparql_conn = conn;
	endpoint_thread_data->dbus_conn = dbus_conn;

	endpoint_thread_data->thread =
		g_thread_try_new ("SPARQL endpoint",
		                  endpoint_thread_func,
		                  endpoint_thread_data,
		                  error);

	if (!endpoint_thread_data->thread)
		return FALSE;

	g_mutex_lock (&endpoint_thread_data->mutex);

	while (!endpoint_thread_data->initialized) {
		g_cond_wait (&endpoint_thread_data->cond,
		             &endpoint_thread_data->mutex);
	}

	g_mutex_unlock (&endpoint_thread_data->mutex);

	if (endpoint_thread_data->error) {
		g_propagate_error (error, endpoint_thread_data->error);
		g_thread_join (endpoint_thread_data->thread);
		g_clear_pointer (&endpoint_thread_data, g_free);
		return FALSE;
	}

	return TRUE;
}

void
finish_endpoint_thread (void)
{
	g_assert (endpoint_thread_data != NULL);

	g_main_loop_quit (endpoint_thread_data->main_loop);

	g_thread_join (endpoint_thread_data->thread);

	g_clear_pointer (&endpoint_thread_data, g_free);
}

static TrackerSparqlConnection *
setup_connection (TrackerDomainOntology  *domain,
                  GError                **error)
{
	TrackerSparqlConnection *sparql_conn;
	g_autoptr (GFile) store = NULL, ontology = NULL;
	GError *internal_error = NULL;

	if (!dry_run)
		store = get_cache_dir (domain);
	ontology = tracker_sparql_get_ontology_nepomuk ();

	sparql_conn = tracker_sparql_connection_new (get_fts_connection_flags (),
	                                             store,
	                                             ontology,
	                                             NULL,
	                                             &internal_error);

	if (store && g_error_matches (internal_error, TRACKER_SPARQL_ERROR, TRACKER_SPARQL_ERROR_CORRUPT)) {
		g_autoptr (GFile) backup_location = NULL, parent = NULL;
		g_autofree gchar *filename = NULL;

		/* Move the database directory away, for possible forensics */
		parent = g_file_get_parent (store);
		filename = g_strdup_printf ("files.%" G_GINT64_FORMAT, g_get_monotonic_time ());
		backup_location = g_file_get_child (parent, filename);

		if (g_file_move (store, backup_location,
		                 G_FILE_COPY_NONE,
		                 NULL, NULL, NULL, NULL)) {
			g_autofree gchar *path = NULL;

			path = g_file_get_path (backup_location);
			g_message ("Database is corrupt, it is now backed up at %s. Reindexing from scratch", path);
			sparql_conn = tracker_sparql_connection_new (get_fts_connection_flags (),
			                                             store,
			                                             ontology,
			                                             NULL,
			                                             &internal_error);
		}
	}

	if (internal_error)
		g_propagate_error (error, internal_error);

	return sparql_conn;
}

int
main (gint argc, gchar *argv[])
{
	TrackerConfig *config;
	TrackerMiner *miner_files;
	GOptionContext *context;
	GError *error = NULL;
	TrackerMinerProxy *proxy;
	GDBusConnection *connection;
	TrackerSparqlConnection *sparql_conn;
	TrackerIndexingTree *indexing_tree;
	TrackerStorage *storage;
	TrackerDomainOntology *domain_ontology;
	TrackerController *controller;
#if GLIB_CHECK_VERSION (2, 64, 0)
	GMemoryMonitor *memory_monitor;
#endif
	gchar *domain_name, *dbus_name;
	TrackerFilesInterface *files_interface;
	gboolean initial_index = TRUE;

	main_loop = NULL;

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Set timezone info */
	tzset ();

	/* This makes sure we don't steal all the system's resources */
	initialize_priority_and_scheduling ();

	/* This makes it harder to run out of file descriptors while there
	 * are many concurrently running queries through the endpoint.
	 */
	raise_file_descriptor_limit ();

	/* Preempt possible registry updates, before tracker-extract-3 deals
	 * with gstreamer plugins.
	 */
#if defined(HAVE_GSTREAMER)
	gst_init (NULL, NULL);
#endif

	/* Translators: this messagge will apper immediately after the
	 * usage string - Usage: COMMAND <THIS_MESSAGE>
	 */
	context = g_option_context_new (_("— start the tracker indexer"));

	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_parse (context, &argc, &argv, &error);
	g_option_context_free (context);

	if (error) {
		g_printerr ("%s\n", error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	if (version) {
		g_print ("\n" ABOUT "\n" LICENSE "\n");
		return EXIT_SUCCESS;
	}

	indexing_tree = tracker_indexing_tree_new ();
	storage = tracker_storage_new ();

	if (eligible) {
		return check_eligible (indexing_tree, storage);
	}

	domain_ontology = tracker_domain_ontology_new (domain_ontology_name, NULL, &error);
	if (error) {
		g_critical ("Could not load domain ontology '%s': %s",
		            domain_ontology_name, error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	connection = g_bus_get_sync (TRACKER_IPC_BUS, NULL, &error);
	if (error) {
		g_critical ("Could not create DBus connection: %s\n",
		            error->message);
		g_error_free (error);
		return EXIT_FAILURE;
	}

	files_interface = tracker_files_interface_new (connection);

	/* Initialize logging */
	config = tracker_config_new ();

	if (initial_sleep > -1) {
		tracker_config_set_initial_sleep (config, initial_sleep);
	}

	log_option_values (config);

	main_loop = g_main_loop_new (NULL, FALSE);

	initialize_signal_handler ();

	if (no_daemon) {
		g_debug ("tracker-miner-fs-3 running in --no-daemon mode.");
	} else if (domain_ontology_name) {
		domain_name = tracker_domain_ontology_get_domain (domain_ontology, NULL);

		g_debug ("tracker-miner-fs-3 running in --domain-ontology mode as "
		         "%s." DBUS_NAME_SUFFIX ". The service will exit when %s "
		         "disappears from the bus.", domain_name, domain_name);

		g_bus_watch_name_on_connection (connection, domain_name,
		                                G_BUS_NAME_WATCHER_FLAGS_NONE,
		                                NULL, on_domain_vanished,
		                                main_loop, NULL);
		g_free (domain_name);
	} else {
		g_debug ("tracker-miner-fs-3 running as org.freedesktop." DBUS_NAME_SUFFIX);
	}

	if (!dry_run) {
		GFile *store = get_cache_dir (domain_ontology);

		initial_index = !g_file_query_exists (store, NULL);
		tracker_error_report_init (store);
		g_object_unref (store);
	}

	sparql_conn = setup_connection (domain_ontology, &error);
	if (!sparql_conn) {

		g_critical ("Could not create store: %s",
		            error->message);
		g_error_free (error);

		return EXIT_FAILURE;
	}

	if (!start_endpoint_thread (sparql_conn, connection, &error)) {
		g_critical ("Could not set up SPARQL endpoint: %s",
		            error->message);
		g_error_free (error);

		return EXIT_FAILURE;
	}

	/* Create new TrackerMinerFiles object */
	miner_files = tracker_miner_files_new (sparql_conn,
	                                       indexing_tree,
	                                       storage,
	                                       config,
	                                       domain_ontology,
	                                       initial_index);

	controller = tracker_controller_new (indexing_tree, storage, files_interface);

	proxy = tracker_miner_proxy_new (miner_files, connection, DBUS_PATH, NULL, &error);
	if (error) {
		g_critical ("Couldn't create miner proxy: %s", error->message);
		g_error_free (error);
		g_object_unref (config);
		g_object_unref (miner_files);
		return EXIT_FAILURE;
	}

	/* Request DBus names */
	dbus_name = tracker_domain_ontology_get_domain (domain_ontology, DBUS_NAME_SUFFIX);

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		g_free (dbus_name);
		return EXIT_FAILURE;
	}

	dbus_name = tracker_domain_ontology_get_domain (domain_ontology, LEGACY_DBUS_NAME_SUFFIX);

	if (!tracker_dbus_request_name (connection, dbus_name, &error)) {
		g_critical ("Could not request legacy DBus name '%s': %s",
		            dbus_name, error->message);
		g_error_free (error);
		g_free (dbus_name);
		return EXIT_FAILURE;
	}

	g_free (dbus_name);

	/* Check if we should crawl and if we should force mtime
	 * checking based on the config.
	 */
	do_crawling = should_crawl (TRACKER_MINER_FILES (miner_files), config);

	g_signal_connect (miner_files, "started",
			  G_CALLBACK (miner_started_cb),
			  NULL);
	g_signal_connect (miner_files, "finished",
			  G_CALLBACK (miner_finished_cb),
			  NULL);
	g_signal_connect (miner_files, "notify::status",
	                  G_CALLBACK (miner_status_cb),
	                  NULL);
	g_signal_connect (miner_files, "corrupt",
			  G_CALLBACK (miner_corrupt_cb),
			  NULL);

#if GLIB_CHECK_VERSION (2, 64, 0)
	memory_monitor = g_memory_monitor_dup_default ();
	g_signal_connect (memory_monitor, "low-memory-warning", G_CALLBACK (on_low_memory), NULL);
#endif

	/* Preempt creation of graphs */
	tracker_sparql_connection_update_async (tracker_miner_get_connection (miner_files),
	                                        "CREATE SILENT GRAPH tracker:FileSystem; "
	                                        "CREATE SILENT GRAPH tracker:Software; "
	                                        "CREATE SILENT GRAPH tracker:Documents; "
	                                        "CREATE SILENT GRAPH tracker:Pictures; "
	                                        "CREATE SILENT GRAPH tracker:Audio; "
	                                        "CREATE SILENT GRAPH tracker:Video ",
	                                        NULL, graphs_created_cb, miner_files);

	if (do_crawling)
		miner_start (miner_files, config);

	/* Go, go, go! */
	g_main_loop_run (main_loop);

	g_debug ("Shutdown started");

	finish_endpoint_thread ();

	g_object_unref (files_interface);

	g_main_loop_unref (main_loop);
	g_object_unref (config);

	g_object_unref (controller);

	g_object_unref (miner_files);

	g_object_unref (proxy);
	g_object_unref (connection);
	tracker_domain_ontology_unref (domain_ontology);

	tracker_sparql_connection_close (sparql_conn);
	g_object_unref (sparql_conn);

	g_clear_object (&indexing_tree);
	g_clear_object (&storage);

#if GLIB_CHECK_VERSION (2, 64, 0)
	g_signal_handlers_disconnect_by_func (memory_monitor, on_low_memory, NULL);
	g_object_unref (memory_monitor);
#endif

	return corrupted ? EXIT_FAILURE : EXIT_SUCCESS;
}
