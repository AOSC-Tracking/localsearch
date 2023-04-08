/*
 * Copyright (C) 2014 Carlos Garnacho <carlosg@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config-miners.h"

#include <libtracker-sparql/tracker-sparql.h>
#include <libtracker-extract/tracker-extract.h>

#include "tracker-extract-decorator.h"
#include "tracker-extract-persistence.h"

enum {
	PROP_EXTRACTOR = 1
};

typedef struct _TrackerExtractDecoratorPrivate TrackerExtractDecoratorPrivate;
typedef struct _ExtractData ExtractData;

struct _ExtractData {
	TrackerDecorator *decorator;
	TrackerDecoratorInfo *decorator_info;
	GFile *file;
	GCancellable *cancellable;
	gulong signal_id;
};

struct _TrackerExtractDecoratorPrivate {
	TrackerExtract *extractor;
	GTimer *timer;
	gboolean extracting;

	TrackerExtractPersistence *persistence;
	GDBusProxy *index_proxy;
};

static GInitableIface *parent_initable_iface;

static void decorator_get_next_file (TrackerDecorator *decorator);
static void tracker_extract_decorator_initable_iface_init (GInitableIface *iface);

static void decorator_ignore_file (GFile                   *file,
                                   TrackerExtractDecorator *decorator,
                                   const gchar             *error_message,
                                   const gchar             *extra_info);

G_DEFINE_TYPE_WITH_CODE (TrackerExtractDecorator, tracker_extract_decorator,
                        TRACKER_TYPE_DECORATOR_FS,
                        G_ADD_PRIVATE (TrackerExtractDecorator)
                        G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, tracker_extract_decorator_initable_iface_init))

static void
tracker_extract_decorator_get_property (GObject    *object,
                                        guint       param_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
	TrackerExtractDecoratorPrivate *priv;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (object));

	switch (param_id) {
	case PROP_EXTRACTOR:
		g_value_set_object (value, priv->extractor);
		break;
	}
}

static void
tracker_extract_decorator_set_property (GObject      *object,
                                        guint         param_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
	TrackerExtractDecoratorPrivate *priv;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (object));

	switch (param_id) {
	case PROP_EXTRACTOR:
		priv->extractor = g_value_dup_object (value);
		break;
	}
}

static void
tracker_extract_decorator_finalize (GObject *object)
{
	TrackerExtractDecoratorPrivate *priv;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (object));

	if (priv->extractor)
		g_object_unref (priv->extractor);

	if (priv->timer)
		g_timer_destroy (priv->timer);

	g_clear_object (&priv->index_proxy);

	G_OBJECT_CLASS (tracker_extract_decorator_parent_class)->finalize (object);
}

static void
ensure_data (TrackerExtractInfo *info)
{
	TrackerResource *resource, *dataobject;
	GStrv rdf_types;
	const gchar *mimetype;
	g_autofree gchar *uri = NULL;
	GFile *file;
	gint i;

	resource = tracker_extract_info_get_resource (info);
	mimetype = tracker_extract_info_get_mimetype (info);
	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);

	dataobject = tracker_resource_new (uri);
	tracker_resource_set_string (resource, "nie:mimeType", mimetype);
	tracker_resource_add_take_relation (resource, "nie:isStoredAs", dataobject);
	tracker_resource_add_uri (dataobject, "nie:interpretedAs",
	                          tracker_resource_get_identifier (resource));

	rdf_types = tracker_extract_module_manager_get_rdf_types (mimetype);

	for (i = 0; rdf_types[i] != NULL; i++)
		tracker_resource_add_uri (resource, "rdf:type", rdf_types[i]);

	g_strfreev (rdf_types);
}

static void
tracker_extract_decorator_update (TrackerDecorator   *decorator,
                                  TrackerExtractInfo *info,
                                  TrackerBatch       *batch)
{
	TrackerResource *resource;
	const gchar *graph, *mime_type, *hash;
	gchar *update_hash_sparql;
	g_autofree gchar *uri = NULL;
	GFile *file;

	mime_type = tracker_extract_info_get_mimetype (info);
	hash = tracker_extract_module_manager_get_hash (mime_type);
	graph = tracker_extract_info_get_graph (info);
	resource = tracker_extract_info_get_resource (info);
	file = tracker_extract_info_get_file (info);
	uri = g_file_get_uri (file);

	update_hash_sparql =
		g_strdup_printf ("INSERT DATA {"
		                 "  GRAPH tracker:FileSystem {"
		                 "    <%s> tracker:extractorHash \"%s\" ."
		                 "  }"
		                 "}",
		                 uri, hash);
	tracker_batch_add_sparql (batch, update_hash_sparql);

	if (resource)
		tracker_batch_add_resource (batch, graph, resource);
}

static void
get_metadata_cb (TrackerExtract *extract,
                 GAsyncResult   *result,
                 ExtractData    *data)
{
	TrackerExtractDecoratorPrivate *priv;
	TrackerExtractInfo *info;
	GError *error = NULL;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (data->decorator));
	info = tracker_extract_file_finish (extract, result, &error);

	tracker_extract_persistence_remove_file (priv->persistence, data->file);

	if (data->cancellable && data->signal_id != 0) {
		g_cancellable_disconnect (data->cancellable, data->signal_id);
	}

	if (error) {
		decorator_ignore_file (data->file,
		                       TRACKER_EXTRACT_DECORATOR (data->decorator),
		                       error->message, NULL);
		tracker_decorator_info_complete_error (data->decorator_info, error);
	} else {
		ensure_data (info);
		tracker_decorator_info_complete (data->decorator_info, info);
		tracker_extract_info_unref (info);
	}

	priv->extracting = FALSE;
	decorator_get_next_file (data->decorator);

	tracker_decorator_info_unref (data->decorator_info);
	g_object_unref (data->file);
	g_object_unref (data->cancellable);
	g_free (data);
}

static void
task_cancellable_cancelled_cb (GCancellable *cancellable,
                               ExtractData  *data)
{
	TrackerExtractDecoratorPrivate *priv;
	gchar *uri;

	/* Delete persistence file on cancellation, we don't want to interpret
	 * this as a failed operation.
	 */
	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (data->decorator));
	tracker_extract_persistence_remove_file (priv->persistence, data->file);
	uri = g_file_get_uri (data->file);

	g_debug ("Cancelled task for '%s' was currently being "
		 "processed, _exit()ing immediately",
		 uri);
	g_free (uri);

	_exit (EXIT_FAILURE);
}

static void
decorator_get_next_file (TrackerDecorator *decorator)
{
	TrackerExtractDecoratorPrivate *priv;
	TrackerDecoratorInfo *info;
	g_autoptr (GError) error = NULL;
	ExtractData *data;
	GCancellable *cancellable;
	GFile *file;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (decorator));

	if (!tracker_miner_is_started (TRACKER_MINER (decorator)) ||
	    tracker_miner_is_paused (TRACKER_MINER (decorator)))
		return;

	if (priv->extracting)
		return;

	info = tracker_decorator_next (decorator, &error);

	if (!info) {
		if (error &&
		    g_error_matches (error,
		                     TRACKER_DECORATOR_ERROR,
		                     TRACKER_DECORATOR_ERROR_PAUSED)) {
			g_debug ("Next item is on hold because miner is paused");
		} else if (error) {
			g_warning ("Next item could not be processed, %s", error->message);
		}

		g_clear_error (&error);
		return;
	} else if (!tracker_decorator_info_get_url (info)) {
		/* Skip virtual elements with no real file representation */
		tracker_decorator_info_unref (info);
		decorator_get_next_file (decorator);
		return;
	}

	file = g_file_new_for_uri (tracker_decorator_info_get_url (info));

	if (!g_file_is_native (file)) {
		g_warning ("URI '%s' is not native",
		           tracker_decorator_info_get_url (info));
		tracker_decorator_info_unref (info);
		decorator_get_next_file (decorator);
		return;
	}

	priv->extracting = TRUE;

	data = g_new0 (ExtractData, 1);
	data->decorator = decorator;
	data->decorator_info = info;
	data->file = file;
	cancellable = tracker_decorator_info_get_cancellable (info);

	TRACKER_NOTE (DECORATOR,
	              g_message ("[Decorator] Extracting metadata for '%s'",
	                         tracker_decorator_info_get_url (info)));

	tracker_extract_persistence_add_file (priv->persistence, data->file);

	g_set_object (&data->cancellable, cancellable);

	if (data->cancellable) {
		data->signal_id = g_cancellable_connect (data->cancellable,
		                                         G_CALLBACK (task_cancellable_cancelled_cb),
		                                         data, NULL);
	}

	tracker_extract_file (priv->extractor,
	                      tracker_decorator_info_get_url (info),
	                      tracker_decorator_info_get_mimetype (info),
	                      cancellable,
	                      (GAsyncReadyCallback) get_metadata_cb, data);
}

static void
tracker_extract_decorator_paused (TrackerMiner *miner)
{
	TrackerExtractDecoratorPrivate *priv;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (miner));
	g_debug ("Decorator paused");

	if (priv->timer)
		g_timer_stop (priv->timer);
}

static void
tracker_extract_decorator_resumed (TrackerMiner *miner)
{
	TrackerExtractDecoratorPrivate *priv;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (miner));
	g_debug ("Decorator resumed, processing remaining %d items",
		 tracker_decorator_get_n_items (TRACKER_DECORATOR (miner)));

	if (priv->timer)
		g_timer_continue (priv->timer);

	decorator_get_next_file (TRACKER_DECORATOR (miner));
}

static void
tracker_extract_decorator_items_available (TrackerDecorator *decorator)
{
	TrackerExtractDecoratorPrivate *priv;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (decorator));
	g_debug ("Starting to process %d items",
	         tracker_decorator_get_n_items (decorator));

	priv->timer = g_timer_new ();
	if (tracker_miner_is_paused (TRACKER_MINER (decorator)))
		g_timer_stop (priv->timer);

	decorator_get_next_file (decorator);
}

static void
tracker_extract_decorator_finished (TrackerDecorator *decorator)
{
	TrackerExtractDecoratorPrivate *priv;
	gchar *time_str;
	gdouble elapsed = 0;

	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (decorator));
	if (priv->timer) {
		elapsed = g_timer_elapsed (priv->timer, NULL);
		g_clear_pointer (&priv->timer, g_timer_destroy);
	}

	time_str = tracker_seconds_to_string (elapsed, TRUE);
	g_debug ("Extraction finished in %s", time_str);
	g_free (time_str);
}

static void
tracker_extract_decorator_error (TrackerDecorator   *decorator,
                                 TrackerExtractInfo *extract_info,
                                 const gchar        *error_message)
{
	g_autofree gchar *sparql = NULL;
	TrackerResource *resource;
	const gchar *graph;
	GFile *file;

	file = tracker_extract_info_get_file (extract_info);
	graph = tracker_extract_info_get_graph (extract_info);
	resource = tracker_extract_info_get_resource (extract_info);

	sparql = tracker_resource_print_sparql_update (resource,
	                                               NULL,
	                                               graph);

	decorator_ignore_file (file, TRACKER_EXTRACT_DECORATOR (decorator), error_message, sparql);
}

static void
tracker_extract_decorator_class_init (TrackerExtractDecoratorClass *klass)
{
	TrackerDecoratorClass *decorator_class = TRACKER_DECORATOR_CLASS (klass);
	TrackerMinerClass *miner_class = TRACKER_MINER_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_extract_decorator_finalize;
	object_class->get_property = tracker_extract_decorator_get_property;
	object_class->set_property = tracker_extract_decorator_set_property;

	miner_class->paused = tracker_extract_decorator_paused;
	miner_class->resumed = tracker_extract_decorator_resumed;

	decorator_class->items_available = tracker_extract_decorator_items_available;
	decorator_class->finished = tracker_extract_decorator_finished;
	decorator_class->error = tracker_extract_decorator_error;
	decorator_class->update = tracker_extract_decorator_update;

	g_object_class_install_property (object_class,
	                                 PROP_EXTRACTOR,
	                                 g_param_spec_object ("extractor",
	                                                      "Extractor",
	                                                      "Extractor",
	                                                      TRACKER_TYPE_EXTRACT,
	                                                      G_PARAM_READWRITE |
	                                                      G_PARAM_CONSTRUCT_ONLY |
	                                                      G_PARAM_STATIC_STRINGS));
}

static void
decorator_ignore_file (GFile                   *file,
                       TrackerExtractDecorator *decorator,
                       const gchar             *error_message,
                       const gchar             *extra_info)
{
	TrackerSparqlConnection *conn;
	GError *error = NULL;
	gchar *uri, *query;
	const gchar *mimetype, *hash;
	GFileInfo *info;

	uri = g_file_get_uri (file);
	g_debug ("Extraction on file '%s' failed in previous execution, ignoring", uri);

	info = g_file_query_info (file,
	                          G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
	                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                          NULL, &error);

	if (info) {
		tracker_error_report (file, error_message, extra_info);

		mimetype = g_file_info_get_attribute_string (info,
		                                             G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
		hash = tracker_extract_module_manager_get_hash (mimetype);
		g_object_unref (info);

		query = g_strdup_printf ("INSERT DATA { GRAPH tracker:FileSystem {"
		                         "  <%s> tracker:extractorHash \"%s\" ;"
		                         "}}",
		                         uri, hash);
	} else {
		g_debug ("Could not get mimetype: %s", error->message);

		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
			tracker_error_report_delete (file);
		else
			tracker_error_report (file, error->message, NULL);

		g_clear_error (&error);
		query = g_strdup_printf ("DELETE {"
		                         "  GRAPH ?g { <%s> a rdfs:Resource }"
		                         "} WHERE {"
		                         "  GRAPH ?g { <%s> a nfo:FileDataObject }"
		                         "  FILTER (?g != tracker:FileSystem)"
		                         "}",
		                         uri, uri);
	}

	conn = tracker_miner_get_connection (TRACKER_MINER (decorator));
	tracker_sparql_connection_update (conn, query, NULL, &error);

	if (error) {
		g_warning ("Failed to update ignored file '%s': %s",
		           uri, error->message);
		g_error_free (error);
	}

	g_free (query);
	g_free (uri);
}

static void
persistence_ignore_file (GFile    *file,
                         gpointer  user_data)
{
	TrackerExtractDecorator *decorator = user_data;

	decorator_ignore_file (file, decorator, "Crash/hang handling file", NULL);
}

static void
tracker_extract_decorator_init (TrackerExtractDecorator *decorator)
{
}

static void
update_graphs_from_proxy (TrackerExtractDecorator *decorator,
                          GDBusProxy              *proxy)
{
	const gchar **graphs = NULL;
	GVariant *v;

	v = g_dbus_proxy_get_cached_property (proxy, "Graphs");
	if (v)
		graphs = g_variant_get_strv (v, NULL);

	tracker_decorator_set_priority_graphs (TRACKER_DECORATOR (decorator),
	                                       graphs);
	g_free (graphs);
}

static void
proxy_properties_changed_cb (GDBusProxy *proxy,
                             GVariant   *changed_properties,
                             GStrv       invalidated_properties,
                             gpointer    user_data)
{
	update_graphs_from_proxy (user_data, proxy);
}

static gboolean
tracker_extract_decorator_initable_init (GInitable     *initable,
                                         GCancellable  *cancellable,
                                         GError       **error)
{
	TrackerExtractDecorator        *decorator;
	TrackerExtractDecoratorPrivate *priv;
	GDBusConnection                *conn;
	gboolean                        ret = TRUE;

	decorator = TRACKER_EXTRACT_DECORATOR (initable);
	priv = tracker_extract_decorator_get_instance_private (TRACKER_EXTRACT_DECORATOR (decorator));

	conn = g_bus_get_sync (TRACKER_IPC_BUS, NULL, error);
	if (conn == NULL) {
		ret = FALSE;
		goto out;
	}

	priv->index_proxy = g_dbus_proxy_new_sync (conn,
	                                           G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
	                                           NULL,
	                                           "org.freedesktop.Tracker3.Miner.Files.Control",
	                                           "/org/freedesktop/Tracker3/Miner/Files/Proxy",
	                                           "org.freedesktop.Tracker3.Miner.Files.Proxy",
	                                           cancellable,
	                                           error);
	if (!priv->index_proxy) {
		ret = FALSE;
		goto out;
	}

	g_signal_connect (priv->index_proxy, "g-properties-changed",
	                  G_CALLBACK (proxy_properties_changed_cb), decorator);
	update_graphs_from_proxy (decorator, priv->index_proxy);

	/* Chainup to parent's init last, to have a chance to export our
	 * DBus interface before RequestName returns. Otherwise our iface
	 * won't be ready by the time the tracker-extract appear on the bus. */
	if (!parent_initable_iface->init (initable, cancellable, error)) {
		ret = FALSE;
	}

	priv->persistence = tracker_extract_persistence_initialize (persistence_ignore_file,
	                                                            decorator);
out:
	g_clear_object (&conn);

	return ret;
}

static void
tracker_extract_decorator_initable_iface_init (GInitableIface *iface)
{
	parent_initable_iface = g_type_interface_peek_parent (iface);
	iface->init = tracker_extract_decorator_initable_init;
}

TrackerDecorator *
tracker_extract_decorator_new (TrackerSparqlConnection  *connection,
                               TrackerExtract           *extract,
                               GCancellable             *cancellable,
                               GError                  **error)
{
	return g_initable_new (TRACKER_TYPE_EXTRACT_DECORATOR,
	                       cancellable, error,
	                       "connection", connection,
	                       "extractor", extract,
	                       NULL);
}
