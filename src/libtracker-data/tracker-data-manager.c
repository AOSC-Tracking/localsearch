/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2007, Jason Kivlighn (jkivlighn@gmail.com)
 * Copyright (C) 2007, Creative Commons (http://creativecommons.org)
 * Copyright (C) 2008, Nokia
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

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <zlib.h>
#include <inttypes.h>

#include <glib/gstdio.h>

#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-parser.h>
#include <libtracker-common/tracker-type-utils.h>
#include <libtracker-common/tracker-utils.h>
#include <libtracker-common/tracker-ontology.h>

#include <tracker-fts/tracker-fts.h>

#include <libtracker-db/tracker-db-interface-sqlite.h>
#include <libtracker-db/tracker-db-manager.h>

#include "tracker-data-manager.h"
#include "tracker-data-update.h"
#include "tracker-sparql-query.h"

#define RDF_PREFIX TRACKER_RDF_PREFIX
#define RDF_PROPERTY RDF_PREFIX "Property"
#define RDF_TYPE RDF_PREFIX "type"

#define RDFS_PREFIX TRACKER_RDFS_PREFIX
#define RDFS_CLASS RDFS_PREFIX "Class"
#define RDFS_DOMAIN RDFS_PREFIX "domain"
#define RDFS_RANGE RDFS_PREFIX "range"
#define RDFS_SUB_CLASS_OF RDFS_PREFIX "subClassOf"
#define RDFS_SUB_PROPERTY_OF RDFS_PREFIX "subPropertyOf"

#define NRL_PREFIX TRACKER_NRL_PREFIX
#define NRL_MAX_CARDINALITY NRL_PREFIX "maxCardinality"

#define TRACKER_PREFIX TRACKER_TRACKER_PREFIX

#define ZLIBBUFSIZ 8192

static gchar		  *ontologies_dir;
static gboolean            initialized;

static void
load_ontology_file_from_path (const gchar	 *ontology_file)
{
	TrackerTurtleReader *reader;
	GError              *error = NULL;

	reader = tracker_turtle_reader_new (ontology_file);
	while (error == NULL && tracker_turtle_reader_next (reader, &error)) {
		const gchar *subject, *predicate, *object;

		subject = tracker_turtle_reader_get_subject (reader);
		predicate = tracker_turtle_reader_get_predicate (reader);
		object = tracker_turtle_reader_get_object (reader);

		if (g_strcmp0 (predicate, RDF_TYPE) == 0) {
			if (g_strcmp0 (object, RDFS_CLASS) == 0) {
				TrackerClass *class;

				if (tracker_ontology_get_class_by_uri (subject) != NULL) {
					g_critical ("%s: Duplicate definition of class %s", ontology_file, subject);
					continue;
				}

				class = tracker_class_new ();
				tracker_class_set_uri (class, subject);
				tracker_ontology_add_class (class);
				g_object_unref (class);
			} else if (g_strcmp0 (object, RDF_PROPERTY) == 0) {
				TrackerProperty *property;

				if (tracker_ontology_get_property_by_uri (subject) != NULL) {
					g_critical ("%s: Duplicate definition of property %s", ontology_file, subject);
					continue;
				}

				property = tracker_property_new ();
				tracker_property_set_uri (property, subject);
				tracker_ontology_add_property (property);
				g_object_unref (property);
			} else if (g_strcmp0 (object, TRACKER_PREFIX "Namespace") == 0) {
				TrackerNamespace *namespace;

				if (tracker_ontology_get_namespace_by_uri (subject) != NULL) {
					g_critical ("%s: Duplicate definition of namespace %s", ontology_file, subject);
					continue;
				}

				namespace = tracker_namespace_new ();
				tracker_namespace_set_uri (namespace, subject);
				tracker_ontology_add_namespace (namespace);
				g_object_unref (namespace);
			}
		} else if (g_strcmp0 (predicate, RDFS_SUB_CLASS_OF) == 0) {
			TrackerClass *class, *super_class;

			class = tracker_ontology_get_class_by_uri (subject);
			if (class == NULL) {
				g_critical ("%s: Unknown class %s", ontology_file, subject);
				continue;
			}

			super_class = tracker_ontology_get_class_by_uri (object);
			if (super_class == NULL) {
				g_critical ("%s: Unknown class %s", ontology_file, object);
				continue;
			}

			tracker_class_add_super_class (class, super_class);
		} else if (g_strcmp0 (predicate, RDFS_SUB_PROPERTY_OF) == 0) {
			TrackerProperty *property, *super_property;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			super_property = tracker_ontology_get_property_by_uri (object);
			if (super_property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, object);
				continue;
			}

			tracker_property_add_super_property (property, super_property);
		} else if (g_strcmp0 (predicate, RDFS_DOMAIN) == 0) {
			TrackerProperty *property;
			TrackerClass *domain;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			domain = tracker_ontology_get_class_by_uri (object);
			if (domain == NULL) {
				g_critical ("%s: Unknown class %s", ontology_file, object);
				continue;
			}

			tracker_property_set_domain (property, domain);
		} else if (g_strcmp0 (predicate, RDFS_RANGE) == 0) {
			TrackerProperty *property;
			TrackerClass *range;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			range = tracker_ontology_get_class_by_uri (object);
			if (range == NULL) {
				g_critical ("%s: Unknown class %s", ontology_file, object);
				continue;
			}

			tracker_property_set_range (property, range);
		} else if (g_strcmp0 (predicate, NRL_MAX_CARDINALITY) == 0) {
			TrackerProperty *property;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			if (atoi (object) == 1) {
				tracker_property_set_multiple_values (property, FALSE);
			}
		} else if (g_strcmp0 (predicate, TRACKER_PREFIX "indexed") == 0) {
			TrackerProperty *property;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			if (strcmp (object, "true") == 0) {
				tracker_property_set_indexed (property, TRUE);
			}
		} else if (g_strcmp0 (predicate, TRACKER_PREFIX "transient") == 0) {
			TrackerProperty *property;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			if (g_strcmp0 (object, "true") == 0) {
				tracker_property_set_transient (property, TRUE);
			}
		} else if (g_strcmp0 (predicate, TRACKER_PREFIX "fulltextIndexed") == 0) {
			TrackerProperty *property;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				continue;
			}

			if (strcmp (object, "true") == 0) {
				tracker_property_set_fulltext_indexed (property, TRUE);
			}
		} else if (g_strcmp0 (predicate, TRACKER_PREFIX "prefix") == 0) {
			TrackerNamespace *namespace;

			namespace = tracker_ontology_get_namespace_by_uri (subject);
			if (namespace == NULL) {
				g_critical ("%s: Unknown namespace %s", ontology_file, subject);
				continue;
			}

			tracker_namespace_set_prefix (namespace, object);
		}
	}

	g_object_unref (reader);

	if (error) {
		g_critical ("Turtle parse error: %s", error->message);
		g_error_free (error);
	}
}

static void
load_ontology_file (const gchar	      *filename)
{
	gchar		*ontology_file;

	ontology_file = g_build_filename (ontologies_dir, filename, NULL);
	load_ontology_file_from_path (ontology_file);
	g_free (ontology_file);
}

static void
import_ontology_file (const gchar	      *filename)
{
	gchar		*ontology_file;
	GError          *error = NULL;

	ontology_file = g_build_filename (ontologies_dir, filename, NULL);
	tracker_turtle_reader_load (ontology_file, &error);
	g_free (ontology_file);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}
}

static void
class_add_super_classes_from_db (TrackerDBInterface *iface, TrackerClass *class)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;

	stmt = tracker_db_interface_create_statement (iface,
						      "SELECT (SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"rdfs:subClassOf\") "
						      "FROM \"rdfs:Class_rdfs:subClassOf\" "
						      "WHERE ID = (SELECT ID FROM \"rdfs:Resource\" WHERE Uri = ?)");
	tracker_db_statement_bind_text (stmt, 0, tracker_class_get_uri (class));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerClass *super_class;
			const gchar *super_class_uri;

			super_class_uri = tracker_db_cursor_get_string (cursor, 0);
			super_class = tracker_ontology_get_class_by_uri (super_class_uri);
			tracker_class_add_super_class (class, super_class);
		}

		g_object_unref (cursor);
	}
}

static void
property_add_super_properties_from_db (TrackerDBInterface *iface, TrackerProperty *property)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;

	stmt = tracker_db_interface_create_statement (iface,
						      "SELECT (SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"rdfs:subPropertyOf\") "
						      "FROM \"rdf:Property_rdfs:subPropertyOf\" "
						      "WHERE ID = (SELECT ID FROM \"rdfs:Resource\" WHERE Uri = ?)");
	tracker_db_statement_bind_text (stmt, 0, tracker_property_get_uri (property));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerProperty *super_property;
			const gchar *super_property_uri;

			super_property_uri = tracker_db_cursor_get_string (cursor, 0);
			super_property = tracker_ontology_get_property_by_uri (super_property_uri);
			tracker_property_add_super_property (property, super_property);
		}

		g_object_unref (cursor);
	}
}

static void
db_get_static_data (TrackerDBInterface *iface)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;
	TrackerDBResultSet *result_set;

	stmt = tracker_db_interface_create_statement (iface,
						      "SELECT (SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"tracker:Namespace\".ID), "
						      "\"tracker:prefix\" "
						      "FROM \"tracker:Namespace\"");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerNamespace *namespace;
			const gchar      *uri, *prefix;

			namespace = tracker_namespace_new ();

			uri = tracker_db_cursor_get_string (cursor, 0);
			prefix = tracker_db_cursor_get_string (cursor, 1);

			tracker_namespace_set_uri (namespace, uri);
			tracker_namespace_set_prefix (namespace, prefix);
			tracker_ontology_add_namespace (namespace);

			g_object_unref (namespace);

		}

		g_object_unref (cursor);
	}

	stmt = tracker_db_interface_create_statement (iface,
						      "SELECT (SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"rdfs:Class\".ID) "
						      "FROM \"rdfs:Class\" ORDER BY ID");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerClass *class;
			const gchar  *uri;
			gint          count;

			class = tracker_class_new ();

			uri = tracker_db_cursor_get_string (cursor, 0);

			tracker_class_set_uri (class, uri);
			class_add_super_classes_from_db (iface, class);
			tracker_ontology_add_class (class);

			/* xsd classes do not derive from rdfs:Resource and do not use separate tables */
			if (!g_str_has_prefix (tracker_class_get_name (class), "xsd:")) {
				/* update statistics */
				stmt = tracker_db_interface_create_statement (iface, "SELECT COUNT(1) FROM \"%s\"", tracker_class_get_name (class));
				result_set = tracker_db_statement_execute (stmt, NULL);
				tracker_db_result_set_get (result_set, 0, &count, -1);
				tracker_class_set_count (class, count);
				g_object_unref (result_set);
				g_object_unref (stmt);
			}

			g_object_unref (class);
		}

		g_object_unref (cursor);
	}

	stmt = tracker_db_interface_create_statement (iface,
						      "SELECT (SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"rdf:Property\".ID), "
						      "(SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"rdfs:domain\"), "
						      "(SELECT Uri FROM \"rdfs:Resource\" WHERE ID = \"rdfs:range\"), "
						      "\"nrl:maxCardinality\", "
						      "\"tracker:indexed\", "
						      "\"tracker:fulltextIndexed\", "
						      "\"tracker:transient\" "
						      "FROM \"rdf:Property\" ORDER BY ID");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {

		while (tracker_db_cursor_iter_next (cursor)) {
			GValue value = { 0 };
			TrackerProperty *property;
			const gchar     *uri, *domain_uri, *range_uri;
			gboolean         multi_valued, indexed, fulltext_indexed;
			gboolean         transient = FALSE;

			property = tracker_property_new ();

			uri = tracker_db_cursor_get_string (cursor, 0);
			domain_uri = tracker_db_cursor_get_string (cursor, 1);
			range_uri = tracker_db_cursor_get_string (cursor, 2);

			tracker_db_cursor_get_value (cursor, 3, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				multi_valued = (g_value_get_int (&value) > 1);
				g_value_unset (&value);
			} else {
				/* nrl:maxCardinality not set
				   not limited to single value */
				multi_valued = TRUE;
			}

			tracker_db_cursor_get_value (cursor, 4, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				indexed = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				indexed = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 5, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				fulltext_indexed = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				fulltext_indexed = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 6, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				transient = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				transient = FALSE;
			}

			tracker_property_set_transient (property, transient);
			tracker_property_set_uri (property, uri);
			tracker_property_set_domain (property, tracker_ontology_get_class_by_uri (domain_uri));
			tracker_property_set_range (property, tracker_ontology_get_class_by_uri (range_uri));
			tracker_property_set_multiple_values (property, multi_valued);
			tracker_property_set_indexed (property, indexed);
			tracker_property_set_fulltext_indexed (property, fulltext_indexed);
			property_add_super_properties_from_db (iface, property);
			tracker_ontology_add_property (property);

			g_object_unref (property);

		}

		g_object_unref (cursor);
	}
}

static void
create_decomposed_metadata_property_table (TrackerDBInterface *iface, 
					   TrackerProperty   **property, 
					   const gchar        *service_name,
					   const gchar       **sql_type_for_single_value)
{
	const char *field_name;
	const char *sql_type;
	gboolean    transient;

	field_name = tracker_property_get_name (*property);

	transient = !sql_type_for_single_value;

	if (!transient) {
		transient = tracker_property_get_transient (*property);
	}

	switch (tracker_property_get_data_type (*property)) {
	case TRACKER_PROPERTY_TYPE_STRING:
		sql_type = "TEXT";
		break;
	case TRACKER_PROPERTY_TYPE_INTEGER:
	case TRACKER_PROPERTY_TYPE_BOOLEAN:
	case TRACKER_PROPERTY_TYPE_DATE:
	case TRACKER_PROPERTY_TYPE_DATETIME:
	case TRACKER_PROPERTY_TYPE_RESOURCE:
		sql_type = "INTEGER";
		break;
	case TRACKER_PROPERTY_TYPE_DOUBLE:
		sql_type = "REAL";
		break;
	case TRACKER_PROPERTY_TYPE_BLOB:
	case TRACKER_PROPERTY_TYPE_STRUCT:
	case TRACKER_PROPERTY_TYPE_FULLTEXT:
	default:
		sql_type = "";
		break;
	}

	if (transient || tracker_property_get_multiple_values (*property)) {
		/* multiple values */
		if (tracker_property_get_indexed (*property)) {
			/* use different UNIQUE index for properties whose
			 * value should be indexed to minimize index size */
			tracker_db_interface_execute_query (iface, NULL,
				"CREATE %sTABLE \"%s_%s\" ("
				"ID INTEGER NOT NULL, "
				"\"%s\" %s NOT NULL, "
				"UNIQUE (\"%s\", ID))",
				transient ? "TEMPORARY " : "",
				service_name,
				field_name,
				field_name,
				sql_type,
				field_name);

			tracker_db_interface_execute_query (iface, NULL,
				"CREATE INDEX \"%s_%s_ID\" ON \"%s_%s\" (ID)",
				service_name,
				field_name,
				service_name,
				field_name);
		} else {
			/* we still have to include the property value in
			 * the unique index for proper constraints */
			tracker_db_interface_execute_query (iface, NULL,
				"CREATE %sTABLE \"%s_%s\" ("
				"ID INTEGER NOT NULL, "
				"\"%s\" %s NOT NULL, "
				"UNIQUE (ID, \"%s\"))",
				transient ? "TEMPORARY " : "",
				service_name,
				field_name,
				field_name,
				sql_type,
				field_name);
		}
	} else if (sql_type_for_single_value) {
		*sql_type_for_single_value = sql_type;
	}

}

static void
create_decomposed_metadata_tables (TrackerDBInterface *iface,
				   TrackerClass       *service,
				   gint               *max_id)
{
	const char *service_name;
	GString    *sql;
	TrackerProperty	  **properties, **property;
	GSList      *class_properties, *field_it;
	gboolean    main_class;

	service_name = tracker_class_get_name (service);
	main_class = (strcmp (service_name, "rdfs:Resource") == 0);

	if (g_str_has_prefix (service_name, "xsd:")) {
		/* xsd classes do not derive from rdfs:Resource and do not need separate tables */
		return;
	}

	sql = g_string_new ("");
	g_string_append_printf (sql, "CREATE TABLE \"%s\" (ID INTEGER NOT NULL PRIMARY KEY", service_name);
	if (main_class) {
		g_string_append (sql, ", Uri TEXT NOT NULL, Available INTEGER NOT NULL");
	}

	properties = tracker_ontology_get_properties ();
	class_properties = NULL;
	for (property = properties; *property; property++) {
		if (tracker_property_get_domain (*property) == service) {
			const gchar *sql_type_for_single_value = NULL;

			create_decomposed_metadata_property_table (iface, property, 
								   service_name, 
								   &sql_type_for_single_value);

			if (sql_type_for_single_value) {
				/* single value */

				class_properties = g_slist_prepend (class_properties, *property);

				g_string_append_printf (sql, ", \"%s\" %s", 
							tracker_property_get_name (*property), 
							sql_type_for_single_value);
			}
		}
	}

	if (main_class) {
		g_string_append (sql, ", UNIQUE (Uri)");
	}
	g_string_append (sql, ")");
	tracker_db_interface_execute_query (iface, NULL, "%s", sql->str);

	g_string_free (sql, TRUE);

	/* create index for single-valued fields */
	for (field_it = class_properties; field_it != NULL; field_it = field_it->next) {
		TrackerProperty *field;
		const char   *field_name;

		field = field_it->data;

		if (!tracker_property_get_multiple_values (field)
		    && tracker_property_get_indexed (field)) {
			field_name = tracker_property_get_name (field);
			tracker_db_interface_execute_query (iface, NULL,
							    "CREATE INDEX \"%s_%s\" ON \"%s\" (\"%s\")",
							    service_name,
							    field_name,
							    service_name,
							    field_name);
		}
	}

	g_slist_free (class_properties);

	/* insert class uri in rdfs:Resource table */
	if (tracker_class_get_uri (service) != NULL) {
		TrackerDBStatement *stmt;

		stmt = tracker_db_interface_create_statement (iface,
							      "INSERT OR IGNORE INTO \"rdfs:Resource\" (ID, Uri, \"tracker:modified\") VALUES (?, ?, ?)");
		tracker_db_statement_bind_int (stmt, 0, ++(*max_id));
		tracker_db_statement_bind_text (stmt, 1, tracker_class_get_uri (service));
		tracker_db_statement_bind_int64 (stmt, 2, (gint64) time (NULL));
		tracker_db_statement_execute (stmt, NULL);
		g_object_unref (stmt);
	}
}

static void
create_decomposed_transient_metadata_tables (TrackerDBInterface *iface)
{
	TrackerProperty **properties;
	TrackerProperty **property;

	properties = tracker_ontology_get_properties ();

	for (property = properties; *property; property++) {
		if (tracker_property_get_transient (*property)) {

			TrackerClass *domain;
			const gchar *service_name;
			const char *field_name;

			field_name = tracker_property_get_name (*property);

			domain = tracker_property_get_domain (*property);
			service_name = tracker_class_get_name (domain);

			/* create the TEMPORARY table */
			create_decomposed_metadata_property_table (iface, property,
								   service_name,
								   NULL);
			
		}
	}
}

static void
create_fts_table (TrackerDBInterface *iface)
{
	gchar *query = tracker_fts_get_create_fts_table_query ();

	tracker_db_interface_execute_query (iface, NULL, "%s", query);

	g_free (query);
}

gboolean
tracker_data_manager_init (TrackerDBManagerFlags       flags,
			   const gchar                *test_schema,
			   gboolean                   *first_time,
			   gboolean                   *need_journal)
{
	TrackerDBInterface *iface;
	gboolean is_first_time_index;

	if (initialized) {
		return TRUE;
	}

	tracker_db_manager_init (flags, &is_first_time_index, FALSE, need_journal);

	if (first_time != NULL) {
		*first_time = is_first_time_index;
	}

	iface = tracker_db_manager_get_db_interface ();

	if (is_first_time_index) {
		TrackerClass **classes;
		TrackerClass **cl;
		gint max_id = 0;
		GList *sorted = NULL, *l;
		gchar *test_schema_path;
		const gchar *env_path;
		GError *error = NULL;

		env_path = g_getenv ("TRACKER_DB_ONTOLOGIES_DIR");
		
		if (G_LIKELY (!env_path)) {
			ontologies_dir = g_build_filename (SHAREDIR,
							   "tracker",
							   "ontologies",
							   NULL);
		} else {
			ontologies_dir = g_strdup (env_path);
		}

		if (test_schema) {
			/* load test schema, not used in normal operation */
			test_schema_path = g_strconcat (test_schema, ".ontology", NULL);

			sorted = g_list_prepend (sorted, g_strdup ("12-nrl.ontology"));
			sorted = g_list_prepend (sorted, g_strdup ("11-rdf.ontology"));
			sorted = g_list_prepend (sorted, g_strdup ("10-xsd.ontology"));
		} else {
			GDir        *ontologies;
			const gchar *conf_file;

			ontologies = g_dir_open (ontologies_dir, 0, NULL);

			conf_file = g_dir_read_name (ontologies);

			/* .ontology files */
			while (conf_file) {
				if (g_str_has_suffix (conf_file, ".ontology")) {
					sorted = g_list_insert_sorted (sorted,
								       g_strdup (conf_file), 
								       (GCompareFunc) strcmp);
				}
				conf_file = g_dir_read_name (ontologies);
			}

			g_dir_close (ontologies);
		}

		/* load ontology from files into memory */
		for (l = sorted; l; l = l->next) {
			g_debug ("Loading ontology %s", (char *) l->data);
			load_ontology_file (l->data);
		}

		if (test_schema) {
			g_debug ("Loading ontology:'%s' (TEST ONTOLOGY)", test_schema_path);

			load_ontology_file_from_path (test_schema_path);
		}

		classes = tracker_ontology_get_classes ();

		tracker_data_begin_transaction ();

		/* create tables */
		for (cl = classes; *cl; cl++) {
			create_decomposed_metadata_tables (iface, *cl, &max_id);
		}

		create_fts_table (iface);

		/* store ontology in database */
		for (l = sorted; l; l = l->next) {
			import_ontology_file (l->data);
		}
		if (test_schema) {
			tracker_turtle_reader_load (test_schema_path, &error);
			g_free (test_schema_path);

			if (error) {
				g_critical ("%s", error->message);
				g_error_free (error);
			}
		}

		tracker_data_commit_transaction ();

		g_list_foreach (sorted, (GFunc) g_free, NULL);
		g_list_free (sorted);

		g_free (ontologies_dir);
		ontologies_dir = NULL;
	} else {
		/* load ontology from database into memory */
		db_get_static_data (iface);
		create_decomposed_transient_metadata_tables (iface);
	}

	/* ensure FTS is fully initialized */
	tracker_db_interface_execute_query (iface, NULL, "SELECT 1 FROM fulltext.fts WHERE rowid = 0");

	initialized = TRUE;

	return TRUE;
}


void
tracker_data_manager_shutdown (void)
{
	g_return_if_fail (initialized == TRUE);

	tracker_db_manager_shutdown ();

	initialized = FALSE;
}

gint64
tracker_data_manager_get_db_option_int64 (const gchar *option)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	TrackerDBResultSet *result_set;
	gchar		   *str;
	gint		    value = 0;

	g_return_val_if_fail (option != NULL, 0);

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface, "SELECT OptionValue FROM Options WHERE OptionKey = ?");
	tracker_db_statement_bind_text (stmt, 0, option);
	result_set = tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	if (result_set) {
		tracker_db_result_set_get (result_set, 0, &str, -1);

		if (str) {
			value = g_ascii_strtoull (str, NULL, 10);
			g_free (str);
		}

		g_object_unref (result_set);
	}

	return value;
}

void
tracker_data_manager_set_db_option_int64 (const gchar *option,
					  gint64       value)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	gchar		   *str;

	g_return_if_fail (option != NULL);

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface, "REPLACE INTO Options (OptionKey, OptionValue) VALUES (?,?)");
	tracker_db_statement_bind_text (stmt, 0, option);

	str = g_strdup_printf ("%"G_GINT64_FORMAT, value);
	tracker_db_statement_bind_text (stmt, 1, str);
	g_free (str);

	tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);
}
