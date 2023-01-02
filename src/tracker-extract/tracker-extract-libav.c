/*
 * Copyright (C) 2013-2014 Jolla Ltd. <andrew.den.exter@jollamobile.com>
 * Author: Andrew den Exter <andrew.den.exter@jollamobile.com>
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
 */


#include <glib.h>

#include <libtracker-sparql/tracker-ontologies.h>
#include <libtracker-miners-common/tracker-file-utils.h>

#include <libtracker-extract/tracker-extract.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

static AVDictionaryEntry *
find_tag (AVFormatContext *format,
          AVStream        *stream1,
          AVStream        *stream2,
          const gchar     *name)
{
	AVDictionaryEntry *tag = av_dict_get (format->metadata, name, NULL, 0);
	if (!tag && stream1) {
		tag = av_dict_get (stream1->metadata, name, NULL, 0);
	}
	if (!tag && stream2) {
		tag = av_dict_get (stream2->metadata, name, NULL, 0);
	}

	return tag;
}

static void
extract_audio_info(TrackerResource *metadata,
                   AVFormatContext *format,
                   AVStream        *audio_stream)
{
	if (audio_stream->codecpar->sample_rate > 0) {
		tracker_resource_set_int64 (metadata, "nfo:sampleRate", audio_stream->codecpar->sample_rate);
	}
	if (audio_stream->codecpar->channels > 0) {
		tracker_resource_set_int64 (metadata, "nfo:channels", audio_stream->codecpar->channels);
	}
}

static void
extract_video_info(TrackerResource *metadata,
                   AVFormatContext *format,
                   AVStream        *video_stream)
{
	g_autofree char *content_created = NULL;
	AVDictionaryEntry *tag = NULL;

	tracker_resource_add_uri(metadata, "rdf:type", "nmm:Video");

	if (video_stream->codecpar->width > 0 && video_stream->codecpar->height > 0) {
		tracker_resource_set_int64 (metadata, "nfo:width", video_stream->codecpar->width);
		tracker_resource_set_int64 (metadata, "nfo:height", video_stream->codecpar->height);
	}

	if (video_stream->avg_frame_rate.num > 0) {
		gdouble frame_rate = (gdouble) video_stream->avg_frame_rate.num
		                     / video_stream->avg_frame_rate.den;
		tracker_resource_set_double (metadata, "nfo:frameRate", frame_rate);
	}

	if (video_stream->duration > 0) {
		gint64 duration = av_rescale(video_stream->duration, video_stream->time_base.num,
		                             video_stream->time_base.den);
		tracker_resource_set_int64 (metadata, "nfo:duration", duration);
	}

	if (video_stream->sample_aspect_ratio.num > 0) {
		gdouble aspect_ratio = (gdouble) video_stream->sample_aspect_ratio.num
		                       / video_stream->sample_aspect_ratio.den;
		tracker_resource_set_double (metadata, "nfo:aspectRatio", aspect_ratio);
	}

	if (video_stream->nb_frames > 0) {
		tracker_resource_set_int64 (metadata, "nfo:frameCount", video_stream->nb_frames);
	}

	if ((tag = find_tag (format, video_stream, NULL, "synopsis"))) {
		tracker_resource_set_string (metadata, "nmm:synopsis", tag->value);
	}

	if ((tag = find_tag (format, video_stream, NULL, "episode_sort"))) {
		tracker_resource_set_int64 (metadata, "nmm:episodeNumber", atoi (tag->value));
	}

	if ((tag = find_tag (format, video_stream, NULL, "season_number"))) {
		tracker_resource_set_int64 (metadata, "nmm:season", atoi (tag->value));
	}

	if ((tag = find_tag (format, video_stream, NULL, "creation_time"))) {
		content_created = tracker_date_guess (tag->value);
		if (content_created) {
			tracker_resource_set_string (metadata, "nie:contentCreated", content_created);
		}
	}
}

static TrackerResource *
ensure_file_resource (TrackerResource *resource,
                      const gchar     *file_url)
{
	TrackerResource *file_resource;

	file_resource = tracker_resource_get_first_relation (resource, "nie:isStoredAs");
	if (!file_resource) {
		file_resource = tracker_resource_new (file_url);
		tracker_resource_set_take_relation (resource, "nie:isStoredAs", file_resource);
	}

	return file_resource;
}

static void
extract_music_piece_info(const char      *file_url,
                         TrackerResource *metadata,
                         AVFormatContext *format,
                         AVStream        *audio_stream)
{
	g_autoptr(TrackerResource) artist = NULL, performer = NULL;
	g_autofree char *content_created = NULL;
	AVDictionaryEntry *tag = NULL;

	tracker_resource_add_uri (metadata, "rdf:type", "nmm:MusicPiece");
	tracker_resource_add_uri (metadata, "rdf:type", "nfo:Audio");

	if (audio_stream->duration > 0) {
		gint64 duration = av_rescale(audio_stream->duration, audio_stream->time_base.num,
		                             audio_stream->time_base.den);
		tracker_resource_set_int64 (metadata, "nfo:duration", duration);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "track"))) {
		int track = atoi(tag->value);
		if (track > 0) {
			tracker_resource_set_int64 (metadata, "nmm:trackNumber", track);
		}
	}

	if ((tag = find_tag (format, audio_stream, NULL, "artist"))) {
		artist = tracker_extract_new_artist (tag->value);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "performer"))) {
		performer = tracker_extract_new_artist (tag->value);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "date"))) {
		content_created = tracker_date_guess (tag->value);
		if (content_created) {
			tracker_resource_set_string (metadata, "nie:contentCreated", content_created);
		}
	}

	if (artist) {
		tracker_resource_set_relation (metadata, "nmm:artist", artist);

		if ((tag = find_tag (format, audio_stream, NULL, "MUSICBRAINZ_ARTISTID"))) {
			const char *mb_artist_id = tag->value;
			g_autofree char *mb_artist_uri = g_strdup_printf("https://musicbrainz.org/artist/%s", mb_artist_id);
			TrackerResource *mb_artist = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Artist",
			                                                                    mb_artist_id,
			                                                                    mb_artist_uri);

			tracker_resource_add_take_relation (artist, "tracker:hasExternalReference", mb_artist);
		}
	}

	if (performer) {
		tracker_resource_set_relation (metadata, "nmm:performer", performer);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "composer"))) {
		TrackerResource *composer = tracker_extract_new_artist (tag->value);
		tracker_resource_set_relation (metadata, "nmm:composer", composer);
		g_object_unref (composer);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "MUSICBRAINZ_TRACKID"))) {
		const char *mb_recording_id = tag->value;
		g_autofree char *mb_recording_uri = g_strdup_printf("https://musicbrainz.org/recording/%s", mb_recording_id);
		TrackerResource *mb_recording = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Recording",
		                                                                       mb_recording_id,
		                                                                       mb_recording_uri);

		tracker_resource_add_take_relation (metadata, "tracker:hasExternalReference", mb_recording);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "MUSICBRAINZ_RELEASETRACKID"))) {
		const char *mb_track_id = tag->value;
		g_autofree char *mb_track_uri = g_strdup_printf("https://musicbrainz.org/track/%s", mb_track_id);
		TrackerResource *mb_track = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Track",
		                                                                   mb_track_id,
		                                                                   mb_track_uri);

		tracker_resource_add_take_relation (metadata, "tracker:hasExternalReference", mb_track);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "ACOUSTID_FINGERPRINT"))) {
		const char *acoustid_fingerprint = tag->value;
		TrackerResource *hash_resource = tracker_resource_new (NULL);
		TrackerResource *file_resource = ensure_file_resource (metadata, file_url);

		tracker_resource_set_uri (hash_resource, "rdf:type", "nfo:FileHash");
		tracker_resource_set_string (hash_resource, "nfo:hashValue", acoustid_fingerprint);
		tracker_resource_set_string (hash_resource, "nfo:hashAlgorithm", "chromaprint");

		tracker_resource_add_take_relation (file_resource, "nfo:hasHash", hash_resource);
	}
}

static void
extract_music_album_info(TrackerResource *metadata,
                         AVFormatContext *format,
                         AVStream        *audio_stream)
{
	g_autoptr(TrackerResource) album_artist = NULL, album_disc = NULL, album = NULL;
	const char *album_artist_name = NULL;
	const char *album_title = NULL;
	const char *content_created;
	int disc_number = 1;
	int album_track_count = 0;
	AVDictionaryEntry *tag = NULL;

	if ((tag = find_tag (format, audio_stream, NULL, "album"))) {
		album_title = tag->value;
	}

	if (!album_title) {
		return;
	}

	if ((tag = find_tag (format, audio_stream, NULL, "album_artist"))) {
		album_artist_name = tag->value;
		album_artist = tracker_extract_new_artist (album_artist_name);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "disc"))) {
		disc_number = atoi (tag->value);
	}

	content_created = tracker_resource_get_first_string (metadata, "nie:contentCreated");

	album_disc = tracker_extract_new_music_album_disc (album_title, album_artist, disc_number, content_created);
	album = tracker_resource_get_first_relation (album_disc, "nmm:albumDiscAlbum");

	tracker_resource_set_relation (metadata, "nmm:musicAlbumDisc", album_disc);
	tracker_resource_set_relation (metadata, "nmm:musicAlbum", tracker_resource_get_first_relation (album_disc, "nmm:albumDiscAlbum"));

	/* There is no officially specified 'total tracks' field, these two names
	 * are taken from MusicBrainz Picard tag mapping for Vorbis comments.
	 *
	 * https://picard-docs.musicbrainz.org/en/appendices/tag_mapping.html
	 */
	if ((tag = find_tag (format, audio_stream, NULL, "TOTALTRACKS"))) {
		album_track_count = atoi (tag->value);
	} else if ((tag = find_tag (format, audio_stream, NULL, "TRACKTOTAL"))) {
		album_track_count = atoi (tag->value);
	}

	if (album_track_count > 0) {
		tracker_resource_set_int (album, "nmm:albumTrackCount", album_track_count);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "MUSICBRAINZ_ALBUMID"))) {
		const char *mb_release_id = tag->value;
		g_autofree char *mb_release_uri = g_strdup_printf("https://musicbrainz.org/release/%s", mb_release_id);
		TrackerResource *mb_release = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Release",
		                                                                     mb_release_id,
		                                                                     mb_release_uri);

		tracker_resource_add_take_relation (album, "tracker:hasExternalReference", mb_release);
	}

	if ((tag = find_tag (format, audio_stream, NULL, "MUSICBRAINZ_RELEASEGROUPID"))) {
		const char *mb_release_group_id = tag->value;
		g_autofree char *mb_release_group_uri = g_strdup_printf("https://musicbrainz.org/release-group/%s", mb_release_group_id);
		TrackerResource *mb_release_group = tracker_extract_new_external_reference("https://musicbrainz.org/doc/Release_Group",
		                                                                           mb_release_group_id,
		                                                                           mb_release_group_uri);

		tracker_resource_add_take_relation (album, "tracker:hasExternalReference", mb_release_group);
	}
}

G_MODULE_EXPORT gboolean
tracker_extract_get_metadata (TrackerExtractInfo  *info,
                              GError             **error)
{
	GFile *file;
	TrackerResource *metadata;
	gchar *absolute_file_path;
	gchar *uri, *resource_uri;
	AVFormatContext *format = NULL;
	AVStream *audio_stream = NULL;
	AVStream *video_stream = NULL;
	int audio_stream_index;
	int video_stream_index;
	AVDictionaryEntry *tag = NULL;
	const char *title = NULL;

	file = tracker_extract_info_get_file (info);

	uri = g_file_get_uri (file);

	absolute_file_path = g_file_get_path (file);
	if (avformat_open_input (&format, absolute_file_path, NULL, NULL)) {
		g_free (absolute_file_path);
		g_free (uri);
		return FALSE;
	}
	g_free (absolute_file_path);

	avformat_find_stream_info (format, NULL);

	audio_stream_index = av_find_best_stream (format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (audio_stream_index >= 0) {
		audio_stream = format->streams[audio_stream_index];
	}

	video_stream_index = av_find_best_stream (format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (video_stream_index >= 0) {
		video_stream = format->streams[video_stream_index];
	}

	if (!audio_stream && !video_stream) {
		avformat_close_input (&format);
		g_free (uri);
		return FALSE;
	}

	resource_uri = tracker_file_get_content_identifier (file, NULL, NULL);
	metadata = tracker_resource_new (resource_uri);
	g_free (resource_uri);

	if (audio_stream) {
		extract_audio_info (metadata, format, audio_stream);
	}

	if (video_stream && !(video_stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
		extract_video_info (metadata, format, video_stream);
	} else if (audio_stream) {
		extract_music_piece_info (uri, metadata, format, audio_stream);
		extract_music_album_info (metadata, format, audio_stream);
	}

	if (format->bit_rate > 0) {
		tracker_resource_set_int64 (metadata, "nfo:averageBitrate", format->bit_rate);
	}

	if ((tag = find_tag (format, audio_stream, video_stream, "comment"))) {
		tracker_resource_set_string (metadata, "nie:comment", tag->value);
	}

	if ((tag = find_tag (format, audio_stream, video_stream, "copyright"))) {
		tracker_resource_set_string (metadata, "nie:copyright", tag->value);
	}

	if ((tag = find_tag (format, audio_stream, video_stream, "description"))) {
		tracker_resource_set_string (metadata, "nie:description", tag->value);
	}

	if ((tag = find_tag (format, audio_stream, video_stream, "genre"))) {
		tracker_resource_set_string (metadata, "nfo:genre", tag->value);
	}

	if ((tag = find_tag (format, audio_stream, video_stream, "title"))) {
		title = tag->value;
	}

	tracker_guarantee_resource_title_from_file (metadata, "nie:title", title, uri, NULL);

	g_free (uri);

	avformat_close_input (&format);

	tracker_extract_info_set_resource (info, metadata);
	g_object_unref (metadata);

	return TRUE;
}
