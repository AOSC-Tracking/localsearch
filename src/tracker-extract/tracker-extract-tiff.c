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

#include <ctype.h>
#include <string.h>
#include <fcntl.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <tiff.h>
#include <tiffio.h>

#include <libtracker-common/tracker-ontology.h>
#include <libtracker-common/tracker-type-utils.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-utils.h>

#include "tracker-main.h"
#include "tracker-xmp.h"
#include "tracker-iptc.h"
#include "tracker-exif.h"

#define EXIF_DATE_FORMAT        "%Y:%m:%d %H:%M:%S"

#define NMM_PREFIX TRACKER_NMM_PREFIX
#define NFO_PREFIX TRACKER_NFO_PREFIX
#define NIE_PREFIX TRACKER_NIE_PREFIX
#define DC_PREFIX TRACKER_DC_PREFIX
#define NCO_PREFIX TRACKER_NCO_PREFIX
#define RDF_PREFIX TRACKER_RDF_PREFIX

typedef enum {
	TIFF_TAGTYPE_UNDEFINED = 0,
	TIFF_TAGTYPE_STRING,
	TIFF_TAGTYPE_UINT16,
	TIFF_TAGTYPE_UINT32,
	TIFF_TAGTYPE_DOUBLE,
	TIFF_TAGTYPE_C16_UINT16
} TagType;

typedef struct {
	gchar *camera, *title, *orientation, *copyright, *white_balance,
		*fnumber, *flash, *focal_length, *artist,
		*exposure_time, *iso_speed_ratings, *date, *description,
		*metering_mode, *creator, *x_dimension, *y_dimension;
} TiffNeedsMergeData;

typedef struct {
	gchar *artist, *copyright, *datetime, *documentname, *imagedescription,
		*imagewidth, *imagelength, *make, *model, *orientation;
} TiffData;

static void extract_tiff (const gchar *filename,
                          TrackerSparqlBuilder   *metadata);

static TrackerExtractData extract_data[] = {
	{ "image/tiff", extract_tiff },
	{ NULL, NULL }
};

static gchar *
get_flash (TIFF *image)
{
	guint16 varui16 = 0;

	if (TIFFGetField (image, EXIFTAG_FLASH, &varui16)) {
		switch (varui16) {
		case 0x0001:
		case 0x0009:
		case 0x000D:
		case 0x000F:
		case 0x0019:
		case 0x001D:
		case 0x001F:
		case 0x0041:
		case 0x0045:
		case 0x0047:
		case 0x0049:
		case 0x004D:
		case 0x004F:
		case 0x0059:
		case 0x005F:
		case 0x005D:
			return g_strdup ("nmm:flash-on");
		default:
			return g_strdup ("nmm:flash-off");
		}
	}

	return NULL;
}

static gchar *
get_orientation (TIFF *image)
{
	guint16 varui16 = 0;

	if (TIFFGetField (image, TIFFTAG_ORIENTATION, &varui16)) {
		switch (varui16) {
		default:
		case 0:
			return  g_strdup ("nfo:orientation-top");
		case 1:
			return  g_strdup ("nfo:orientation-top-mirror");
		case 2:
			return  g_strdup ("nfo:orientation-bottom");
		case 3:
			return  g_strdup ("nfo:orientation-bottom-mirror");
		case 4:
			return  g_strdup ("nfo:orientation-left-mirror");
		case 5:
			return  g_strdup ("nfo:orientation-right");
		case 6:
			return  g_strdup ("nfo:orientation-right-mirror");
		case 7:
			return  g_strdup ("nfo:orientation-left");
		}
	}

	return NULL;
}


static gchar *
get_metering_mode (TIFF *image)
{
	guint16 varui16 = 0;

	if (TIFFGetField (image, EXIFTAG_METERINGMODE, &varui16)) {
		switch (varui16) {
		case 1:
			return g_strdup ("nmm:meteringMode-average");
		case 2:
			return g_strdup ("nmm:meteringMode-center-weighted-average");
		case 3:
			return g_strdup ("nmm:meteringMode-spot");
		case 4:
			return g_strdup ("nmm:meteringMode-multispot");
		case 5:
			return g_strdup ("nmm:meteringMode-pattern");
		case 6:
			return g_strdup ("nmm:meteringMode-partial");
		default:
			return g_strdup ("nmm:meteringMode-other");
		}
	}

	return NULL;
}



static gchar *
get_white_balance (TIFF *image)
{
	guint16 varui16 = 0;

	if (TIFFGetField (image, EXIFTAG_WHITEBALANCE, &varui16)) {
		if (varui16 == 0) {
			return g_strdup ("nmm:whiteBalance-auto");
		}
		return g_strdup ("nmm:whiteBalance-manual");
	}

	return NULL;
}


static gchar *
get_value (TIFF *image, guint tag, guint type)
{
	guint16 count16 = 0;
	gfloat vardouble = 0;
	guint16 varui16 = 0;
	guint32 varui32 = 0;
	gchar *text = NULL;
	void *data = NULL;

	switch (type) {
	case TIFF_TAGTYPE_STRING:
		if (TIFFGetField (image, tag, &text)) {
			return g_strdup (text);
		}
		break;
	case TIFF_TAGTYPE_UINT16:
		if (TIFFGetField (image, tag, &varui16)) {
			return g_strdup_printf ("%i", varui16);
		}
		break;
	case TIFF_TAGTYPE_UINT32:
		if (TIFFGetField (image, tag, &varui32)) {
			return g_strdup_printf ("%i", varui32);
		}
		break;
	case TIFF_TAGTYPE_DOUBLE:
		if (TIFFGetField (image, tag, &vardouble)) {
			return g_strdup_printf ("%f", vardouble);
		}
		break;
	case TIFF_TAGTYPE_C16_UINT16:
		if (TIFFGetField (image, tag, &count16, &data)) {
			return g_strdup_printf ("%i", * (guint16 *) data);
		}
		break;
	default:
		break;
	}

	return NULL;
}


static void
insert_keywords (TrackerSparqlBuilder *metadata, const gchar *uri, gchar *keywords)
{
	char *lasts, *keyw;
	size_t len;

	keyw = keywords;
	keywords = strchr (keywords, '"');
	if (keywords)
		keywords++;
	else
		keywords = keyw;

	len = strlen (keywords);
	if (keywords[len - 1] == '"')
		keywords[len - 1] = '\0';

	for (keyw = strtok_r (keywords, ",; ", &lasts); keyw;
	     keyw = strtok_r (NULL, ",; ", &lasts)) {
		tracker_sparql_builder_predicate (metadata, "nao:hasTag");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nao:Tag");

		tracker_sparql_builder_predicate (metadata, "nao:prefLabel");
		tracker_sparql_builder_object_unvalidated (metadata, keyw);

		tracker_sparql_builder_object_blank_close (metadata);
	}
}

static void
extract_tiff (const gchar *uri, TrackerSparqlBuilder *metadata)
{
	TIFF *image;
	glong exifOffset;
	gchar *filename = g_filename_from_uri (uri, NULL, NULL);
	TrackerXmpData xmp_data = { 0 };
	TrackerIptcData iptc_data = { 0 };
	TrackerExifData exif_data = { 0 };
	TiffNeedsMergeData merge_data = { 0 };
	TiffData tiff_data = { 0 };

#ifdef HAVE_LIBIPTCDATA
	gchar   *iptcOffset;
	guint32  iptcSize;
#endif

#ifdef HAVE_EXEMPI
	gchar *xmpOffset;
	guint32 size;
#endif /* HAVE_EXEMPI */

	if ((image = TIFFOpen (filename, "r")) == NULL){
		g_warning ("Could not open image:'%s'\n", filename);
		g_free (filename);
		return;
	}

	tracker_sparql_builder_subject_iri (metadata, uri);
	tracker_sparql_builder_predicate (metadata, "a");
	tracker_sparql_builder_object (metadata, "nfo:Image");
	tracker_sparql_builder_object (metadata, "nmm:Photo");

#ifdef HAVE_LIBIPTCDATA
	if (TIFFGetField (image, TIFFTAG_RICHTIFFIPTC, &iptcSize, &iptcOffset)) {
		if (TIFFIsByteSwapped(image) != 0)
			TIFFSwabArrayOfLong((uint32 *) iptcOffset,(unsigned long) iptcSize);
		tracker_read_iptc (iptcOffset,
		                   4*iptcSize,
		                   uri, &iptc_data);
	}
#endif /* HAVE_LIBIPTCDATA */

	/* FIXME There are problems between XMP data embedded with different tools
	   due to bugs in the original spec (type) */
#ifdef HAVE_EXEMPI
	if (TIFFGetField (image, TIFFTAG_XMLPACKET, &size, &xmpOffset)) {
		tracker_read_xmp (xmpOffset,
		                  size,
		                  uri,
		                  &xmp_data);
	}
#endif /* HAVE_EXEMPI */

	if (!tiff_data.artist)
		tiff_data.artist = get_value (image, TIFFTAG_ARTIST, TIFF_TAGTYPE_STRING);
	if (!tiff_data.copyright)
		tiff_data.copyright = get_value (image, TIFFTAG_COPYRIGHT, TIFF_TAGTYPE_STRING);
	if (!tiff_data.datetime)
		tiff_data.datetime = get_value (image, TIFFTAG_DATETIME, TIFF_TAGTYPE_STRING);
	if (!tiff_data.documentname)
		tiff_data.documentname = get_value (image, TIFFTAG_DOCUMENTNAME, TIFF_TAGTYPE_STRING);
	if (!tiff_data.imagedescription)
		tiff_data.imagedescription = get_value (image, TIFFTAG_IMAGEDESCRIPTION, TIFF_TAGTYPE_STRING);
	if (!tiff_data.make)
		tiff_data.make = get_value (image, TIFFTAG_MAKE, TIFF_TAGTYPE_STRING);
	if (!tiff_data.model)
		tiff_data.model = get_value (image, TIFFTAG_MODEL, TIFF_TAGTYPE_STRING);
	if (!tiff_data.orientation)
		tiff_data.orientation = get_orientation (image);

	if (TIFFGetField (image, TIFFTAG_EXIFIFD, &exifOffset)) {
		if (TIFFReadEXIFDirectory (image, exifOffset)) {
			if (!exif_data.exposure_time)
				exif_data.exposure_time = get_value (image, EXIFTAG_EXPOSURETIME, TIFF_TAGTYPE_DOUBLE);
			if (!exif_data.fnumber)
				exif_data.fnumber = get_value (image, EXIFTAG_FNUMBER, TIFF_TAGTYPE_DOUBLE);
			if (!exif_data.iso_speed_ratings)
				exif_data.iso_speed_ratings = get_value (image, EXIFTAG_ISOSPEEDRATINGS, TIFF_TAGTYPE_C16_UINT16);
			if (!exif_data.time_original)
				exif_data.time_original = get_value (image, EXIFTAG_DATETIMEORIGINAL, TIFF_TAGTYPE_STRING);
			if (!exif_data.metering_mode)
				exif_data.metering_mode = get_metering_mode (image);
			if (!exif_data.flash)
				exif_data.flash = get_flash (image);
			if (!exif_data.focal_length)
				exif_data.focal_length = get_value (image, EXIFTAG_DATETIMEORIGINAL, TIFF_TAGTYPE_DOUBLE);
			if (!exif_data.white_balance)
				exif_data.white_balance = get_white_balance (image);
		}
	}

	TIFFClose (image);
	g_free (filename);

	merge_data.camera = tracker_merge (" ", 2, tiff_data.make,
	                                   tiff_data.model);

	if (!merge_data.camera) {
		merge_data.camera = tracker_merge (" ", 2, xmp_data.Make,
		                                   xmp_data.Model);

		if (!merge_data.camera) {
			merge_data.camera = tracker_merge (" ", 2, exif_data.make,
			                                   exif_data.model);
		} else {
			g_free (exif_data.model);
			g_free (exif_data.make);
		}
	} else {
		g_free (xmp_data.Model);
		g_free (xmp_data.Make);
		g_free (exif_data.model);
		g_free (exif_data.make);
	}

	merge_data.title = tracker_coalesce (3, tiff_data.documentname,
	                                     xmp_data.title, xmp_data.Title,
	                                     exif_data.document_name);

	merge_data.orientation = tracker_coalesce (4, tiff_data.orientation,
	                                           exif_data.orientation,
	                                           xmp_data.Orientation,
	                                           iptc_data.image_orientation);

	merge_data.copyright = tracker_coalesce (4, tiff_data.copyright,
	                                         exif_data.copyright,
	                                         xmp_data.rights,
	                                         iptc_data.copyright_notice);

	merge_data.white_balance = tracker_coalesce (2, exif_data.white_balance,
	                                             xmp_data.WhiteBalance);


	merge_data.fnumber =  tracker_coalesce (2, exif_data.fnumber,
	                                        xmp_data.FNumber);

	merge_data.flash =  tracker_coalesce (2, exif_data.flash,
	                                      xmp_data.Flash);

	merge_data.focal_length =  tracker_coalesce (2, exif_data.focal_length,
	                                             xmp_data.FocalLength);

	merge_data.artist =  tracker_coalesce (4, tiff_data.artist,
	                                       exif_data.artist,
	                                       xmp_data.Artist,
	                                       xmp_data.contributor);

	merge_data.exposure_time =  tracker_coalesce (2, exif_data.exposure_time,
	                                              xmp_data.ExposureTime);

	merge_data.iso_speed_ratings =  tracker_coalesce (2, exif_data.iso_speed_ratings,
	                                                  xmp_data.ISOSpeedRatings);

	merge_data.date =  tracker_coalesce (6, tiff_data.datetime,
	                                     exif_data.time,
	                                     xmp_data.date,
	                                     iptc_data.date_created,
	                                     exif_data.time_original,
	                                     xmp_data.DateTimeOriginal);

	merge_data.description = tracker_coalesce (3, tiff_data.imagedescription,
	                                           exif_data.description,
	                                           xmp_data.description);

	merge_data.metering_mode =  tracker_coalesce (2, exif_data.metering_mode,
	                                              xmp_data.MeteringMode);

	merge_data.creator =  tracker_coalesce (3, iptc_data.byline,
	                                        xmp_data.creator,
	                                        iptc_data.credit);

	merge_data.x_dimension =  tracker_coalesce (2, tiff_data.imagewidth,
	                                            exif_data.x_dimension);
	merge_data.y_dimension =  tracker_coalesce (2, tiff_data.imagelength,
	                                            exif_data.y_dimension);

	if (exif_data.user_comment) {
		tracker_sparql_builder_predicate (metadata, "nie:comment");
		tracker_sparql_builder_object_unvalidated (metadata, exif_data.user_comment);
		g_free (exif_data.user_comment);
	}

	if (merge_data.x_dimension) {
		tracker_sparql_builder_predicate (metadata, "nfo:width");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.x_dimension);
		g_free (merge_data.x_dimension);
	}

	if (merge_data.y_dimension) {
		tracker_sparql_builder_predicate (metadata, "nfo:height");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.y_dimension);
		g_free (merge_data.y_dimension);
	}

	if (xmp_data.keywords) {
		insert_keywords (metadata, uri, xmp_data.keywords);
		g_free (xmp_data.keywords);
	}

	if (xmp_data.subject) {
		insert_keywords (metadata, uri, xmp_data.subject);
		g_free (xmp_data.subject);
	}

	if (xmp_data.publisher) {
		tracker_sparql_builder_predicate (metadata, "nco:publisher");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nco:Contact");

		tracker_sparql_builder_predicate (metadata, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.publisher);
		tracker_sparql_builder_object_blank_close (metadata);
		g_free (xmp_data.publisher);
	}

	if (xmp_data.type) {
		tracker_sparql_builder_predicate (metadata, "dc:type");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.type);
		g_free (xmp_data.type);
	}

	if (xmp_data.format) {
		tracker_sparql_builder_predicate (metadata, "dc:format");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.format);
		g_free (xmp_data.format);
	}

	if (xmp_data.identifier) {
		tracker_sparql_builder_predicate (metadata, "dc:identifier");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.identifier);
		g_free (xmp_data.identifier);
	}

	if (xmp_data.source) {
		tracker_sparql_builder_predicate (metadata, "dc:source");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.source);
		g_free (xmp_data.source);
	}

	if (xmp_data.language) {
		tracker_sparql_builder_predicate (metadata, "dc:language");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.language);
		g_free (xmp_data.language);
	}

	if (xmp_data.relation) {
		tracker_sparql_builder_predicate (metadata, "dc:relation");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.relation);
		g_free (xmp_data.relation);
	}

	if (xmp_data.coverage) {
		tracker_sparql_builder_predicate (metadata, "dc:coverage");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.coverage);
		g_free (xmp_data.coverage);
	}

	if (xmp_data.license) {
		tracker_sparql_builder_predicate (metadata, "nie:license");
		tracker_sparql_builder_object_unvalidated (metadata, xmp_data.license);
		g_free (xmp_data.license);
	}

	if (xmp_data.Address || xmp_data.Country || xmp_data.City) {
		tracker_sparql_builder_predicate (metadata, "mlo:location");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "mlo:GeoPoint");

		if (xmp_data.Address) {
			tracker_sparql_builder_predicate (metadata, "mlo:address");
			tracker_sparql_builder_object_unvalidated (metadata, xmp_data.Address);
			g_free (xmp_data.Address);
		}

		if (xmp_data.State) {
			tracker_sparql_builder_predicate (metadata, "mlo:state");
			tracker_sparql_builder_object_unvalidated (metadata, xmp_data.State);
			g_free (xmp_data.State);
		}

		if (xmp_data.City) {
			tracker_sparql_builder_predicate (metadata, "mlo:city");
			tracker_sparql_builder_object_unvalidated (metadata, xmp_data.City);
			g_free (xmp_data.City);
		}

		if (xmp_data.Country) {
			tracker_sparql_builder_predicate (metadata, "mlo:country");
			tracker_sparql_builder_object_unvalidated (metadata, xmp_data.Country);
			g_free (xmp_data.Country);
		}
	
		tracker_sparql_builder_object_blank_close (metadata);
	}

	if (iptc_data.keywords) {
		insert_keywords (metadata, uri, iptc_data.keywords);
		g_free (iptc_data.keywords);
	}

	if (merge_data.camera) {
		tracker_sparql_builder_predicate (metadata, "nmm:camera");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.camera);
		g_free (merge_data.camera);
	}

	if (merge_data.title) {
		tracker_sparql_builder_predicate (metadata, "nie:title");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.title);
		g_free (merge_data.title);
	}

	if (merge_data.orientation) {
		tracker_sparql_builder_predicate (metadata, "nfo:orientation");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.orientation);
		g_free (merge_data.orientation);
	}

	if (merge_data.copyright) {
		tracker_sparql_builder_predicate (metadata, "nie:copyright");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.copyright);
		g_free (merge_data.copyright);
	}

	if (merge_data.white_balance) {
		tracker_sparql_builder_predicate (metadata, "nmm:whiteBalance");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.white_balance);
		g_free (merge_data.white_balance);
	}

	if (merge_data.fnumber) {
		tracker_sparql_builder_predicate (metadata, "nmm:fnumber");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.fnumber);
		g_free (merge_data.fnumber);
	}

	if (merge_data.flash) {
		tracker_sparql_builder_predicate (metadata, "nmm:flash");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.flash);
		g_free (merge_data.flash);
	}

	if (merge_data.focal_length) {
		tracker_sparql_builder_predicate (metadata, "nmm:focalLength");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.focal_length);
		g_free (merge_data.focal_length);
	}

	if (merge_data.artist) {
		tracker_sparql_builder_predicate (metadata, "nco:contributor");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nco:Contact");

		tracker_sparql_builder_predicate (metadata, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.artist);
		tracker_sparql_builder_object_blank_close (metadata);
		g_free (merge_data.artist);
	}

	if (merge_data.exposure_time) {
		tracker_sparql_builder_predicate (metadata, "nmm:exposureTime");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.exposure_time);
		g_free (merge_data.exposure_time);
	}

	if (merge_data.iso_speed_ratings) {
		tracker_sparql_builder_predicate (metadata, "nmm:isoSpeed");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.iso_speed_ratings);
		g_free (merge_data.iso_speed_ratings);
	}

	if (merge_data.date) {
		tracker_sparql_builder_predicate (metadata, "nie:contentCreated");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.date);
		g_free (merge_data.date);
	}

	if (merge_data.description) {
		tracker_sparql_builder_predicate (metadata, "nie:description");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.description);
		g_free (merge_data.description);
	}

	if (merge_data.metering_mode) {
		tracker_sparql_builder_predicate (metadata, "nmm:meteringMode");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.metering_mode);
		g_free (merge_data.metering_mode);
	}

	if (merge_data.creator) {
		tracker_sparql_builder_predicate (metadata, "nco:creator");

		tracker_sparql_builder_object_blank_open (metadata);
		tracker_sparql_builder_predicate (metadata, "a");
		tracker_sparql_builder_object (metadata, "nco:Contact");

		tracker_sparql_builder_predicate (metadata, "nco:fullname");
		tracker_sparql_builder_object_unvalidated (metadata, merge_data.creator);
		tracker_sparql_builder_object_blank_close (metadata);
		g_free (merge_data.creator);
	}
}

TrackerExtractData *
tracker_get_extract_data (void)
{
	return extract_data;
}
