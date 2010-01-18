/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strcasestr() */
#endif

#include <ctype.h>
#include <string.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <setjmp.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <jpeglib.h>

#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-ontology.h>
#include <libtracker-common/tracker-statement-list.h>
#include <libtracker-common/tracker-type-utils.h>
#include <libtracker-common/tracker-utils.h>

#include "tracker-main.h"
#include "tracker-xmp.h"
#include "tracker-iptc.h"
#include "tracker-exif.h"

#ifdef HAVE_LIBEXIF
#define EXIF_NAMESPACE          "Exif"
#define EXIF_NAMESPACE_LENGTH   4
#endif /* HAVE_LIBEXIF */

#ifdef HAVE_EXEMPI
#define XMP_NAMESPACE           "http://ns.adobe.com/xap/1.0/\x00"
#define XMP_NAMESPACE_LENGTH    29
#endif /* HAVE_EXEMPI */

#ifdef HAVE_LIBIPTCDATA
#define PS3_NAMESPACE           "Photoshop 3.0\0"
#define PS3_NAMESPACE_LENGTH    14
#include <libiptcdata/iptc-jpeg.h>
#endif /* HAVE_LIBIPTCDATA */

typedef struct {
	gchar *camera;
	gchar *title;
	gchar *orientation;
	gchar *copyright;
	gchar *white_balance;
	gchar *fnumber;
	gchar *flash;
	gchar *focal_length;
	gchar *artist;
	gchar *exposure_time;
	gchar *iso_speed_ratings;
	gchar *date;
	gchar *description;
	gchar *metering_mode;
	gchar *creator;
	gchar *comment;
	gchar *city;
	gchar *state;
	gchar *address;
	gchar *country; 
} MergeData;

static void extract_jpeg (const gchar          *filename,
                          TrackerSparqlBuilder *metadata);

static TrackerExtractData data[] = {
	{ "image/jpeg", extract_jpeg },
	{ NULL, NULL }
};

struct tej_error_mgr {
	struct jpeg_error_mgr jpeg;
	jmp_buf setjmp_buffer;
};

static void
insert_keywords (TrackerSparqlBuilder *metadata,
                 gchar                *keywords)
{
	char *saveptr, *p;
	size_t len;

	p = keywords;
	keywords = strchr (keywords, '"');

	if (keywords) {
		keywords++;
	} else {
		keywords = p;
	}

	len = strlen (keywords);
	if (keywords[len - 1] == '"') {
		keywords[len - 1] = '\0';
	}

	for (p = strtok_r (keywords, ",; ", &saveptr);
	     p;
	     p = strtok_r (NULL, ",; ", &saveptr)) {
		tracker_sparql_builder_predicate (metadata, "nao:hasTag");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nao:Tag");

		tracker_sparql_builder_predicate (metadata, "nao:prefLabel");
		tracker_sparql_builder_object_unvalidated (metadata, p);

		tracker_sparql_builder_object_blank_close (metadata);
	}
}

static void
extract_jpeg_error_exit (j_common_ptr cinfo)
{
	struct tej_error_mgr *h = (struct tej_error_mgr *) cinfo->err;
	(*cinfo->err->output_message)(cinfo);
	longjmp (h->setjmp_buffer, 1);
}

static void
extract_jpeg (const gchar          *uri,
              TrackerSparqlBuilder *metadata)
{
	struct jpeg_decompress_struct cinfo;
	struct tej_error_mgr tejerr;
	struct jpeg_marker_struct *marker;
	FILE *f;
	goffset size;
	gchar *filename;

	filename = g_filename_from_uri (uri, NULL, NULL);

	size = tracker_file_get_size (filename);

	if (size < 18) {
		g_free (filename);
		return;
	}

	f = tracker_file_open (filename, "rb", FALSE);

	if (f) {
		TrackerXmpData xd = { 0 };
		TrackerExifData ed = { 0 };
		TrackerIptcData id = { 0 };
		MergeData md = { 0 };
		gchar *comment = NULL;

		tracker_sparql_builder_subject_iri (metadata, uri);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nfo:Image");
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nmm:Photo");

		cinfo.err = jpeg_std_error (&tejerr.jpeg);
		tejerr.jpeg.error_exit = extract_jpeg_error_exit;
		if (setjmp (tejerr.setjmp_buffer)) {
			goto fail;
		}

		jpeg_create_decompress (&cinfo);

		jpeg_save_markers (&cinfo, JPEG_COM, 0xFFFF);
		jpeg_save_markers (&cinfo, JPEG_APP0 + 1, 0xFFFF);
		jpeg_save_markers (&cinfo, JPEG_APP0 + 13, 0xFFFF);

		jpeg_stdio_src (&cinfo, f);

		jpeg_read_header (&cinfo, TRUE);

		/* FIXME? It is possible that there are markers after SOS,
		 * but there shouldn't be. Should we decompress the whole file?
		 *
		 * jpeg_start_decompress(&cinfo);
		 * jpeg_finish_decompress(&cinfo);
		 *
		 * jpeg_calc_output_dimensions(&cinfo);
		 */

		marker = (struct jpeg_marker_struct *) &cinfo.marker_list;

		while (marker) {
			gchar *str;
			gsize len;
#ifdef HAVE_LIBIPTCDATA
			gsize offset;
			guint sublen;
#endif /* HAVE_LIBIPTCDATA */

			switch (marker->marker) {
			case JPEG_COM:
				g_free (comment);
				comment = g_strndup ((gchar*) marker->data, marker->data_length);
				break;

			case JPEG_APP0 + 1:
				str = (gchar*) marker->data;
				len = marker->data_length;

#ifdef HAVE_LIBEXIF
				if (strncmp (EXIF_NAMESPACE, str, EXIF_NAMESPACE_LENGTH) == 0) {
					tracker_exif_read ((unsigned char*) marker->data,
					                   len,
					                   uri,
					                   &ed);
				}
#endif /* HAVE_LIBEXIF */

#ifdef HAVE_EXEMPI
				if (strncmp (XMP_NAMESPACE, str, XMP_NAMESPACE_LENGTH) == 0) {
					tracker_xmp_read (str + XMP_NAMESPACE_LENGTH,
					                  len - XMP_NAMESPACE_LENGTH,
					                  uri,
					                  &xd);
				}
#endif /* HAVE_EXEMPI */

				break;

			case JPEG_APP0 + 13:
				str = (gchar*) marker->data;
				len = marker->data_length;
#ifdef HAVE_LIBIPTCDATA
				if (strncmp (PS3_NAMESPACE, str, PS3_NAMESPACE_LENGTH) == 0) {
					offset = iptc_jpeg_ps3_find_iptc (str, len, &sublen);
					if (offset > 0) {
						tracker_iptc_read (str + offset,
						                   sublen,
						                   uri,
						                   &id);
					}
				}
#endif /* HAVE_LIBIPTCDATA */

				break;

			default:
				marker = marker->next;
				continue;
			}

			marker = marker->next;
		}

		/* Don't merge if the make is in the model */
		if ((xd.make == NULL || xd.model == NULL) ||
		    (xd.make && xd.model && strstr (xd.model, xd.make) == NULL)) {
			md.camera = tracker_merge (" ", 2, xd.make, xd.model);
		} else {
			md.camera = g_strdup (xd.model);
			g_free (xd.model);
			g_free (xd.make);
		}

		if (!md.camera) {
			if ((ed.make == NULL || ed.model == NULL) ||
			    (ed.make && ed.model && strstr (ed.model, ed.make) == NULL)) {
				md.camera = tracker_merge (" ", 2, ed.make, ed.model);
			} else {
				md.camera = g_strdup (ed.model);
				g_free (ed.model);
				g_free (ed.make);
			}
		} else {
			g_free (ed.model);
			g_free (ed.make);
		}

		md.title = tracker_coalesce (3, xd.title, xd.title2, ed.document_name, xd.pdf_title);
		md.orientation = tracker_coalesce (3, ed.orientation, xd.orientation, id.image_orientation);
		md.copyright = tracker_coalesce (3, ed.copyright, xd.copyright, xd.rights, id.copyright_notice);
		md.white_balance = tracker_coalesce (2, ed.white_balance, xd.white_balance);
		md.fnumber =  tracker_coalesce (2, ed.fnumber, xd.fnumber);
		md.flash =  tracker_coalesce (2, ed.flash, xd.flash);
		md.focal_length =  tracker_coalesce (2, ed.focal_length, xd.focal_length);
		md.artist =  tracker_coalesce (3, ed.artist, xd.artist, xd.contributor);
		md.exposure_time =  tracker_coalesce (2, ed.exposure_time, xd.exposure_time);
		md.iso_speed_ratings =  tracker_coalesce (2, ed.iso_speed_ratings, xd.iso_speed_ratings);
		md.date =  tracker_coalesce (5, ed.time, xd.date, id.date_created, ed.time_original, xd.time_original);
		md.description = tracker_coalesce (2, ed.description, xd.description);
		md.metering_mode = tracker_coalesce (2, ed.metering_mode, xd.metering_mode);

		md.city = tracker_coalesce (2, id.city, xd.city);
		md.state = tracker_coalesce (2, id.state, xd.state);
		md.address = tracker_coalesce (2, id.sublocation, xd.address);
		md.country  = tracker_coalesce (2, id.country_name, xd.country);

		md.creator =  tracker_coalesce (3, id.byline, xd.creator, id.credit);
		md.comment = tracker_coalesce (2, comment, ed.user_comment);

		/* Prioritize on native dimention in all cases */
		tracker_sparql_builder_predicate (metadata, "nfo:width");
		tracker_sparql_builder_object_int64 (metadata, cinfo.image_width);
		g_free (ed.x_dimension);

		tracker_sparql_builder_predicate (metadata, "nfo:height");
		tracker_sparql_builder_object_int64 (metadata, cinfo.image_height);
		g_free (ed.y_dimension);

		if (id.contact) {
			tracker_sparql_builder_predicate (metadata, "nco:representative");
			tracker_sparql_builder_object_blank_open (metadata);
			tracker_sparql_builder_predicate (metadata, "a");
			tracker_sparql_builder_object (metadata, "nco:Contact");
			tracker_sparql_builder_predicate (metadata, "nco:fullname");
			tracker_sparql_builder_object_unvalidated (metadata, id.contact);
			tracker_sparql_builder_object_blank_close (metadata);
			g_free (id.contact);
		}

		if (id.byline_title) {
			tracker_sparql_builder_predicate (metadata, "nco:hasAffiliation");
			tracker_sparql_builder_object_blank_open (metadata);
			tracker_sparql_builder_predicate (metadata, "a");
			tracker_sparql_builder_object (metadata, "nco:Affiliation");
			tracker_sparql_builder_predicate (metadata, "nco:title");
			tracker_sparql_builder_object_unvalidated (metadata, id.byline_title);
			tracker_sparql_builder_object_blank_close (metadata);
			g_free (id.byline_title);
		}

		if (xd.keywords) {
			insert_keywords (metadata, xd.keywords);
			g_free (xd.keywords);
		}

		if (xd.pdf_keywords) {
			insert_keywords (metadata, xd.pdf_keywords);
			g_free (xd.pdf_keywords);
		}

		if (xd.subject) {
			insert_keywords (metadata, xd.subject);
			g_free (xd.subject);
		}

		if (xd.publisher) {
			tracker_sparql_builder_predicate (metadata, "nco:publisher");
			tracker_sparql_builder_object_blank_open (metadata);
			tracker_sparql_builder_predicate (metadata, "a");
			tracker_sparql_builder_object (metadata, "nco:Contact");
			tracker_sparql_builder_predicate (metadata, "nco:fullname");
			tracker_sparql_builder_object_unvalidated (metadata, xd.publisher);
			tracker_sparql_builder_object_blank_close (metadata);
			g_free (xd.publisher);
		}

		if (xd.type) {
			tracker_sparql_builder_predicate (metadata, "dc:type");
			tracker_sparql_builder_object_unvalidated (metadata, xd.type);
			g_free (xd.type);
		}

		if (xd.format) {
			tracker_sparql_builder_predicate (metadata, "dc:format");
			tracker_sparql_builder_object_unvalidated (metadata, xd.format);
			g_free (xd.format);
		}

		if (xd.identifier) {
			tracker_sparql_builder_predicate (metadata, "dc:indentifier");
			tracker_sparql_builder_object_unvalidated (metadata, xd.identifier);
			g_free (xd.identifier);
		}

		if (xd.source) {
			tracker_sparql_builder_predicate (metadata, "dc:source");
			tracker_sparql_builder_object_unvalidated (metadata, xd.source);
			g_free (xd.source);
		}

		if (xd.language) {
			tracker_sparql_builder_predicate (metadata, "dc:language");
			tracker_sparql_builder_object_unvalidated (metadata, xd.language);
			g_free (xd.language);
		}

		if (xd.relation) {
			tracker_sparql_builder_predicate (metadata, "dc:relation");
			tracker_sparql_builder_object_unvalidated (metadata, xd.relation);
			g_free (xd.relation);
		}

		if (xd.coverage) {
			tracker_sparql_builder_predicate (metadata, "dc:coverage");
			tracker_sparql_builder_object_unvalidated (metadata, xd.coverage);
			g_free (xd.coverage);
		}

		if (xd.license) {
			tracker_sparql_builder_predicate (metadata, "nie:license");
			tracker_sparql_builder_object_unvalidated (metadata, xd.license);
			g_free (xd.license);
		}

		if (id.keywords) {
			insert_keywords (metadata, id.keywords);
			g_free (id.keywords);
		}

		if (md.camera) {
			tracker_sparql_builder_predicate (metadata, "nmm:camera");
			tracker_sparql_builder_object_unvalidated (metadata, md.camera);
			g_free (md.camera);
		}

		if (md.title) {
			tracker_sparql_builder_predicate (metadata, "nie:title");
			tracker_sparql_builder_object_unvalidated (metadata, md.title);
			g_free (md.title);
		}

		if (md.orientation) {
			tracker_sparql_builder_predicate (metadata, "nfo:orientation");
			tracker_sparql_builder_object (metadata, md.orientation);
			g_free (md.orientation);
		}

		if (md.copyright) {
			tracker_sparql_builder_predicate (metadata, "nie:copyright");
			tracker_sparql_builder_object_unvalidated (metadata, md.copyright);
			g_free (md.copyright);
		}

		if (md.white_balance) {
			tracker_sparql_builder_predicate (metadata, "nmm:whiteBalance");
			tracker_sparql_builder_object (metadata, md.white_balance);
			g_free (md.white_balance);
		}

		if (md.fnumber) {
			gdouble value;

			value = g_strtod (md.fnumber, NULL);
			tracker_sparql_builder_predicate (metadata, "nmm:fnumber");
			tracker_sparql_builder_object_double (metadata, value);
			g_free (md.fnumber);
		}

		if (md.flash) {
			tracker_sparql_builder_predicate (metadata, "nmm:flash");
			tracker_sparql_builder_object (metadata, md.flash);
			g_free (md.flash);
		}

		if (md.focal_length) {
			gdouble value;

			value = g_strtod (md.focal_length, NULL);
			tracker_sparql_builder_predicate (metadata, "nmm:focalLength");
			tracker_sparql_builder_object_double (metadata, value);
			g_free (md.focal_length);
		}

		if (md.artist) {
			tracker_sparql_builder_predicate (metadata, "nco:contributor");
			tracker_sparql_builder_object_blank_open (metadata);
			tracker_sparql_builder_predicate (metadata, "a");
			tracker_sparql_builder_object (metadata, "nco:Contact");
			tracker_sparql_builder_predicate (metadata, "nco:fullname");
			tracker_sparql_builder_object_unvalidated (metadata, md.artist);
			tracker_sparql_builder_object_blank_close (metadata);
			g_free (md.artist);
		}

		if (md.exposure_time) {
			gdouble value;

			value = g_strtod (md.exposure_time, NULL);
			tracker_sparql_builder_predicate (metadata, "nmm:exposureTime");
			tracker_sparql_builder_object_double (metadata, value);
			g_free (md.exposure_time);
		}

		if (md.iso_speed_ratings) {
			gdouble value;

			value = g_strtod (md.iso_speed_ratings, NULL);
			tracker_sparql_builder_predicate (metadata, "nmm:isoSpeed");
			tracker_sparql_builder_object_double (metadata, value);
			g_free (md.iso_speed_ratings);
		}

		if (md.date) {
			tracker_sparql_builder_predicate (metadata, "nie:contentCreated");
			tracker_sparql_builder_object_unvalidated (metadata, md.date);
			g_free (md.date);
		}

		if (md.description) {
			tracker_sparql_builder_predicate (metadata, "nie:description");
			tracker_sparql_builder_object_unvalidated (metadata, md.description);
			g_free (md.description);
		}

		if (md.metering_mode) {
			tracker_sparql_builder_predicate (metadata, "nmm:meteringMode");
			tracker_sparql_builder_object (metadata, md.metering_mode);
			g_free (md.metering_mode);
		}

		if (md.creator) {
			tracker_sparql_builder_predicate (metadata, "nco:creator");
			tracker_sparql_builder_object_blank_open (metadata);
			tracker_sparql_builder_predicate (metadata, "a");
			tracker_sparql_builder_object (metadata, "nco:Contact");
			tracker_sparql_builder_predicate (metadata, "nco:fullname");
			tracker_sparql_builder_object_unvalidated (metadata, md.creator);
			tracker_sparql_builder_object_blank_close (metadata);
			g_free (md.creator);
		}

		if (md.comment) {
			tracker_sparql_builder_predicate (metadata, "nie:comment");
			tracker_sparql_builder_object_unvalidated (metadata, md.comment);
			g_free (md.comment);
		}

		if (md.city || md.state || md.address || md.country) {
			tracker_sparql_builder_predicate (metadata, "mlo:location");

			tracker_sparql_builder_object_blank_open (metadata);
			tracker_sparql_builder_predicate (metadata, "a");
			tracker_sparql_builder_object (metadata, "mlo:GeoPoint");
	
			if (md.address) {
				tracker_sparql_builder_predicate (metadata, "mlo:address");
				tracker_sparql_builder_object_unvalidated (metadata, md.address);
				g_free (md.address);
			}
	
			if (md.state) {
				tracker_sparql_builder_predicate (metadata, "mlo:state");
				tracker_sparql_builder_object_unvalidated (metadata, md.state);
				g_free (md.state);
			}
	
			if (md.city) {
				tracker_sparql_builder_predicate (metadata, "mlo:city");
				tracker_sparql_builder_object_unvalidated (metadata, md.city);
				g_free (md.city);
			}
	
			if (md.country) {
				tracker_sparql_builder_predicate (metadata, "mlo:country");
				tracker_sparql_builder_object_unvalidated (metadata, md.country);
				g_free (md.country);
			}
		
			tracker_sparql_builder_object_blank_close (metadata);
		}

		jpeg_destroy_decompress (&cinfo);
	fail:
		tracker_file_close (f, FALSE);
	}

	g_free (filename);
}

TrackerExtractData *
tracker_get_extract_data (void)
{
	return data;
}
