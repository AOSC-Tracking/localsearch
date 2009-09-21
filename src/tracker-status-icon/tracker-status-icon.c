/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009, Nokia (urho.konttori@nokia.com)
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
#include "tracker-status-icon.h"
#include <libtracker-miner/tracker-miner-manager.h>
#include <string.h>
#include <locale.h>
#include <glib/gi18n.h>

#define TRACKER_STATUS_ICON_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_STATUS_ICON, TrackerStatusIconPrivate))

typedef struct TrackerStatusIconPrivate TrackerStatusIconPrivate;
typedef struct MinerMenuEntry MinerMenuEntry;

enum {
	ICON_IDLE,
	ICON_PAUSED,
	ICON_INDEXING_1,
	ICON_INDEXING_2,
	ICON_LAST
};

typedef enum {
	STATUS_NONE,
	STATUS_IDLE,
	STATUS_PAUSED,
	STATUS_INDEXING
} TrackerStatus;

struct TrackerStatusIconPrivate {
	GdkPixbuf *icons [ICON_LAST];
	TrackerStatus current_status;
	guint animation_id;

	TrackerMinerManager *manager;
	GtkWidget *miner_menu;
	GtkWidget *context_menu;
	GtkSizeGroup *size_group;

	GHashTable *miners;
};

struct MinerMenuEntry {
	GtkWidget *menu_item;
	GtkWidget *box;
	GtkWidget *state;
	GtkWidget *name;
	GtkWidget *progress_bar;

	gdouble progress;
	guint32 cookie;
	guint active : 1;
};

static void status_icon_constructed (GObject          *object);
static void status_icon_finalize    (GObject          *object);

static void status_icon_activate   (GtkStatusIcon     *icon);
static void status_icon_popup_menu (GtkStatusIcon     *icon,
				    guint              button,
				    guint32            activate_time);

static void status_icon_miner_progress (TrackerMinerManager *manager,
					const gchar         *miner_name,
					const gchar         *status,
					gdouble              progress,
					gpointer             user_data);
static void status_icon_miner_paused   (TrackerMinerManager *manager,
					const gchar         *miner_name,
					gpointer             user_data);
static void status_icon_miner_resumed  (TrackerMinerManager *manager,
					const gchar         *miner_name,
					gpointer             user_data);
static void status_icon_miner_activated   (TrackerMinerManager *manager,
					   const gchar         *miner_name,
					   gpointer             user_data);
static void status_icon_miner_deactivated (TrackerMinerManager *manager,
					   const gchar         *miner_name,
					   gpointer             user_data);

static void        status_icon_initialize_miners_menu (TrackerStatusIcon *icon);
static GtkWidget * status_icon_create_context_menu    (TrackerStatusIcon *icon);

static void status_icon_set_status (TrackerStatusIcon *icon,
				    TrackerStatus      status);

G_DEFINE_TYPE (TrackerStatusIcon, tracker_status_icon, GTK_TYPE_STATUS_ICON)

static void
tracker_status_icon_class_init (TrackerStatusIconClass *klass)
{
	GtkStatusIconClass *status_icon_class = GTK_STATUS_ICON_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	status_icon_class->activate = status_icon_activate;
	status_icon_class->popup_menu = status_icon_popup_menu;

	object_class->constructed = status_icon_constructed;
	object_class->finalize = status_icon_finalize;

	g_type_class_add_private (klass, sizeof (TrackerStatusIconPrivate));
}

static void
tracker_status_icon_init (TrackerStatusIcon *icon)
{
	TrackerStatusIconPrivate *priv;
	const gchar *icon_names[] = {
		"tracker-applet-default.png",
		"tracker-applet-paused.png",
		"tracker-applet-indexing1.png",
		"tracker-applet-indexing2.png"
	};
	gint i;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);

	for (i = ICON_IDLE; i < ICON_LAST; i++) {
		GError *error = NULL;
		gchar *icon_path;

		icon_path = g_strconcat (ICONS_DIR, G_DIR_SEPARATOR_S, icon_names[i], NULL);
		priv->icons[i] = gdk_pixbuf_new_from_file (icon_path, &error);

		if (error) {
			g_warning ("Could not load icon '%s': %s\n", icon_names[i], error->message);
			g_error_free (error);
		}

		g_free (icon_path);
	}

	priv->miners = g_hash_table_new_full (g_str_hash, g_str_equal,
					      (GDestroyNotify) g_free,
					      (GDestroyNotify) g_free);

	priv->miner_menu = gtk_menu_new ();
	priv->context_menu = status_icon_create_context_menu (icon);
	priv->size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

	priv->manager = tracker_miner_manager_new ();
	g_signal_connect (priv->manager, "miner-progress",
			  G_CALLBACK (status_icon_miner_progress), icon);
	g_signal_connect (priv->manager, "miner-paused",
			  G_CALLBACK (status_icon_miner_paused), icon);
	g_signal_connect (priv->manager, "miner-resumed",
			  G_CALLBACK (status_icon_miner_resumed), icon);
	g_signal_connect (priv->manager, "miner-activated",
			  G_CALLBACK (status_icon_miner_activated), icon);
	g_signal_connect (priv->manager, "miner-deactivated",
			  G_CALLBACK (status_icon_miner_deactivated), icon);
	status_icon_initialize_miners_menu (icon);
}

static void
status_icon_constructed (GObject *object)
{
	/* Initialize status */
	status_icon_set_status (TRACKER_STATUS_ICON (object), STATUS_IDLE);

	if (G_OBJECT_CLASS (tracker_status_icon_parent_class)->constructed) {
		G_OBJECT_CLASS (tracker_status_icon_parent_class)->constructed (object);
	}
}

static void
status_icon_finalize (GObject *object)
{
	TrackerStatusIconPrivate *priv;
	gint i;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (object);

	for (i = ICON_IDLE; i < ICON_LAST; i++) {
		if (priv->icons[i]) {
			g_object_unref (priv->icons[i]);
		}
	}

	if (priv->animation_id) {
		g_source_remove (priv->animation_id);
		priv->animation_id = 0;
	}

	g_object_unref (priv->manager);
	g_object_unref (priv->size_group);

	G_OBJECT_CLASS (tracker_status_icon_parent_class)->finalize (object);
}

static void
status_icon_activate (GtkStatusIcon *icon)
{
	TrackerStatusIconPrivate *priv;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);

	gtk_menu_popup (GTK_MENU (priv->miner_menu),
			NULL, NULL,
			gtk_status_icon_position_menu,
			icon, 0,
			gtk_get_current_event_time ());
}

static void
status_icon_popup_menu (GtkStatusIcon *icon,
			guint          button,
			guint32        activate_time)
{
	TrackerStatusIconPrivate *priv;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);

	gtk_menu_popup (GTK_MENU (priv->context_menu),
			NULL, NULL,
			gtk_status_icon_position_menu,
			icon, button, activate_time);
}

static void
update_icon_status (TrackerStatusIcon *icon)
{
	TrackerStatusIconPrivate *priv;
	GHashTableIter iter;
	gpointer key, value;
	gint miners_idle, miners_indexing, miners_paused;
	TrackerStatus status;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	g_hash_table_iter_init (&iter, priv->miners);
	miners_idle = miners_indexing = miners_paused = 0;

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		MinerMenuEntry *entry = value;

		if (!entry->active) {
			miners_idle++;
		} else {
			if (entry->cookie == 0) {
				if (entry->progress != 1) {
					miners_indexing++;
				} else {
					miners_idle++;
				}
			} else {
				miners_paused++;
			}
		}
	}

	if (miners_indexing > 0) {
		/* Some miner is indexing, the others are either
		 * paused or inactive, so we shouldn't care about them
		 */
		status = STATUS_INDEXING;
	} else if (miners_paused > 0) {
		/* All active miners are paused */
		status = STATUS_PAUSED;
	} else {
		/* No paused nor running miners */
		status = STATUS_IDLE;
	}

	status_icon_set_status (icon, status);
}

static void
status_icon_miner_progress (TrackerMinerManager *manager,
			    const gchar         *miner_name,
			    const gchar         *status,
			    gdouble              progress,
			    gpointer             user_data)
{
	TrackerStatusIconPrivate *priv;
	TrackerStatusIcon *icon;
	MinerMenuEntry *entry;
	gchar *progress_str;

	icon = TRACKER_STATUS_ICON (user_data);
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	entry = g_hash_table_lookup (priv->miners, miner_name);

	if (G_UNLIKELY (!entry)) {
		g_critical ("Got progress signal from unknown miner");
		return;
	}

	gtk_widget_set_tooltip_text (entry->box, status);
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (entry->progress_bar), progress);

	progress_str = g_strdup_printf ("%3.0f%%", progress * 100);
	gtk_progress_bar_set_text (GTK_PROGRESS_BAR (entry->progress_bar), progress_str);
	g_free (progress_str);

	entry->progress = progress;

	update_icon_status (icon);
}

static void
status_icon_miner_paused (TrackerMinerManager *manager,
			  const gchar         *miner_name,
			  gpointer             user_data)
{
	TrackerStatusIconPrivate *priv;
	TrackerStatusIcon *icon;
	MinerMenuEntry *entry;

	icon = TRACKER_STATUS_ICON (user_data);
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	entry = g_hash_table_lookup (priv->miners, miner_name);

	if (G_UNLIKELY (!entry)) {
		g_critical ("Got pause signal from unknown miner");
		return;
	}

	gtk_image_set_from_stock (GTK_IMAGE (entry->state),
				  GTK_STOCK_MEDIA_PAUSE,
				  GTK_ICON_SIZE_MENU);

	update_icon_status (icon);
}

static void
status_icon_miner_resumed (TrackerMinerManager *manager,
			   const gchar         *miner_name,
			   gpointer             user_data)
{
	TrackerStatusIconPrivate *priv;
	TrackerStatusIcon *icon;
	MinerMenuEntry *entry;

	icon = TRACKER_STATUS_ICON (user_data);
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	entry = g_hash_table_lookup (priv->miners, miner_name);

	if (G_UNLIKELY (!entry)) {
		g_critical ("Got pause signal from unknown miner");
		return;
	}

	gtk_image_set_from_stock (GTK_IMAGE (entry->state),
				  GTK_STOCK_MEDIA_PLAY,
				  GTK_ICON_SIZE_MENU);

	update_icon_status (icon);
}

static void
status_icon_miner_activated (TrackerMinerManager *manager,
			     const gchar         *miner_name,
			     gpointer             user_data)
{
	TrackerStatusIconPrivate *priv;
	TrackerStatusIcon *icon;
	MinerMenuEntry *entry;

	icon = TRACKER_STATUS_ICON (user_data);
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	entry = g_hash_table_lookup (priv->miners, miner_name);

	if (G_UNLIKELY (!entry)) {
		g_critical ("Got pause signal from unknown miner");
		return;
	}

	gtk_widget_set_sensitive (entry->menu_item, TRUE);
	entry->active = TRUE;

	update_icon_status (icon);
}

static void
status_icon_miner_deactivated (TrackerMinerManager *manager,
			       const gchar         *miner_name,
			       gpointer             user_data)
{
	TrackerStatusIconPrivate *priv;
	TrackerStatusIcon *icon;
	MinerMenuEntry *entry;

	icon = TRACKER_STATUS_ICON (user_data);
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	entry = g_hash_table_lookup (priv->miners, miner_name);

	if (G_UNLIKELY (!entry)) {
		g_critical ("Got pause signal from unknown miner");
		return;
	}

	gtk_widget_set_sensitive (entry->menu_item, FALSE);
	status_icon_miner_progress (priv->manager, miner_name,
				    _("Miner is not running"), 0.0, icon);
	entry->active = FALSE;

	/* invalidate pause cookie */
	entry->cookie = 0;

	update_icon_status (icon);
}

static void
miner_menu_entry_activate_cb (GtkMenuItem *item,
			      gpointer     user_data)
{
	TrackerStatusIconPrivate *priv;
	MinerMenuEntry *entry;
	const gchar *miner;
	guint32 cookie;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (user_data);
	miner = g_object_get_data (G_OBJECT (item), "menu-entry-miner-name");
	entry = g_hash_table_lookup (priv->miners, miner);

	g_assert (entry != NULL);
	if (G_UNLIKELY (!entry)) {
		g_critical ("Got pause signal from unknown miner");
		return;
	}

	if (entry->cookie == 0) {
		/* Miner was not paused from here */
		if (tracker_miner_manager_pause (priv->manager, miner,
						 _("Paused by user"), &cookie)) {
			entry->cookie = cookie;
		}
	} else {
		/* Miner was paused from here */
		if (tracker_miner_manager_resume (priv->manager, miner, entry->cookie)) {
			entry->cookie = 0;
		}
	}
}

static void
miner_menu_entry_add (TrackerStatusIcon *icon,
		      const gchar       *miner)
{
	TrackerStatusIconPrivate *priv;
	MinerMenuEntry *entry;
	const gchar *name;
	gchar *str;

	name = strrchr (miner, '.');

	if (!name) {
		g_warning ("Miner name '%s' doesn't look valid", miner);
		return;
	}

	name++;
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	str = g_strdup (miner);

	entry = g_new0 (MinerMenuEntry, 1);
	entry->box = gtk_hbox_new (FALSE, 6);
	entry->state = gtk_image_new_from_stock (GTK_STOCK_MEDIA_PLAY,
						 GTK_ICON_SIZE_MENU);
	entry->name = gtk_label_new (name);
	gtk_misc_set_alignment (GTK_MISC (entry->name), 0, 0.5);

	entry->progress_bar = gtk_progress_bar_new ();

	gtk_box_pack_start (GTK_BOX (entry->box), entry->name, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (entry->box), entry->progress_bar, TRUE, TRUE, 0);

	gtk_size_group_add_widget (priv->size_group, entry->name);

	entry->menu_item = gtk_image_menu_item_new ();
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (entry->menu_item), entry->state);
	g_object_set_data (G_OBJECT (entry->menu_item), "menu-entry-miner-name", str);
	g_signal_connect (entry->menu_item, "activate",
			  G_CALLBACK (miner_menu_entry_activate_cb), icon);

	gtk_container_add (GTK_CONTAINER (entry->menu_item), entry->box);
	gtk_widget_show_all (entry->menu_item);

	gtk_menu_shell_append (GTK_MENU_SHELL (priv->miner_menu), entry->menu_item);

	entry->active = tracker_miner_manager_is_active (priv->manager, miner);

	if (!entry->active) {
		gtk_widget_set_sensitive (entry->menu_item, FALSE);
	}

	g_hash_table_replace (priv->miners, str, entry);
}

static void
status_icon_initialize_miners_menu (TrackerStatusIcon *icon)
{
	TrackerStatusIconPrivate *priv;
	GSList *miners, *m;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);

	miners = tracker_miner_manager_get_available (priv->manager);

	for (m = miners; m; m = m->next) {
		miner_menu_entry_add (icon, (const gchar *) m->data);
	}

	g_slist_free (miners);
}

static void
launch_application_on_screen (GdkScreen   *screen,
			      const gchar *command_line)
{
	GError *error = NULL;

	if (!gdk_spawn_command_line_on_screen (screen, command_line, &error)) {
		g_critical ("Could not spawn '%s': %s", command_line, error->message);
		g_error_free (error);
	}
}

static void
context_menu_pause_cb (GtkMenuItem *item,
		       gpointer     user_data)
{
	TrackerStatusIcon *icon;
	TrackerStatusIconPrivate *priv;
	GHashTableIter iter;
	gpointer key, value;
	gboolean active;

	icon = user_data;
	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	active = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (item));
	g_hash_table_iter_init (&iter, priv->miners);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		MinerMenuEntry *entry = value;
		const gchar *miner = key;
		guint32 cookie;

		if (active && entry->cookie == 0) {
			if (tracker_miner_manager_pause (priv->manager, miner,
							 _("Paused by user"), &cookie)) {
				entry->cookie = cookie;
			}
		} else if (!active && entry->cookie != 0) {
			if (tracker_miner_manager_resume (priv->manager, miner, entry->cookie)) {
				entry->cookie = 0;
			}
		}
	}

	update_icon_status (icon);
}

static void
context_menu_search_cb (GtkMenuItem *item,
			gpointer     user_data)
{
	launch_application_on_screen (gtk_widget_get_screen (GTK_WIDGET (item)),
				      "tracker-search-tool");
}

static void
context_menu_preferences_cb (GtkMenuItem *item,
			     gpointer     user_data)
{
	launch_application_on_screen (gtk_widget_get_screen (GTK_WIDGET (item)),
				      "tracker-preferences");
}

static void
context_menu_about_cb (GtkMenuItem *item,
		       gpointer     user_data)
{
	const gchar *authors[] = {
		"Jamie McCracken <jamiemcc at gnome.org>",
		"Saleem Abdulrasool <compnerd at compnerd.org>"
                "Laurent Aguerreche <laurent.aguerreche at free fr>",
		"Luca Ferretti <elle.uca@libero.it>",
		"Eugenio <me at eugesoftware com>",
		"Michael Biebl <mbiebl at gmail com>",
		"Edward Duffy <eduffy at gmail com>",
		"Gergan Penkov <gergan at gmail com>",
		"Deji Akingunola <dakingun gmail com>",
		"Julien <julienc psychologie-fr org>",
		"Tom <tpgww@onepost.net>",
		"Samuel Cormier-Iijima <sciyoshi at gmail com>",
		"Eskil Bylund <eskil at letterboxes org>",
		"Ulrik Mikaelsson <ulrik mikaelsson gmail com>",
		"tobutaz <tobutaz gmail com>",
		"Mikkel Kamstrup Erlandsen <mikkel kamstrup gmail com>",
		"Baptiste Mille-Mathias <baptiste.millemathias gmail com>",
		"Richard Quirk <quirky@zoom.co.uk>",
		"Marcus Fritzsch <fritschy at googlemail com>",
		"Jedy Wang <Jedy Wang at Sun COM>",
		"Anders Aagaard <aagaande at gmail com>",
		"Fabien VALLON <fabien at sonappart net>",
		"Jaime Frutos Morales <acidborg at gmail com>",
		"Christoph Laimburg <christoph laimburg at rolmail net>",
		NULL
	};

	const gchar *documenters[] = {
		NULL
	};

	const gchar *license[] = {
		N_("Tracker is free software; you can redistribute it and/or modify "
		   "it under the terms of the GNU General Public License as published by "
		   "the Free Software Foundation; either version 2 of the License, or "
		   "(at your option) any later version."),
		N_("Tracker is distributed in the hope that it will be useful, " 
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of " 
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the " 
		   "GNU General Public License for more details."),
		N_("You should have received a copy of the GNU General Public License " 
		   "along with Tracker; if not, write to the Free Software Foundation, Inc., " 
		   "51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.")
	};

	gchar *license_trans;

	license_trans = g_strjoin ("\n\n",
				   _(license[0]),
				   _(license[1]),
				   _(license[2]),
				   NULL);

	gtk_show_about_dialog (NULL,
			       "version", PACKAGE_VERSION,
			       "comments", _("Tracker is a tool designed to extract info and metadata about"
					     " your personal data so that it can be searched easily and quickly"),
			       "copyright", _("Copyright \xC2\xA9 2005-2009 "
					      "The Tracker authors"),
			       "license", license_trans,
			       "wrap-license", TRUE,
			       "authors", authors,
			       "documenters", documenters,
			       /* Translators should localize the following string
				* which will be displayed at the bottom of the about
				* box to give credit to the translator(s).
				*/
			       "translator-credits", _("translator-credits"),
			       "logo-icon-name", "tracker",
			       "website", "http://www.tracker-project.org/",
			       "website-label", _("Tracker Web Site"),
			       NULL);

	g_free (license_trans);
}

static GtkWidget *
status_icon_create_context_menu (TrackerStatusIcon *icon)
{
	GtkWidget *menu, *item, *image;

	menu = gtk_menu_new ();

	item = gtk_check_menu_item_new_with_mnemonic (_("_Pause All Indexing"));
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), FALSE);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "toggled",
			  G_CALLBACK (context_menu_pause_cb), icon);

	item = gtk_separator_menu_item_new ();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

	item = gtk_image_menu_item_new_with_mnemonic (_("_Search"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_FIND,
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (context_menu_search_cb), icon);

	item = gtk_image_menu_item_new_with_mnemonic (_("_Preferences"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_PREFERENCES,
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (context_menu_preferences_cb),
			  icon);

	/*
	item = gtk_image_menu_item_new_with_mnemonic (_("_Indexer Preferences"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_PREFERENCES,
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (preferences_menu_activated), icon);
	*/

	/*
	item = gtk_image_menu_item_new_with_mnemonic (_("S_tatistics"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_INFO,
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (statistics_menu_activated), icon);
	*/

	item = gtk_image_menu_item_new_with_mnemonic (_("_About"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_ABOUT,
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (context_menu_about_cb), icon);

	/*
	item = gtk_separator_menu_item_new ();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

	item = gtk_image_menu_item_new_with_mnemonic (_("_Quit"));
	image = gtk_image_new_from_icon_name (GTK_STOCK_QUIT,
					      GTK_ICON_SIZE_MENU);
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	g_signal_connect (G_OBJECT (item), "activate",
			  G_CALLBACK (quit_menu_activated), icon);
	*/

	gtk_widget_show_all (menu);

	return menu;
}

static gboolean
animate_indexing_cb (TrackerStatusIcon *icon)
{
	TrackerStatusIconPrivate *priv;
	GdkPixbuf *pixbuf, *current_pixbuf;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);
	current_pixbuf = gtk_status_icon_get_pixbuf (GTK_STATUS_ICON (icon));

	if (current_pixbuf == priv->icons[ICON_INDEXING_1]) {
		pixbuf = priv->icons[ICON_INDEXING_2];
	} else {
		pixbuf = priv->icons[ICON_INDEXING_1];
	}

	gtk_status_icon_set_from_pixbuf (GTK_STATUS_ICON (icon), pixbuf);

	return TRUE;
}

static void
animate_indexing (TrackerStatusIcon *icon,
		  gboolean           animate)
{
	TrackerStatusIconPrivate *priv;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);

	if (animate) {
		if (priv->animation_id == 0) {
			priv->animation_id =
				g_timeout_add_seconds (2, (GSourceFunc) animate_indexing_cb, icon);

			animate_indexing_cb (icon);
		}
	} else {
		if (priv->animation_id != 0) {
			g_source_remove (priv->animation_id);
			priv->animation_id = 0;
		}
	}
}

static void
status_icon_set_status (TrackerStatusIcon *icon,
			TrackerStatus      status)
{
	TrackerStatusIconPrivate *priv;

	priv = TRACKER_STATUS_ICON_GET_PRIVATE (icon);

	if (priv->current_status == status) {
		return;
	}

	switch (status) {
	case STATUS_IDLE:
		animate_indexing (icon, FALSE);
		gtk_status_icon_set_from_pixbuf (GTK_STATUS_ICON (icon),
						 priv->icons [ICON_IDLE]);
		break;
	case STATUS_PAUSED:
		animate_indexing (icon, FALSE);
		gtk_status_icon_set_from_pixbuf (GTK_STATUS_ICON (icon),
						 priv->icons [ICON_PAUSED]);
		break;
	case STATUS_INDEXING:
		animate_indexing (icon, TRUE);
		break;
	default:
		g_critical ("Unknown status '%d'", status);
		g_assert_not_reached ();
	}

	priv->current_status = status;
}

GtkStatusIcon *
tracker_status_icon_new (void)
{
	return g_object_new (TRACKER_TYPE_STATUS_ICON, NULL);
}
