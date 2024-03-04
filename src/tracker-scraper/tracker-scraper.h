/*
 * Copyright (C) 2024, Carlos Garnacho

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
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#ifndef __TRACKER_SCRAPER_H__
#define __TRACKER_SCRAPER_H__

#include <libtracker-sparql/tracker-sparql.h>

#define TRACKER_TYPE_SCRAPER (tracker_scraper_get_type ())
G_DECLARE_FINAL_TYPE (TrackerScraper, tracker_scraper, TRACKER, SCRAPER, GObject)

TrackerScraper * tracker_scraper_new (TrackerSparqlConnection *sparql_conn);

#endif /* __TRACKER_SCRAPER_H__ */
