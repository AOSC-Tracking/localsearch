/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * Author: Carlos Garnacho  <carlos@lanedo.com>
 */

#include "config-miners.h"

#include <libtracker-miners-common/tracker-common.h>
#include <libtracker-extract/tracker-extract.h>
#include <libtracker-sparql/tracker-sparql.h>

#include "tracker-file-notifier.h"
#include "tracker-monitor-glib.h"
#include "tracker-utils.h"

enum {
	PROP_0,
	PROP_INDEXING_TREE,
	PROP_CONNECTION,
	PROP_FILE_ATTRIBUTES,
};

enum {
	FILE_CREATED,
	FILE_UPDATED,
	FILE_DELETED,
	FILE_MOVED,
	DIRECTORY_STARTED,
	DIRECTORY_FINISHED,
	FINISHED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	FILE_STATE_NONE,
	FILE_STATE_CREATE,
	FILE_STATE_UPDATE,
	FILE_STATE_DELETE,
	FILE_STATE_EXTRACTOR_UPDATE,
};

typedef struct {
	GList *node;
	GFile *file;
	guint in_disk : 1;
	guint in_store : 1;
	guint is_dir_in_disk : 1;
	guint is_dir_in_store : 1;
	guint state : 3;
	GDateTime *store_mtime;
	GDateTime *disk_mtime;
	gchar *extractor_hash;
	gchar *mimetype;
} TrackerFileData;

typedef struct {
	TrackerFileNotifier *notifier;
	TrackerSparqlCursor *cursor;
	GFile *root;
	GFileEnumerator *enumerator;
	GCancellable *cancellable;
	GHashTable *cache;
	GQueue queue;
	GQueue deleted_dirs;
	GFile *current_dir;
	GQueue *pending_dirs;
	GTimer *timer;
	guint flags;
	guint cursor_idle_id;
	guint directories_found;
	guint directories_ignored;
	guint files_found;
	guint files_ignored;
	guint ignore_root : 1;
	guint cursor_has_content : 1;
} TrackerIndexRoot;

typedef struct {
	TrackerIndexingTree *indexing_tree;

	TrackerSparqlConnection *connection;
	GCancellable *cancellable;

	TrackerMonitor *monitor;

	TrackerSparqlStatement *content_query;
	TrackerSparqlStatement *deleted_query;

	gchar *file_attributes;

	/* List of pending directory
	 * trees to get data from
	 */
	GList *pending_index_roots;
	TrackerIndexRoot *current_index_root;

	guint stopped : 1;
	guint high_water : 1;
	guint active : 1;
} TrackerFileNotifierPrivate;

#define N_CURSOR_BATCH_ITEMS 200
#define N_ENUMERATOR_BATCH_ITEMS 200

static gboolean tracker_index_root_query_contents (TrackerIndexRoot *root);
static gboolean tracker_index_root_crawl_next (TrackerIndexRoot *root);
static gboolean tracker_index_root_continue_cursor (TrackerIndexRoot *root);
static void tracker_index_root_continue (TrackerIndexRoot *root);

static TrackerSparqlStatement * sparql_contents_ensure_statement (TrackerFileNotifier  *notifier,
                                                                  GError              **error);

G_DEFINE_TYPE_WITH_PRIVATE (TrackerFileNotifier, tracker_file_notifier, G_TYPE_OBJECT)

static void
tracker_file_notifier_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		priv->indexing_tree = g_value_dup_object (value);
		break;
	case PROP_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	case PROP_FILE_ATTRIBUTES:
		priv->file_attributes = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tracker_file_notifier_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));

	switch (prop_id) {
	case PROP_INDEXING_TREE:
		g_value_set_object (value, priv->indexing_tree);
		break;
	case PROP_CONNECTION:
		g_value_set_object (value, priv->connection);
		break;
	case PROP_FILE_ATTRIBUTES:
		g_value_set_string (value, priv->file_attributes);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
file_data_free (TrackerFileData *file_data)
{
	g_object_unref (file_data->file);
	g_clear_pointer (&file_data->store_mtime, g_date_time_unref);
	g_clear_pointer (&file_data->disk_mtime, g_date_time_unref);
	g_free (file_data->extractor_hash);
	g_free (file_data->mimetype);
	g_slice_free (TrackerFileData, file_data);
}

static TrackerIndexRoot *
tracker_index_root_new (TrackerFileNotifier *notifier,
                        GFile               *file,
                        guint                flags,
                        gboolean             ignore_root)
{
	TrackerIndexRoot *data;

	data = g_new0 (TrackerIndexRoot, 1);
	data->notifier = notifier;
	data->root = g_object_ref (file);
	data->pending_dirs = g_queue_new ();
	data->flags = flags;
	data->ignore_root = ignore_root;
	data->timer = g_timer_new ();

	g_queue_init (&data->deleted_dirs);
	g_queue_init (&data->queue);
	data->cache = g_hash_table_new_full (g_file_hash,
	                                     (GEqualFunc) g_file_equal,
	                                     NULL,
	                                     (GDestroyNotify) file_data_free);

	return data;
}

static void
tracker_index_root_free (TrackerIndexRoot *data)
{
	g_queue_free_full (data->pending_dirs, (GDestroyNotify) g_object_unref);
	g_timer_destroy (data->timer);
	g_queue_clear (&data->queue);
	g_queue_clear_full (&data->deleted_dirs, g_object_unref);
	g_hash_table_destroy (data->cache);
	g_clear_object (&data->enumerator);
	g_clear_object (&data->current_dir);
	g_clear_object (&data->cursor);
	g_clear_handle_id (&data->cursor_idle_id, g_source_remove);
	g_clear_object (&data->cancellable);
	g_object_unref (data->root);
	g_free (data);
}

static gboolean
check_file (TrackerFileNotifier *notifier,
            GFile               *file,
            GFileInfo           *info)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	return tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                file, info);
}

static gint
index_root_equals_file (TrackerIndexRoot *root,
                        GFile            *file)
{
	if (g_file_equal (root->root, file))
		return 0;

	return -1;
}

static gboolean
check_directory (TrackerFileNotifier *notifier,
                 GFile               *directory,
                 GFileInfo           *info)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);
	g_assert (priv->current_index_root != NULL);

	/* If it's a config root itself, other than the one
	 * currently processed, bypass it, it will be processed
	 * when the time arrives.
	 */
	if (tracker_indexing_tree_file_is_root (priv->indexing_tree, directory) &&
	    index_root_equals_file (priv->current_index_root, directory) != 0)
		return FALSE;

	return tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                directory, info);
}

static gboolean
check_directory_contents (TrackerFileNotifier *notifier,
                          GFile               *parent)
{
	TrackerFileNotifierPrivate *priv;
	gboolean process = TRUE;

	priv = tracker_file_notifier_get_instance_private (notifier);

	/* Do not let content filter apply to configured roots themselves. This
	 * is a measure to trim undesired portions of the filesystem, and if
	 * the folder is configured to be indexed, it's clearly not undesired.
	 */
	if (!tracker_indexing_tree_file_is_root (priv->indexing_tree, parent)) {
		process = tracker_indexing_tree_parent_is_indexable (priv->indexing_tree,
		                                                     parent);
	}

	if (!process)
		tracker_monitor_remove (priv->monitor, parent);

	return process;
}

static gboolean
tracker_file_notifier_notify (TrackerFileNotifier *notifier,
                              TrackerFileData     *file_data,
                              GFileInfo           *info)
{
	GFile *file = file_data->file;
	gboolean stop = FALSE;

	if (file_data->state == FILE_STATE_DELETE) {
		/* In store but not in disk, delete */
		g_signal_emit (notifier, signals[FILE_DELETED], 0, file,
		               file_data->is_dir_in_store);
		stop = TRUE;
	} else if (file_data->state == FILE_STATE_CREATE) {
		/* In disk but not in store, create */
		g_signal_emit (notifier, signals[FILE_CREATED], 0, file, info);
	} else if (file_data->state == FILE_STATE_UPDATE ||
	           file_data->state == FILE_STATE_EXTRACTOR_UPDATE) {
		/* File changed, update */
		g_signal_emit (notifier, signals[FILE_UPDATED], 0, file, info, FALSE);
	}

	return stop;
}

static gboolean
notifier_check_next_root (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->stopped)
		return FALSE;

	if (!sparql_contents_ensure_statement (notifier, NULL))
		return FALSE;

	g_clear_pointer (&priv->current_index_root, tracker_index_root_free);

	while (priv->pending_index_roots) {
		priv->current_index_root = priv->pending_index_roots->data;
		priv->pending_index_roots =
			g_list_delete_link (priv->pending_index_roots,
			                    priv->pending_index_roots);

		if (tracker_index_root_query_contents (priv->current_index_root))
			return TRUE;

		g_clear_pointer (&priv->current_index_root, tracker_index_root_free);
	}

	g_signal_emit (notifier, signals[FINISHED], 0);
	return FALSE;
}

static void
tracker_index_root_notify_changes (TrackerIndexRoot *root)
{
	TrackerFileData *data;

	while ((data = g_queue_pop_tail (&root->queue)) != NULL) {
		tracker_file_notifier_notify (root->notifier, data, NULL);
		g_hash_table_remove (root->cache, data->file);
	}
}

static void
update_state (TrackerFileData *data)
{
	data->state = FILE_STATE_NONE;

	if (data->in_disk) {
		if (data->in_store) {
			if (!g_date_time_equal (data->store_mtime, data->disk_mtime)) {
				data->state = FILE_STATE_UPDATE;
			} else if (data->mimetype) {
				const gchar *current_hash;

				current_hash = tracker_extract_module_manager_get_hash (data->mimetype);

				if (g_strcmp0 (data->extractor_hash, current_hash) != 0) {
					data->state = FILE_STATE_EXTRACTOR_UPDATE;
				}
			}
		} else {
			data->state = FILE_STATE_CREATE;
		}
	} else {
		if (data->in_store) {
			data->state = FILE_STATE_DELETE;
		}
	}
}

static TrackerFileData *
ensure_file_data (TrackerIndexRoot *root,
                  GFile            *file)
{
	TrackerFileData *file_data;

	file_data = g_hash_table_lookup (root->cache, file);
	if (!file_data) {
		file_data = g_slice_new0 (TrackerFileData);
		file_data->file = g_object_ref (file);
		g_hash_table_insert (root->cache, file_data->file, file_data);
		file_data->node = g_list_alloc ();
		file_data->node->data = file_data;
		g_queue_push_head_link (&root->queue, file_data->node);
	}

	return file_data;
}

static TrackerFileData *
_insert_disk_info (TrackerIndexRoot *root,
                   GFile            *file,
                   GFileType         file_type,
                   GDateTime        *datetime)
{
	TrackerFileData *file_data;

	file_data = ensure_file_data (root, file);
	file_data->in_disk = TRUE;
	file_data->is_dir_in_disk = file_type == G_FILE_TYPE_DIRECTORY;
	g_clear_pointer (&file_data->disk_mtime, g_date_time_unref);
	file_data->disk_mtime = g_date_time_ref (datetime);
	update_state (file_data);

	return file_data;
}

static TrackerFileData *
_insert_store_info (TrackerIndexRoot *root,
                    GFile            *file,
                    GFileType         file_type,
                    const gchar      *extractor_hash,
                    const gchar      *mimetype,
                    GDateTime        *datetime)
{
	TrackerFileData *file_data;

	file_data = ensure_file_data (root, file);
	file_data->in_store = TRUE;
	file_data->is_dir_in_store = file_type == G_FILE_TYPE_DIRECTORY;
	file_data->extractor_hash = g_strdup (extractor_hash);
	file_data->mimetype = g_strdup (mimetype);
	file_data->store_mtime = g_date_time_ref (datetime);
	update_state (file_data);

	return file_data;
}

static gboolean
check_high_water (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->high_water) {
		priv->active = FALSE;
		return TRUE;
	}

	return FALSE;
}

static void
handle_file_from_filesystem (TrackerIndexRoot *root,
                             GFile            *file,
                             GFileInfo        *info)
{
	TrackerFileData *file_data;
	GFileType file_type;
	g_autoptr (GDateTime) datetime = NULL;

	file_type = g_file_info_get_file_type (info);
	datetime = g_file_info_get_modification_date_time (info);
	file_data = _insert_disk_info (root,
	                               file,
	                               file_type,
	                               datetime);

	if (file_type == G_FILE_TYPE_DIRECTORY &&
	    file_data->state == FILE_STATE_CREATE &&
	    (root->flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0 &&
	    !g_file_equal (file, root->current_dir) &&
	    check_directory_contents (root->notifier, file) &&
	    !g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT)) {
		/* Queue child dirs for later processing */
		g_queue_push_tail (root->pending_dirs, g_object_ref (file));
	}

	tracker_file_notifier_notify (root->notifier, file_data, info);
	g_queue_delete_link (&root->queue, file_data->node);
	g_hash_table_remove (root->cache, file);
}

static void
enumerator_next_files_cb (GObject      *object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
	TrackerIndexRoot *root;
	g_autoptr (GError) error = NULL;
	GList *infos, *l;
	int n_files = 0;

	infos = g_file_enumerator_next_files_finish (G_FILE_ENUMERATOR (object), res, &error);

	if (error) {
		g_autofree gchar *uri = NULL;

		/* Caller already commanded the way to continue */
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;

		uri = g_file_get_uri (g_file_enumerator_get_container (G_FILE_ENUMERATOR (object)));
		g_warning ("Got error crawling '%s': %s\n",
		           uri, error->message);

		tracker_index_root_continue (user_data);
		return;
	} else if (!infos) {
		/* Directory contents were fully obtained */
		tracker_index_root_continue (user_data);
		return;
	}

	root = user_data;

	for (l = infos; l; l = l->next) {
		GFileInfo *info = l->data;
		GFileType file_type;
		g_autoptr (GFile) file = NULL;

		file = g_file_enumerator_get_child (G_FILE_ENUMERATOR (object), info);
		file_type = g_file_info_get_file_type (info);
		n_files++;

		if (file_type == G_FILE_TYPE_DIRECTORY) {
			root->directories_found++;

			if (!check_directory (root->notifier, file, info)) {
				root->directories_ignored++;
				continue;
			}
		} else {
			root->files_found++;

			if (!check_file (root->notifier, file, info)) {
				root->files_ignored++;
				continue;
			}
		}

		handle_file_from_filesystem (root, file, info);
	}

	g_list_free_full (infos, g_object_unref);

	if (n_files == N_ENUMERATOR_BATCH_ITEMS) {
		g_file_enumerator_next_files_async (root->enumerator,
		                                    N_ENUMERATOR_BATCH_ITEMS,
		                                    G_PRIORITY_DEFAULT,
		                                    root->cancellable,
		                                    enumerator_next_files_cb,
		                                    root);
	} else {
		tracker_index_root_continue (user_data);
	}
}

static void
enumerate_children_cb (GObject      *object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
	TrackerIndexRoot *root;
	g_autoptr (GFileEnumerator) enumerator = NULL;
	g_autoptr (GError) error = NULL;

	enumerator = g_file_enumerate_children_finish (G_FILE (object), res, &error);

	if (!enumerator) {
		/* Caller already commanded the way to continue */
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;

		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
			g_autofree gchar *uri = NULL;

			uri = g_file_get_uri (G_FILE (object));
			g_warning ("Got error crawling '%s': %s\n",
			           uri, error->message);
		}

		tracker_index_root_continue (user_data);
		return;
	}

	root = user_data;
	g_set_object (&root->enumerator, enumerator);
	g_file_enumerator_next_files_async (root->enumerator,
	                                    N_ENUMERATOR_BATCH_ITEMS,
	                                    G_PRIORITY_DEFAULT,
	                                    root->cancellable,
	                                    enumerator_next_files_cb,
	                                    root);
}

static void
query_root_info_cb (GObject      *object,
                    GAsyncResult *res,
                    gpointer      user_data)
{
	TrackerIndexRoot *root;
	TrackerFileNotifier *notifier;
	TrackerFileNotifierPrivate *priv;
	g_autoptr (GFileInfo) info = NULL;
	g_autoptr (GError) error = NULL;

	info = g_file_query_info_finish (G_FILE (object), res, &error);
	if (error) {
		/* Caller already commanded the way to continue */
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return;

		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
			gchar *uri;

			uri = g_file_get_uri (G_FILE (object));
			g_warning ("Got error querying root '%s': %s\n",
			           uri, error->message);
			g_free (uri);
		}

		tracker_index_root_continue (user_data);
		return;
	}

	root = user_data;
	notifier = root->notifier;
	priv = tracker_file_notifier_get_instance_private (notifier);

	handle_file_from_filesystem (root, G_FILE (object), info);

	g_file_enumerate_children_async (G_FILE (object),
	                                 priv->file_attributes,
	                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                                 G_PRIORITY_DEFAULT,
	                                 root->cancellable,
	                                 enumerate_children_cb,
	                                 root);
}

static gboolean
tracker_index_root_crawl_next (TrackerIndexRoot *root)
{
	TrackerFileNotifier *notifier;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	g_autoptr (GFile) directory = NULL;

	notifier = root->notifier;
	priv = tracker_file_notifier_get_instance_private (notifier);

	if (check_high_water (root->notifier))
		return TRUE;

	if (g_queue_is_empty (root->pending_dirs))
		return FALSE;

	directory = g_queue_pop_head (root->pending_dirs);
	g_set_object (&root->current_dir, directory);

	tracker_indexing_tree_get_root (priv->indexing_tree,
	                                directory, &flags);

	if ((flags & TRACKER_DIRECTORY_FLAG_MONITOR) != 0)
		tracker_monitor_add (priv->monitor, directory);

	priv->active = TRUE;

	if (directory == root->root && !root->ignore_root) {
		g_file_query_info_async (directory,
		                         priv->file_attributes,
		                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                         G_PRIORITY_DEFAULT,
		                         root->cancellable,
		                         query_root_info_cb,
		                         root);
	} else {
		g_file_enumerate_children_async (directory,
		                                 priv->file_attributes,
		                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		                                 G_PRIORITY_DEFAULT,
		                                 root->cancellable,
		                                 enumerate_children_cb,
		                                 root);
	}

	return TRUE;
}

static void
tracker_file_notifier_emit_directory_finished (TrackerFileNotifier *notifier,
                                               TrackerIndexRoot    *root)
{
	g_signal_emit (notifier, signals[DIRECTORY_FINISHED], 0,
	               root->root,
	               root->directories_found,
	               root->directories_ignored,
	               root->files_found,
	               root->files_ignored);

	TRACKER_NOTE (STATISTICS,
	              g_message ("  Notified files after %2.2f seconds",
	                         g_timer_elapsed (root->timer, NULL)));
	TRACKER_NOTE (STATISTICS,
	              g_message ("  Found %d directories, ignored %d directories",
	                         root->directories_found,
	                         root->directories_ignored));
	TRACKER_NOTE (STATISTICS,
	              g_message ("  Found %d files, ignored %d files",
	                         root->files_found,
	                         root->files_ignored));
}

static void
tracker_index_root_continue (TrackerIndexRoot *root)
{
	if (tracker_index_root_continue_cursor (root))
		return;

	if (tracker_index_root_crawl_next (root))
		return;

	tracker_index_root_notify_changes (root);
	tracker_file_notifier_emit_directory_finished (root->notifier, root);
	notifier_check_next_root (root->notifier);
}

static void
tracker_index_root_remove_directory (TrackerIndexRoot *root,
                                     GFile            *directory)
{
	GList *l = root->pending_dirs->head, *next;
	GFile *file;

	while (l) {
		file = l->data;
		next = l->next;

		if (g_file_equal (file, directory) ||
		    g_file_has_prefix (file, directory)) {
			g_queue_remove (root->pending_dirs, file);
			g_object_unref (file);
		}

		l = next;
	}
}

static void
file_notifier_current_root_check_remove_directory (TrackerFileNotifier *notifier,
                                                   GFile               *file)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->current_index_root)
		tracker_index_root_remove_directory (priv->current_index_root, file);
}

static TrackerSparqlStatement *
sparql_contents_ensure_statement (TrackerFileNotifier  *notifier,
                                  GError              **error)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->content_query)
		return priv->content_query;

	priv->content_query =
		tracker_load_statement (priv->connection, "get-index-root-content.rq", error);
	return priv->content_query;
}

static TrackerSparqlStatement *
sparql_deleted_ensure_statement (TrackerFileNotifier  *notifier,
                                 GError              **error)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->deleted_query)
		return priv->deleted_query;

	priv->deleted_query =
		tracker_load_statement (priv->connection, "get-file-mimetype.rq", error);
	return priv->deleted_query;
}

static int
file_is_equal (gconstpointer a,
               gconstpointer b)
{
	return g_file_equal (G_FILE (a), G_FILE (b)) ? 0 : -1;
}

static int
file_is_equal_or_child (gconstpointer a,
                        gconstpointer b)
{
	GFile *deleted_file = G_FILE (a);
	GFile *file = G_FILE (b);

	return (g_file_equal (file, deleted_file) || g_file_has_parent (file, deleted_file)) ? 0 : -1;
}

static int
file_is_equal_or_descendant (gconstpointer a,
                             gconstpointer b)
{
	GFile *deleted_file = G_FILE (a);
	GFile *file = G_FILE (b);

	return (g_file_equal (file, deleted_file) || g_file_has_prefix (file, deleted_file)) ? 0 : -1;
}

static void
handle_file_from_cursor (TrackerIndexRoot    *root,
                         TrackerSparqlCursor *cursor)
{
	TrackerFileNotifier *notifier;
	TrackerFileNotifierPrivate *priv;
	const gchar *folder_urn, *uri;
	GFileType file_type;
	g_autoptr (GFile) file = NULL, parent = NULL;
	g_autoptr (GDateTime) store_mtime = NULL;
	g_autoptr (GFileInfo) info = NULL;
	TrackerFileData *file_data;

	notifier = root->notifier;
	priv = tracker_file_notifier_get_instance_private (notifier);
	uri = tracker_sparql_cursor_get_string (cursor, 0, NULL);
	file = g_file_new_for_uri (uri);

	/* If the file is contained in a deleted dir, skip it */
	if (g_queue_find_custom (&root->deleted_dirs, file,
	                         file_is_equal_or_descendant))
		return;

	/* Get stored info */
	folder_urn = tracker_sparql_cursor_get_string (cursor, 1, NULL);
	store_mtime = tracker_sparql_cursor_get_datetime (cursor, 2);
	file_type = folder_urn != NULL ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_UNKNOWN;

	file_data = _insert_store_info (root,
	                                file,
	                                file_type,
	                                tracker_sparql_cursor_get_string (cursor, 3, NULL),
	                                tracker_sparql_cursor_get_string (cursor, 4, NULL),
	                                store_mtime);

	/* Query fs info in place */
	info = g_file_query_info (file, priv->file_attributes,
	                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                          NULL, NULL);

	if (info) {
		g_autoptr (GDateTime) disk_mtime = NULL;
		GFileType file_type;

		file_type = g_file_info_get_file_type (info);
		disk_mtime = g_file_info_get_modification_date_time (info);

		_insert_disk_info (root,
		                   file,
		                   file_type,
		                   disk_mtime);
	}

	if (file_data->state == FILE_STATE_DELETE &&
	    (file_data->is_dir_in_store || file_data->is_dir_in_disk)) {
		/* Cache deleted dir, in order to skip children */
		g_queue_push_head (&root->deleted_dirs, g_object_ref (file));
	} else if (file_data->is_dir_in_disk &&
	           ((!!(root->flags & TRACKER_DIRECTORY_FLAG_RECURSE) &&
	             !g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_UNIX_IS_MOUNTPOINT)) ||
	            index_root_equals_file (root, file) == 0) &&
	           check_directory_contents (notifier, file)) {
		if (!!(root->flags & TRACKER_DIRECTORY_FLAG_MONITOR)) {
			/* Directory, needs monitoring */
			tracker_monitor_add (priv->monitor, file);
		}

		if ((file_data->state == FILE_STATE_CREATE ||
		     file_data->state == FILE_STATE_UPDATE) &&
		    !g_queue_find_custom (root->pending_dirs, file, file_is_equal)) {
			/* Updated directory, needs crawling */
			g_queue_push_head (root->pending_dirs, g_object_ref (file));
		}
	}

	parent = g_file_get_parent (file);

	/* Notify immediately of changes, unless the directory needs crawling.
	 * Deleted events and updates due to extractor changes can be emitted
	 * right away.
	 */
	if (file_data->state == FILE_STATE_DELETE ||
	    file_data->state == FILE_STATE_EXTRACTOR_UPDATE ||
	    !g_queue_find_custom (root->pending_dirs, parent,
	                          file_is_equal_or_child)) {
		tracker_file_notifier_notify (notifier, file_data, info);
		g_queue_delete_link (&root->queue, file_data->node);
		g_hash_table_remove (root->cache, file);
	}
}

static gboolean
handle_cursor (TrackerIndexRoot *root)
{
	TrackerSparqlCursor *cursor = root->cursor;
	GCancellable *cancellable = root->cancellable;
	g_autoptr (GError) error = NULL;
	gboolean finished = TRUE, stop = TRUE;
	int i;

	for (i = 0; i < N_CURSOR_BATCH_ITEMS; i++) {
		finished = !tracker_sparql_cursor_next (cursor, cancellable, &error);
		if (finished)
			break;

		handle_file_from_cursor (root, cursor);
		root->cursor_has_content = TRUE;
	}

	if (finished) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return G_SOURCE_REMOVE;

		if (error) {
			g_autofree gchar *uri = NULL;

			uri = g_file_get_uri (root->root);
			g_critical ("Error iterating cursor for indexed folder '%s': %s",
			            uri, error->message);
		} else if (!root->cursor_has_content) {
			/* Indexing from scratch, crawl root dir */
			g_queue_push_tail (root->pending_dirs, g_object_ref (root->root));
		}

		g_clear_object (&root->cursor);
	}

	stop = finished || check_high_water (root->notifier);

	if (stop) {
		root->cursor_idle_id = 0;
		tracker_index_root_continue (root);
	}

	return stop ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static gboolean
tracker_index_root_continue_cursor (TrackerIndexRoot *root)
{
	if (!root->cursor)
		return FALSE;

	if (check_high_water (root->notifier))
		return TRUE;

	if (root->cursor_idle_id == 0) {
		root->cursor_idle_id =
			g_idle_add ((GSourceFunc) handle_cursor, root);
	}

	return TRUE;
}

static void
query_execute_cb (TrackerSparqlStatement *statement,
                  GAsyncResult           *res,
                  TrackerIndexRoot       *root)
{
	g_autoptr (TrackerSparqlCursor) cursor = NULL;
	g_autoptr (GError) error = NULL;

	cursor = tracker_sparql_statement_execute_finish (statement, res, &error);

	if (!cursor) {
		if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_autofree gchar *uri = NULL;

			uri = g_file_get_uri (root->root);
			g_critical ("Could not query contents for indexed folder '%s': %s",
			            uri, error->message);

			tracker_file_notifier_emit_directory_finished (root->notifier,
			                                               root);
		}

		return;
	}

	root->cursor = g_steal_pointer (&cursor);
	tracker_index_root_continue_cursor (root);
}

static gboolean
tracker_index_root_query_contents (TrackerIndexRoot *root)
{
	TrackerFileNotifier *notifier = root->notifier;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	GFile *directory;
	g_autofree gchar *uri = NULL;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!root->cancellable)
		root->cancellable = g_cancellable_new ();
	g_set_object (&priv->cancellable, root->cancellable);

	directory = root->root;
	flags = root->flags;

	if ((flags & TRACKER_DIRECTORY_FLAG_IGNORE) != 0) {
		if ((flags & TRACKER_DIRECTORY_FLAG_PRESERVE) == 0) {
			g_signal_emit (notifier, signals[FILE_DELETED], 0, directory, TRUE);
		}

		return FALSE;
	}

	g_timer_reset (root->timer);
	g_signal_emit (notifier, signals[DIRECTORY_STARTED], 0, directory);

	uri = g_file_get_uri (directory);
	tracker_sparql_statement_bind_string (priv->content_query, "root", uri);

	priv->active = TRUE;

	tracker_sparql_statement_execute_async (priv->content_query,
	                                        root->cancellable,
	                                        (GAsyncReadyCallback) query_execute_cb,
	                                        root);
	return TRUE;
}

static void
notifier_queue_root (TrackerFileNotifier   *notifier,
                     GFile                 *file,
                     TrackerDirectoryFlags  flags,
                     gboolean               ignore_root)
{
	TrackerFileNotifierPrivate *priv;
	TrackerIndexRoot *root;

	priv = tracker_file_notifier_get_instance_private (notifier);

	root = tracker_index_root_new (notifier, file, flags, ignore_root);

	if (flags & TRACKER_DIRECTORY_FLAG_PRIORITY) {
		priv->pending_index_roots = g_list_prepend (priv->pending_index_roots, root);
	} else {
		priv->pending_index_roots = g_list_append (priv->pending_index_roots, root);
	}

	if (!priv->current_index_root)
		notifier_check_next_root (notifier);
}

static GFileInfo *
create_shallow_file_info (GFile    *file,
                          gboolean  is_directory)
{
	GFileInfo *file_info;
	gchar *basename;

	file_info = g_file_info_new ();
	g_file_info_set_file_type (file_info,
	                           is_directory ?
	                           G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR);
	basename = g_file_get_basename (file);
	g_file_info_set_is_hidden (file_info, basename[0] == '.');
	g_free (basename);

	return file_info;
}

/* Monitor signal handlers */
static void
monitor_item_created_cb (TrackerMonitor *monitor,
                         GFile          *file,
                         gboolean        is_directory,
                         gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	gboolean indexable;

	priv = tracker_file_notifier_get_instance_private (notifier);

	indexable = tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                     file, NULL);

	if (!is_directory) {
		gboolean parent_indexable;
		GFile *parent;

		parent = g_file_get_parent (file);

		if (parent) {
			parent_indexable = tracker_indexing_tree_parent_is_indexable (priv->indexing_tree,
			                                                              parent);

			if (!parent_indexable) {
				/* New file triggered a directory content
				 * filter, remove parent directory altogether
				 */
				g_signal_emit (notifier, signals[FILE_DELETED], 0, parent, TRUE);
				file_notifier_current_root_check_remove_directory (notifier, parent);

				tracker_monitor_remove_recursively (priv->monitor, parent);
				return;
			}

			g_object_unref (parent);
		}

		if (!indexable)
			return;
	} else {
		TrackerDirectoryFlags flags;

		if (!indexable)
			return;

		/* If config for the directory is recursive,
		 * Crawl new entire directory and add monitors
		 */
		tracker_indexing_tree_get_root (priv->indexing_tree,
		                                file, &flags);

		if (flags & TRACKER_DIRECTORY_FLAG_RECURSE) {
			notifier_queue_root (notifier, file, flags, TRUE);

			/* Fall though, we want ::file-created to be emitted
			 * ASAP so it is ensured to be processed before any
			 * possible monitor events we might get afterwards.
			 */
		}
	}

	g_signal_emit (notifier, signals[FILE_CREATED], 0, file, NULL);
}

static void
monitor_item_updated_cb (TrackerMonitor *monitor,
                         GFile          *file,
                         gboolean        is_directory,
                         gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                              file, NULL)) {
		/* File should not be indexed */
		return;
	}

	g_signal_emit (notifier, signals[FILE_UPDATED], 0, file, NULL, FALSE);
}

static void
monitor_item_attribute_updated_cb (TrackerMonitor *monitor,
                                   GFile          *file,
                                   gboolean        is_directory,
                                   gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                              file, NULL)) {
		/* File should not be indexed */
		return;
	}

	g_signal_emit (notifier, signals[FILE_UPDATED], 0, file, NULL, TRUE);
}

static void
monitor_item_deleted_cb (TrackerMonitor *monitor,
                         GFile          *file,
                         gboolean        is_directory,
                         gpointer        user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	/* Remove monitors if any */
	if (is_directory &&
	    tracker_indexing_tree_file_is_root (priv->indexing_tree, file)) {
		tracker_monitor_remove_children_recursively (priv->monitor,
		                                             file);
	} else if (is_directory) {
		tracker_monitor_remove_recursively (priv->monitor, file);
	}

	if (!is_directory) {
		TrackerSparqlStatement *stmt;
		TrackerSparqlCursor *cursor = NULL;
		const gchar *mimetype;
		gchar *uri;

		/* TrackerMonitor only knows about monitored folders,
		 * query the data if we don't know that much.
		 */
		stmt = sparql_deleted_ensure_statement (notifier, NULL);

		if (stmt) {
			uri = g_file_get_uri (file);
			tracker_sparql_statement_bind_string (stmt, "uri", uri);
			cursor = tracker_sparql_statement_execute (stmt, NULL, NULL);
			g_free (uri);
		}

		if (cursor && tracker_sparql_cursor_next (cursor, NULL, NULL)) {
			mimetype = tracker_sparql_cursor_get_string (cursor, 0, NULL);
			is_directory = g_strcmp0 (mimetype, "inode/directory") == 0;
		}

		g_clear_object (&cursor);
	}

	/* Note: We might theoretically do live handling of files triggering
	 * TRACKER_FILTER_PARENT_DIRECTORY filters (e.g. reindexing the full
	 * folder after the file was removed). This does not work in practice
	 * since directories affected by that filter do not have a monitor,
	 * but if it worked, this would be the place to handle this.
	 */

	if (!tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                              file, NULL)) {
		/* File was not indexed */
		return ;
	}

	g_signal_emit (notifier, signals[FILE_DELETED], 0, file, is_directory);

	file_notifier_current_root_check_remove_directory (notifier, file);
}

static gboolean
extension_changed (GFile *file1,
                   GFile *file2)
{
	gchar *basename1, *basename2;
	const gchar *ext1, *ext2;
	gboolean changed;

	basename1 = g_file_get_basename (file1);
	basename2 = g_file_get_basename (file2);

	ext1 = strrchr (basename1, '.');
	ext2 = strrchr (basename2, '.');

	changed = g_strcmp0 (ext1, ext2) != 0;

	g_free (basename1);
	g_free (basename2);

	return changed;
}

static void
monitor_item_moved_cb (TrackerMonitor *monitor,
                       GFile          *file,
                       GFile          *other_file,
                       gboolean        is_directory,
                       gboolean        is_source_monitored,
                       gpointer        user_data)
{
	TrackerFileNotifier *notifier;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;

	notifier = user_data;
	priv = tracker_file_notifier_get_instance_private (notifier);
	tracker_indexing_tree_get_root (priv->indexing_tree, other_file, &flags);

	if (!is_source_monitored) {
		if (is_directory) {
			/* Remove monitors if any */
			tracker_monitor_remove_recursively (priv->monitor, file);
			notifier_queue_root (notifier, other_file, flags, FALSE);
		}
		/* else, file, do nothing */
	} else {
		gboolean should_process, should_process_other;
		GFileInfo *file_info, *other_file_info;
		GFile *check_file;

		if (is_directory) {
			check_file = g_object_ref (file);
		} else {
			check_file = g_file_get_parent (file);
		}

		file_info = create_shallow_file_info (file, is_directory);
		other_file_info = create_shallow_file_info (other_file, is_directory);

		/* If the (parent) directory is in
		 * the filesystem, file is stored
		 */
		should_process = tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
		                                                          file, file_info);
		should_process_other = tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
		                                                                other_file, other_file_info);
		g_object_unref (check_file);
		g_object_unref (file_info);
		g_object_unref (other_file_info);

		/* Ref those so they are safe to use after signal emission */
		g_object_ref (file);
		g_object_ref (other_file);

		if (!should_process) {
			/* The source was not an indexable file, the destination
			 * could be though, it should be indexed as if new, then */

			/* Remove monitors if any */
			if (is_directory) {
				tracker_monitor_remove_recursively (priv->monitor,
				                                    file);
			}

			if (should_process_other) {
				gboolean dest_is_recursive;
				TrackerDirectoryFlags flags;

				tracker_indexing_tree_get_root (priv->indexing_tree, other_file, &flags);
				dest_is_recursive = (flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;

				/* Source file was not stored, check dest file as new */
				if (!is_directory || !dest_is_recursive) {
					g_signal_emit (notifier, signals[FILE_CREATED], 0, other_file, NULL);
				} else if (is_directory) {
					/* Crawl dest directory */
					notifier_queue_root (notifier, other_file, flags, FALSE);
				}
			}
			/* Else, do nothing else */
		} else if (!should_process_other) {
			/* Delete original location as it moves to be non indexable */
			if (is_directory) {
				tracker_monitor_remove_recursively (priv->monitor,
				                                    file);
			}

			g_signal_emit (notifier, signals[FILE_DELETED], 0, file, is_directory);
			file_notifier_current_root_check_remove_directory (notifier, file);
		} else {
			/* Handle move */
			if (is_directory) {
				gboolean dest_is_recursive, source_is_recursive;
				TrackerDirectoryFlags source_flags;

				tracker_monitor_move (priv->monitor,
				                      file, other_file);

				tracker_indexing_tree_get_root (priv->indexing_tree,
				                                file, &source_flags);
				source_is_recursive = (source_flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;
				dest_is_recursive = (flags & TRACKER_DIRECTORY_FLAG_RECURSE) != 0;

				if (source_is_recursive && !dest_is_recursive) {
					/* A directory is being moved from a
					 * recursive location to a non-recursive
					 * one, don't do anything here, and let
					 * TrackerMinerFS handle it, see item_move().
					 */
				} else if (!source_is_recursive && dest_is_recursive) {
					/* crawl the folder */
					notifier_queue_root (notifier, other_file, flags, TRUE);
				}
			}

			g_signal_emit (notifier, signals[FILE_MOVED], 0, file, other_file, is_directory);

			if (extension_changed (file, other_file))
				g_signal_emit (notifier, signals[FILE_UPDATED], 0, other_file, NULL, FALSE);
		}

		g_object_unref (other_file);
		g_object_unref (file);
	}
}

/* Indexing tree signal handlers */
static void
indexing_tree_directory_added (TrackerIndexingTree *indexing_tree,
                               GFile               *directory,
                               gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (indexing_tree, directory, &flags);
	notifier_queue_root (notifier, directory, flags, FALSE);
}

static void
indexing_tree_directory_updated (TrackerIndexingTree *indexing_tree,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerDirectoryFlags flags;

	tracker_indexing_tree_get_root (indexing_tree, directory, &flags);
	flags |= TRACKER_DIRECTORY_FLAG_CHECK_DELETED;
	notifier_queue_root (notifier, directory, flags, FALSE);
}

static void
indexing_tree_directory_removed (TrackerIndexingTree *indexing_tree,
                                 GFile               *directory,
                                 gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	GList *elem;

	priv = tracker_file_notifier_get_instance_private (notifier);

	/* Flags are still valid at the moment of deletion */
	tracker_indexing_tree_get_root (indexing_tree, directory, &flags);

	/* If the folder was being ignored, index/crawl it from scratch */
	if (flags & TRACKER_DIRECTORY_FLAG_IGNORE) {
		GFile *parent;

		parent = g_file_get_parent (directory);

		if (parent) {
			TrackerDirectoryFlags parent_flags;

			tracker_indexing_tree_get_root (indexing_tree,
			                                parent,
			                                &parent_flags);

			if (parent_flags & TRACKER_DIRECTORY_FLAG_RECURSE) {
				notifier_queue_root (notifier, directory, parent_flags, FALSE);
			} else if (tracker_indexing_tree_file_is_root (indexing_tree,
			                                               parent)) {
				g_signal_emit (notifier, signals[FILE_CREATED],
				               0, directory, NULL);
			}

			g_object_unref (parent);
		}
		return;
	}

	if ((flags & TRACKER_DIRECTORY_FLAG_PRESERVE) == 0) {
		/* Directory needs to be deleted from the store too */
		g_signal_emit (notifier, signals[FILE_DELETED], 0, directory, TRUE);
	}

	elem = g_list_find_custom (priv->pending_index_roots, directory,
	                           (GCompareFunc) index_root_equals_file);

	if (elem) {
		tracker_index_root_free (elem->data);
		priv->pending_index_roots =
			g_list_delete_link (priv->pending_index_roots, elem);
	}

	if (priv->current_index_root &&
	    index_root_equals_file (priv->current_index_root, directory) == 0) {
		/* Directory being currently processed */
		if (priv->cancellable)
			g_cancellable_cancel (priv->cancellable);
		tracker_file_notifier_emit_directory_finished (notifier,
		                                               priv->current_index_root);
		notifier_check_next_root (notifier);
	}

	/* Remove monitors if any */
	tracker_monitor_remove_recursively (priv->monitor, directory);
}

static void
indexing_tree_child_updated (TrackerIndexingTree *indexing_tree,
                             GFile               *root,
                             GFile               *child,
                             gpointer             user_data)
{
	TrackerFileNotifier *notifier = user_data;
	TrackerFileNotifierPrivate *priv;
	TrackerDirectoryFlags flags;
	g_autoptr (GFileInfo) child_info = NULL;
	GFileType child_type;

	priv = tracker_file_notifier_get_instance_private (notifier);

	child_info = g_file_query_info (child,
	                                priv->file_attributes,
	                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                                NULL, NULL);
	if (!child_info)
		return;

	child_type = g_file_info_get_file_type (child_info);
	tracker_indexing_tree_get_root (indexing_tree, child, &flags);

	if (child_type == G_FILE_TYPE_DIRECTORY &&
	    (flags & TRACKER_DIRECTORY_FLAG_RECURSE)) {
		flags |= TRACKER_DIRECTORY_FLAG_CHECK_DELETED;

		notifier_queue_root (notifier, child, flags, FALSE);
	} else if (tracker_indexing_tree_file_is_indexable (priv->indexing_tree,
	                                                    child, child_info)) {
		g_signal_emit (notifier, signals[FILE_UPDATED], 0,
		               child, child_info, FALSE);
	}
}

static void
tracker_file_notifier_finalize (GObject *object)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));

	g_free (priv->file_attributes);

	if (priv->indexing_tree) {
		g_object_unref (priv->indexing_tree);
	}

	if (priv->cancellable) {
		g_cancellable_cancel (priv->cancellable);
		g_object_unref (priv->cancellable);
	}

	g_clear_object (&priv->content_query);
	g_clear_object (&priv->deleted_query);

	g_signal_handlers_disconnect_by_data (priv->monitor, object);
	g_clear_object (&priv->monitor);

	g_clear_object (&priv->connection);

	g_clear_pointer (&priv->current_index_root, tracker_index_root_free);

	g_list_foreach (priv->pending_index_roots, (GFunc) tracker_index_root_free, NULL);
	g_list_free (priv->pending_index_roots);

	G_OBJECT_CLASS (tracker_file_notifier_parent_class)->finalize (object);
}

static void
tracker_file_notifier_constructed (GObject *object)
{
	TrackerFileNotifierPrivate *priv;

	G_OBJECT_CLASS (tracker_file_notifier_parent_class)->constructed (object);

	priv = tracker_file_notifier_get_instance_private (TRACKER_FILE_NOTIFIER (object));
	g_assert (priv->indexing_tree);

	g_signal_connect (priv->indexing_tree, "directory-added",
	                  G_CALLBACK (indexing_tree_directory_added), object);
	g_signal_connect (priv->indexing_tree, "directory-updated",
	                  G_CALLBACK (indexing_tree_directory_updated), object);
	g_signal_connect (priv->indexing_tree, "directory-removed",
	                  G_CALLBACK (indexing_tree_directory_removed), object);
	g_signal_connect (priv->indexing_tree, "child-updated",
	                  G_CALLBACK (indexing_tree_child_updated), object);
}

static void
tracker_file_notifier_class_init (TrackerFileNotifierClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_file_notifier_finalize;
	object_class->set_property = tracker_file_notifier_set_property;
	object_class->get_property = tracker_file_notifier_get_property;
	object_class->constructed = tracker_file_notifier_constructed;

	signals[FILE_CREATED] =
		g_signal_new ("file-created",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_created),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2, G_TYPE_FILE, G_TYPE_FILE_INFO);
	signals[FILE_UPDATED] =
		g_signal_new ("file-updated",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_updated),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              3, G_TYPE_FILE, G_TYPE_FILE_INFO, G_TYPE_BOOLEAN);
	signals[FILE_DELETED] =
		g_signal_new ("file-deleted",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_deleted),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              2, G_TYPE_FILE, G_TYPE_BOOLEAN);
	signals[FILE_MOVED] =
		g_signal_new ("file-moved",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               file_moved),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              3, G_TYPE_FILE, G_TYPE_FILE, G_TYPE_BOOLEAN);
	signals[DIRECTORY_STARTED] =
		g_signal_new ("directory-started",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               directory_started),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              1, G_TYPE_FILE);
	signals[DIRECTORY_FINISHED] =
		g_signal_new ("directory-finished",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               directory_finished),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE,
		              5, G_TYPE_FILE, G_TYPE_UINT,
		              G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);
	signals[FINISHED] =
		g_signal_new ("finished",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (TrackerFileNotifierClass,
		                               finished),
		              NULL, NULL,
		              NULL,
		              G_TYPE_NONE, 0, G_TYPE_NONE);

	g_object_class_install_property (object_class,
	                                 PROP_INDEXING_TREE,
	                                 g_param_spec_object ("indexing-tree",
	                                                      "Indexing tree",
	                                                      "Indexing tree",
	                                                      TRACKER_TYPE_INDEXING_TREE,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_CONNECTION,
	                                 g_param_spec_object ("connection",
	                                                      "Connection",
	                                                      "Connection to use for queries",
	                                                      TRACKER_SPARQL_TYPE_CONNECTION,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (object_class,
	                                 PROP_FILE_ATTRIBUTES,
	                                 g_param_spec_string ("file-attributes",
	                                                      "File attributes",
	                                                      "File attributes",
	                                                      NULL,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
}

static void
tracker_file_notifier_init (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;
	GError *error = NULL;

	priv = tracker_file_notifier_get_instance_private (notifier);
	priv->stopped = TRUE;

	/* Set up monitor */
	priv->monitor = tracker_monitor_new (&error);

	if (!priv->monitor) {
		g_warning ("Could not init monitor: %s", error->message);
		g_error_free (error);
	} else {
		g_signal_connect (priv->monitor, "item-created",
		                  G_CALLBACK (monitor_item_created_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-updated",
		                  G_CALLBACK (monitor_item_updated_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-attribute-updated",
		                  G_CALLBACK (monitor_item_attribute_updated_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-deleted",
		                  G_CALLBACK (monitor_item_deleted_cb),
		                  notifier);
		g_signal_connect (priv->monitor, "item-moved",
		                  G_CALLBACK (monitor_item_moved_cb),
		                  notifier);
	}
}

TrackerFileNotifier *
tracker_file_notifier_new (TrackerIndexingTree     *indexing_tree,
                           TrackerSparqlConnection *connection,
                           const gchar             *file_attributes)
{
	g_return_val_if_fail (TRACKER_IS_INDEXING_TREE (indexing_tree), NULL);

	return g_object_new (TRACKER_TYPE_FILE_NOTIFIER,
	                     "indexing-tree", indexing_tree,
	                     "connection", connection,
	                     "file-attributes", file_attributes,
	                     NULL);
}

static void
tracker_file_notifier_continue (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->current_index_root)
		tracker_index_root_continue (priv->current_index_root);
	else
		notifier_check_next_root (notifier);
}

void
tracker_file_notifier_set_high_water (TrackerFileNotifier *notifier,
                                      gboolean             high_water)
{
	TrackerFileNotifierPrivate *priv;

	g_return_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier));

	priv = tracker_file_notifier_get_instance_private (notifier);
	if (priv->high_water == high_water)
		return;

	priv->high_water = high_water;

	if (!high_water && !priv->active &&
	    tracker_file_notifier_is_active (notifier)) {
		/* Maybe kick everything back into action */
		tracker_file_notifier_continue (notifier);
	}
}

gboolean
tracker_file_notifier_start (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier), FALSE);

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (priv->stopped) {
		priv->stopped = FALSE;
		tracker_file_notifier_continue (notifier);
	}

	return TRUE;
}

void
tracker_file_notifier_stop (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	g_return_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier));

	priv = tracker_file_notifier_get_instance_private (notifier);

	if (!priv->stopped) {
		if (priv->cancellable)
			g_cancellable_cancel (priv->cancellable);

		if (priv->current_index_root) {
			/* Index root arbitrarily cancelled cannot be easily
			 * resumed, best to queue it again and start from
			 * scratch.
			 */
			notifier_queue_root (notifier,
			                     priv->current_index_root->root,
			                     priv->current_index_root->flags |
			                     TRACKER_DIRECTORY_FLAG_PRIORITY,
			                     priv->current_index_root->ignore_root);
			g_clear_pointer (&priv->current_index_root,
			                 tracker_index_root_free);
		}

		priv->stopped = TRUE;
	}
}

gboolean
tracker_file_notifier_is_active (TrackerFileNotifier *notifier)
{
	TrackerFileNotifierPrivate *priv;

	g_return_val_if_fail (TRACKER_IS_FILE_NOTIFIER (notifier), FALSE);

	priv = tracker_file_notifier_get_instance_private (notifier);
	return priv->pending_index_roots || priv->current_index_root;
}
