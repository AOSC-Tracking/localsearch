/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009, Nokia
 *
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
 *
 * Authors: Philip Van Hoof <philip@codeminded.be>
 */

#include "config.h"

#include <totem-pl-parser.h>

#include <libtracker-common/tracker-ontology.h>

#include "tracker-writeback-file.h"

#define TRACKER_TYPE_WRITEBACK_PLAYLIST (tracker_writeback_playlist_get_type ())

typedef struct TrackerWritebackPlaylist TrackerWritebackPlaylist;
typedef struct TrackerWritebackPlaylistClass TrackerWritebackPlaylistClass;

struct TrackerWritebackPlaylist {
	TrackerWritebackFile parent_instance;
};

struct TrackerWritebackPlaylistClass {
	TrackerWritebackFileClass parent_class;
};

static GType                tracker_writeback_playlist_get_type     (void) G_GNUC_CONST;
static gboolean             writeback_playlist_update_file_metadata (TrackerWritebackFile *wbf,
                                                                     GFile                *file,
                                                                     GPtrArray            *values,
                                                                     TrackerClient        *client);
static const gchar * const *writeback_playlist_content_types        (TrackerWritebackFile *wbf);

G_DEFINE_DYNAMIC_TYPE (TrackerWritebackPlaylist, tracker_writeback_playlist, TRACKER_TYPE_WRITEBACK_FILE);

static void
tracker_writeback_playlist_class_init (TrackerWritebackPlaylistClass *klass)
{
	TrackerWritebackFileClass *writeback_file_class = TRACKER_WRITEBACK_FILE_CLASS (klass);

	writeback_file_class->update_file_metadata = writeback_playlist_update_file_metadata;
	writeback_file_class->content_types = writeback_playlist_content_types;
}

static void
tracker_writeback_playlist_class_finalize (TrackerWritebackPlaylistClass *klass)
{
}

static void
tracker_writeback_playlist_init (TrackerWritebackPlaylist *wbm)
{
}

static const gchar * const *
writeback_playlist_content_types (TrackerWritebackFile *wbf)
{
	static const gchar *content_types[] = {
		"audio/x-mpegurl",
		"audio/mpegurl",
		"audio/x-scpls",
		"audio/x-pn-realaudio",
		"application/ram",
		"application/vnd.ms-wpl",
		"application/smil",
		"audio/x-ms-asx",
		NULL
	};

	return content_types;
}

static void
rewrite_playlist (TrackerClient *client, GFile *file, const gchar *subject)
{
	gchar *path;
	GPtrArray *array;
	GError *error = NULL;
	gchar *query;

	path = g_file_get_path (file);

	query = g_strdup_printf ("SELECT ?entry { ?unknown a nfo:MediaFileListEntry ; "
	                                                  "nie:isStoredAs <%s> ; "
	                                                  "nfo:entryContent ?entry"
	                         "}", subject);

	array = tracker_resources_sparql_query (client, query, &error);

	g_free (query);

	if (!error) {
		if (array && array->len > 0) {
			guint i;
#if 0
			TotemPlPlaylist *playlist;

			playlist = totem_pl_playlist_new ();
#endif
			for (i = 0; i < array->len; i++) {
				GStrv row;

				row = g_ptr_array_index (array, i);

				if (row && row[0]) {
#if 0
					TotemPlPlaylistIter iter;

					totem_pl_playlist_append  (playlist, &iter);
					totem_pl_playlist_set_value (playlist, &iter,
					                             TOTEM_PL_PARSER_FIELD_URI,
					                             row[0]);
#endif
				}
			}
#if 0
			/* TODO: write to path as same type as path */
			g_object_unref (playlist);
#endif
		} else {
			/* TODO: Empty the file in @path */
		}

	} else {
		g_clear_error (&error);
	}

	if (array) {
		g_ptr_array_foreach (array, (GFunc) g_strfreev, NULL);
		g_ptr_array_free (array, TRUE);
	}

	g_free (path);
}

static gboolean
writeback_playlist_update_file_metadata (TrackerWritebackFile *writeback_file,
                                         GFile                *file,
                                         GPtrArray            *values,
                                         TrackerClient        *client)
{
	guint n;

	for (n = 0; n < values->len; n++) {
		const GStrv row = g_ptr_array_index (values, n);
		if (g_strcmp0 (row[1], TRACKER_NFO_PREFIX "entryCounter") == 0) {
			rewrite_playlist (client, file, row[0]);
			break;
		}
	}

	return TRUE;
}

TrackerWriteback *
writeback_module_create (GTypeModule *module)
{
	tracker_writeback_playlist_register_type (module);

	return g_object_new (TRACKER_TYPE_WRITEBACK_PLAYLIST, NULL);
}

const gchar * const *
writeback_module_get_rdf_types (void)
{
	static const gchar *rdftypes[] = {
		TRACKER_NFO_PREFIX "MediaList",
		TRACKER_NFO_PREFIX "MediaFileListEntry",
		NULL
	};

	return rdftypes;
}
