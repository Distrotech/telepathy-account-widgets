/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2005-2007 Imendio AB
 * Copyright (C) 2007-2010 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301  USA
 *
 * Authors: Mikael Hallendal <micke@imendio.com>
 *          Martyn Russell <martyn@imendio.com>
 *          Xavier Claessens <xclaesse@gmail.com>
 *          Travis Reitter <travis.reitter@collabora.co.uk>
 */

#include "config.h"

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include <folks/folks.h>
#include <folks/folks-telepathy.h>
#include <telepathy-glib/util.h>

#include <libempathy/empathy-utils.h>
#include <libempathy/empathy-enum-types.h>
#include <libempathy/empathy-individual-manager.h>

#include "empathy-individual-store.h"
#include "empathy-ui-utils.h"
#include "empathy-gtk-enum-types.h"

#define DEBUG_FLAG EMPATHY_DEBUG_CONTACT
#include <libempathy/empathy-debug.h>

/* Active users are those which have recently changed state
 * (e.g. online, offline or from normal to a busy state).
 */

/* Time in seconds user is shown as active */
#define ACTIVE_USER_SHOW_TIME 7

/* Time in seconds after connecting which we wait before active users are enabled */
#define ACTIVE_USER_WAIT_TO_ENABLE_TIME 5

#define GET_PRIV(obj) EMPATHY_GET_PRIV (obj, EmpathyIndividualStore)
typedef struct
{
  EmpathyIndividualManager *manager;
  gboolean show_offline;
  gboolean show_avatars;
  gboolean show_groups;
  gboolean is_compact;
  gboolean show_protocols;
  gboolean show_active;
  EmpathyIndividualStoreSort sort_criterium;
  guint inhibit_active;
  guint setup_idle_id;
  gboolean dispose_has_run;
  GHashTable *status_icons;
} EmpathyIndividualStorePriv;

typedef struct
{
  GtkTreeIter iter;
  const gchar *name;
  gboolean found;
} FindGroup;

typedef struct
{
  FolksIndividual *individual;
  gboolean found;
  GList *iters;
} FindContact;

typedef struct
{
  EmpathyIndividualStore *self;
  FolksIndividual *individual;
  gboolean remove;
} ShowActiveData;

enum
{
  PROP_0,
  PROP_INDIVIDUAL_MANAGER,
  PROP_SHOW_OFFLINE,
  PROP_SHOW_AVATARS,
  PROP_SHOW_PROTOCOLS,
  PROP_SHOW_GROUPS,
  PROP_IS_COMPACT,
  PROP_SORT_CRITERIUM
};

/* prototypes to break cycles */
static void individual_store_contact_update (EmpathyIndividualStore *self,
    FolksIndividual *individual);

G_DEFINE_TYPE (EmpathyIndividualStore, empathy_individual_store,
    GTK_TYPE_TREE_STORE);

static void
add_individual_to_store (GtkTreeStore *self,
    GtkTreeIter *iter,
    GtkTreeIter *parent,
    FolksIndividual *individual,
    EmpathyIndividualManagerFlags flags)
{
  gtk_tree_store_insert_with_values (self, iter, parent, 0,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME,
      folks_individual_get_alias (individual),
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, individual,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, FALSE,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, FALSE,
      EMPATHY_INDIVIDUAL_STORE_COL_CAN_AUDIO_CALL,
      folks_individual_get_capabilities (individual) &
      FOLKS_CAPABILITIES_FLAGS_AUDIO,
      EMPATHY_INDIVIDUAL_STORE_COL_CAN_VIDEO_CALL,
      folks_individual_get_capabilities (individual) &
      FOLKS_CAPABILITIES_FLAGS_VIDEO,
      EMPATHY_INDIVIDUAL_STORE_COL_FLAGS, flags,
      -1);
}

static gboolean
individual_store_get_group_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    FindGroup *fg)
{
  gchar *str;
  gboolean is_group;

  /* Groups are only at the top level. */
  if (gtk_tree_path_get_depth (path) != 1)
    {
      return FALSE;
    }

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &str,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group, -1);

  if (is_group && !tp_strdiff (str, fg->name))
    {
      fg->found = TRUE;
      fg->iter = *iter;
    }

  g_free (str);

  return fg->found;
}

static void
individual_store_get_group (EmpathyIndividualStore *self,
    const gchar *name,
    GtkTreeIter *iter_group_to_set,
    GtkTreeIter *iter_separator_to_set,
    gboolean *created,
    gboolean is_fake_group)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;
  GtkTreeIter iter_group;
  GtkTreeIter iter_separator;
  FindGroup fg;

  priv = GET_PRIV (self);

  memset (&fg, 0, sizeof (fg));

  fg.name = name;

  model = GTK_TREE_MODEL (self);
  gtk_tree_model_foreach (model,
      (GtkTreeModelForeachFunc) individual_store_get_group_foreach, &fg);

  if (!fg.found)
    {
      if (created)
        {
          *created = TRUE;
        }

      gtk_tree_store_insert_with_values (GTK_TREE_STORE (self), &iter_group,
          NULL, 0,
          EMPATHY_INDIVIDUAL_STORE_COL_ICON_STATUS, NULL,
          EMPATHY_INDIVIDUAL_STORE_COL_NAME, name,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, TRUE,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, FALSE,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, FALSE,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, is_fake_group,
          -1);

      if (iter_group_to_set)
        {
          *iter_group_to_set = iter_group;
        }

      gtk_tree_store_insert_with_values (GTK_TREE_STORE (self), &iter_separator,
          &iter_group, 0,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, TRUE,
          -1);

      if (iter_separator_to_set)
        {
          *iter_separator_to_set = iter_separator;
        }
    }
  else
    {
      if (created)
        {
          *created = FALSE;
        }

      if (iter_group_to_set)
        {
          *iter_group_to_set = fg.iter;
        }

      iter_separator = fg.iter;

      if (gtk_tree_model_iter_next (model, &iter_separator))
        {
          gboolean is_separator;

          gtk_tree_model_get (model, &iter_separator,
              EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator, -1);

          if (is_separator && iter_separator_to_set)
            {
              *iter_separator_to_set = iter_separator;
            }
        }
    }
}

static gboolean
individual_store_find_contact_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    FindContact *fc)
{
  FolksIndividual *individual;

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual, -1);

  if (individual == fc->individual)
    {
      fc->found = TRUE;
      fc->iters = g_list_append (fc->iters, gtk_tree_iter_copy (iter));
    }

  if (individual)
    {
      g_object_unref (individual);
    }

  return FALSE;
}

static GList *
individual_store_find_contact (EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;
  GList *l = NULL;
  FindContact fc;

  priv = GET_PRIV (self);

  memset (&fc, 0, sizeof (fc));

  fc.individual = individual;

  model = GTK_TREE_MODEL (self);
  gtk_tree_model_foreach (model,
      (GtkTreeModelForeachFunc) individual_store_find_contact_foreach, &fc);

  if (fc.found)
    {
      l = fc.iters;
    }

  return l;
}

static void
individual_store_remove_individual (EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;
  GList *iters, *l;

  priv = GET_PRIV (self);

  iters = individual_store_find_contact (self, individual);
  if (!iters)
    {
      return;
    }

  /* Clean up model */
  model = GTK_TREE_MODEL (self);

  for (l = iters; l; l = l->next)
    {
      GtkTreeIter parent;

      /* NOTE: it is only <= 2 here because we have
       * separators after the group name, otherwise it
       * should be 1.
       */
      if (gtk_tree_model_iter_parent (model, &parent, l->data) &&
          gtk_tree_model_iter_n_children (model, &parent) <= 2)
        {
          gtk_tree_store_remove (GTK_TREE_STORE (self), &parent);
        }
      else
        {
          gtk_tree_store_remove (GTK_TREE_STORE (self), l->data);
        }
    }

  g_list_foreach (iters, (GFunc) gtk_tree_iter_free, NULL);
  g_list_free (iters);
}

static void
individual_store_add_individual (EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeIter iter;
  GHashTable *group_set = NULL;
  GList *groups = NULL, *l;
  EmpathyIndividualManager *manager;
  EmpathyContact *contact;
  TpConnection *connection;
  EmpathyIndividualManagerFlags flags = 0;

  priv = GET_PRIV (self);

  if (EMP_STR_EMPTY (folks_individual_get_alias (individual)) ||
      (!priv->show_offline && !folks_individual_is_online (individual)))
    {
      return;
    }

  if (priv->show_groups)
    {
      group_set = folks_individual_get_groups (individual);
      groups = g_hash_table_get_keys (group_set);
    }

  manager = empathy_individual_manager_dup_singleton ();
  contact = empathy_contact_from_folks_individual (individual);
  connection = empathy_contact_get_connection (contact);
  flags = empathy_individual_manager_get_flags_for_connection (manager,
      connection);

  if (!groups)
    {
      GtkTreeIter iter_group, *parent;

      parent = &iter_group;

      /* TODO: implement */
      DEBUG ("forcing the People Nearby group even when 'show "
          "groups' is off is unimplemented");

      if (!priv->show_groups)
        {
          parent = NULL;
        }
      else
        {
          individual_store_get_group (self,
              EMPATHY_INDIVIDUAL_STORE_UNGROUPED,
              &iter_group, NULL, NULL, TRUE);
        }

      add_individual_to_store (GTK_TREE_STORE (self), &iter, parent,
          individual, flags);
    }

  /* Else add to each group. */
  for (l = groups; l; l = l->next)
    {
      GtkTreeIter iter_group;

      individual_store_get_group (self, l->data, &iter_group, NULL, NULL,
          FALSE);

      add_individual_to_store (GTK_TREE_STORE (self), &iter, &iter_group,
          individual, flags);
    }
  g_list_free (groups);
  if (group_set != NULL)
    g_hash_table_unref (group_set);

  if (priv->show_groups &&
      folks_favourite_get_is_favourite (FOLKS_FAVOURITE (individual)))
    {
      /* Add contact to the fake 'Favorites' group */
      GtkTreeIter iter_group;

      individual_store_get_group (self, EMPATHY_INDIVIDUAL_STORE_FAVORITE,
          &iter_group, NULL, NULL, TRUE);

      add_individual_to_store (GTK_TREE_STORE (self), &iter, &iter_group,
          individual, flags);
    }

  individual_store_contact_update (self, individual);
}

static void
individual_store_contact_set_active (EmpathyIndividualStore *self,
    FolksIndividual *individual,
    gboolean active,
    gboolean set_changed)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;
  GList *iters, *l;

  priv = GET_PRIV (self);
  model = GTK_TREE_MODEL (self);

  iters = individual_store_find_contact (self, individual);
  for (l = iters; l; l = l->next)
    {
      GtkTreePath *path;

      gtk_tree_store_set (GTK_TREE_STORE (self), l->data,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_ACTIVE, active,
          -1);

      DEBUG ("Set item %s", active ? "active" : "inactive");

      if (set_changed)
        {
          path = gtk_tree_model_get_path (model, l->data);
          gtk_tree_model_row_changed (model, path, l->data);
          gtk_tree_path_free (path);
        }
    }

  g_list_foreach (iters, (GFunc) gtk_tree_iter_free, NULL);
  g_list_free (iters);

}

static ShowActiveData *
individual_store_contact_active_new (EmpathyIndividualStore *self,
    FolksIndividual *individual,
    gboolean remove_)
{
  ShowActiveData *data;

  DEBUG ("Individual'%s' now active, and %s be removed",
      folks_individual_get_alias (individual), remove_ ? "WILL" : "WILL NOT");

  data = g_slice_new0 (ShowActiveData);

  data->self = g_object_ref (self);
  data->individual = g_object_ref (individual);
  data->remove = remove_;

  return data;
}

static void
individual_store_contact_active_free (ShowActiveData *data)
{
  g_object_unref (data->individual);
  g_object_unref (data->self);

  g_slice_free (ShowActiveData, data);
}

static gboolean
individual_store_contact_active_cb (ShowActiveData *data)
{
  EmpathyIndividualStorePriv *priv;

  priv = GET_PRIV (data->self);

  if (data->remove &&
      !priv->show_offline && !folks_individual_is_online (data->individual))
    {
      DEBUG ("Individual'%s' active timeout, removing item",
          folks_individual_get_alias (data->individual));
      individual_store_remove_individual (data->self, data->individual);
    }

  DEBUG ("Individual'%s' no longer active",
      folks_individual_get_alias (data->individual));

  individual_store_contact_set_active (data->self,
      data->individual, FALSE, TRUE);

  individual_store_contact_active_free (data);

  return FALSE;
}

static void
individual_avatar_pixbuf_received_cb (FolksIndividual *individual,
    GdkPixbuf *pixbuf,
    gpointer user_data)
{
  EmpathyIndividualStore *self = user_data;
  GList *iters, *l;

  iters = individual_store_find_contact (self, individual);

  for (l = iters; l; l = l->next)
    {
      gtk_tree_store_set (GTK_TREE_STORE (self), l->data,
          EMPATHY_INDIVIDUAL_STORE_COL_PIXBUF_AVATAR, pixbuf,
          -1);
    }
}

static void
individual_store_contact_update (EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  EmpathyIndividualStorePriv *priv;
  ShowActiveData *data;
  GtkTreeModel *model;
  GList *iters, *l;
  gboolean in_list;
  gboolean should_be_in_list;
  gboolean was_online = TRUE;
  gboolean now_online = FALSE;
  gboolean set_model = FALSE;
  gboolean do_remove = FALSE;
  gboolean do_set_active = FALSE;
  gboolean do_set_refresh = FALSE;
  gboolean show_avatar = FALSE;
  GdkPixbuf *pixbuf_status;

  priv = GET_PRIV (self);

  model = GTK_TREE_MODEL (self);

  iters = individual_store_find_contact (self, individual);
  if (!iters)
    {
      in_list = FALSE;
    }
  else
    {
      in_list = TRUE;
    }

  /* Get online state now. */
  now_online = folks_individual_is_online (individual);

  if (priv->show_offline || now_online)
    {
      should_be_in_list = TRUE;
    }
  else
    {
      should_be_in_list = FALSE;
    }

  if (!in_list && !should_be_in_list)
    {
      /* Nothing to do. */
      DEBUG ("Individual:'%s' in list:NO, should be:NO",
          folks_individual_get_alias (individual));

      g_list_foreach (iters, (GFunc) gtk_tree_iter_free, NULL);
      g_list_free (iters);
      return;
    }
  else if (in_list && !should_be_in_list)
    {
      DEBUG ("Individual:'%s' in list:YES, should be:NO",
          folks_individual_get_alias (individual));

      if (priv->show_active)
        {
          do_remove = TRUE;
          do_set_active = TRUE;
          do_set_refresh = TRUE;

          set_model = TRUE;
          DEBUG ("Remove item (after timeout)");
        }
      else
        {
          DEBUG ("Remove item (now)!");
          individual_store_remove_individual (self, individual);
        }
    }
  else if (!in_list && should_be_in_list)
    {
      DEBUG ("Individual'%s' in list:NO, should be:YES",
          folks_individual_get_alias (individual));

      individual_store_add_individual (self, individual);

      if (priv->show_active)
        {
          do_set_active = TRUE;

          DEBUG ("Set active (individual added)");
        }
    }
  else
    {
      DEBUG ("Individual'%s' in list:YES, should be:YES",
          folks_individual_get_alias (individual));

      /* Get online state before. */
      if (iters && g_list_length (iters) > 0)
        {
          gtk_tree_model_get (model, iters->data,
              EMPATHY_INDIVIDUAL_STORE_COL_IS_ONLINE, &was_online, -1);
        }

      /* Is this really an update or an online/offline. */
      if (priv->show_active)
        {
          if (was_online != now_online)
            {
              do_set_active = TRUE;
              do_set_refresh = TRUE;

              DEBUG ("Set active (individual updated %s)",
                  was_online ? "online  -> offline" : "offline -> online");
            }
          else
            {
              /* Was TRUE for presence updates. */
              /* do_set_active = FALSE;  */
              do_set_refresh = TRUE;

              DEBUG ("Set active (individual updated)");
            }
        }

      set_model = TRUE;
    }

  if (priv->show_avatars && !priv->is_compact)
    {
      show_avatar = TRUE;
    }

  empathy_pixbuf_avatar_from_individual_scaled_async (individual, 32, 32,
      individual_avatar_pixbuf_received_cb, self);
  pixbuf_status =
      empathy_individual_store_get_individual_status_icon (self, individual);

  for (l = iters; l && set_model; l = l->next)
    {
      gtk_tree_store_set (GTK_TREE_STORE (self), l->data,
          EMPATHY_INDIVIDUAL_STORE_COL_ICON_STATUS, pixbuf_status,
          EMPATHY_INDIVIDUAL_STORE_COL_PIXBUF_AVATAR_VISIBLE, show_avatar,
          EMPATHY_INDIVIDUAL_STORE_COL_NAME,
            folks_individual_get_alias (individual),
          EMPATHY_INDIVIDUAL_STORE_COL_PRESENCE_TYPE,
            folks_individual_get_presence_type (individual),
          EMPATHY_INDIVIDUAL_STORE_COL_STATUS,
            folks_individual_get_presence_message (individual),
          EMPATHY_INDIVIDUAL_STORE_COL_CAN_AUDIO_CALL,
            folks_individual_get_capabilities (individual) &
              FOLKS_CAPABILITIES_FLAGS_AUDIO,
          EMPATHY_INDIVIDUAL_STORE_COL_CAN_VIDEO_CALL,
            folks_individual_get_capabilities (individual) &
              FOLKS_CAPABILITIES_FLAGS_VIDEO,
          EMPATHY_INDIVIDUAL_STORE_COL_COMPACT, priv->is_compact,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, FALSE,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_ONLINE, now_online,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, FALSE,
          -1);
    }

  if (priv->show_active && do_set_active)
    {
      individual_store_contact_set_active (self, individual, do_set_active,
          do_set_refresh);

      if (do_set_active)
        {
          data =
              individual_store_contact_active_new (self, individual,
              do_remove);
          g_timeout_add_seconds (ACTIVE_USER_SHOW_TIME,
              (GSourceFunc) individual_store_contact_active_cb, data);
        }
    }

  /* FIXME: when someone goes online then offline quickly, the
   * first timeout sets the user to be inactive and the second
   * timeout removes the user from the contact list, really we
   * should remove the first timeout.
   */
  g_list_foreach (iters, (GFunc) gtk_tree_iter_free, NULL);
  g_list_free (iters);
}

static void
individual_store_contact_updated_cb (FolksIndividual *individual,
    GParamSpec *param,
    EmpathyIndividualStore *self)
{
  DEBUG ("Individual'%s' updated, checking roster is in sync...",
      folks_individual_get_alias (individual));

  individual_store_contact_update (self, individual);
}

static void
individual_store_add_individual_and_connect (EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  g_signal_connect (individual, "notify::avatar",
      G_CALLBACK (individual_store_contact_updated_cb), self);
  g_signal_connect (individual, "notify::presence-type",
      G_CALLBACK (individual_store_contact_updated_cb), self);
  g_signal_connect (individual, "notify::presence-message",
      G_CALLBACK (individual_store_contact_updated_cb), self);
  g_signal_connect (individual, "notify::alias",
      G_CALLBACK (individual_store_contact_updated_cb), self);
  g_signal_connect (individual, "notify::capabilities",
      G_CALLBACK (individual_store_contact_updated_cb), self);

  individual_store_add_individual (self, individual);
}

static void
individual_store_remove_individual_and_disconnect (
    EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  g_signal_handlers_disconnect_by_func (individual,
      G_CALLBACK (individual_store_contact_updated_cb), self);

  individual_store_remove_individual (self, individual);
}

static void
individual_store_members_changed_cb (EmpathyIndividualManager *manager,
    gchar *message,
    GList *added,
    GList *removed,
    guint reason,
    EmpathyIndividualStore *self)
{
  GList *l;

  for (l = added; l; l = l->next)
    {
      DEBUG ("Individual %s %s", folks_individual_get_id (l->data), "added");

      individual_store_add_individual_and_connect (self, l->data);
    }
  for (l = removed; l; l = l->next)
    {
      DEBUG ("Individual %s %s",
          folks_individual_get_id (l->data), "removed");

      individual_store_remove_individual_and_disconnect (self, l->data);
    }
}

static void
individual_store_favourites_changed_cb (EmpathyIndividualManager *manager,
    FolksIndividual *individual,
    gboolean is_favourite,
    EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  priv = GET_PRIV (self);

  DEBUG ("Individual %s is %s a favourite",
      folks_individual_get_id (individual),
      is_favourite ? "now" : "no longer");

  individual_store_remove_individual (self, individual);
  individual_store_add_individual (self, individual);
}

static void
individual_store_groups_changed_cb (EmpathyIndividualManager *manager,
    FolksIndividual *individual,
    gchar *group,
    gboolean is_member,
    EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;
  gboolean show_active;

  priv = GET_PRIV (self);

  DEBUG ("Updating groups for individual %s",
      folks_individual_get_id (individual));

  /* We do this to make sure the groups are correct, if not, we
   * would have to check the groups already set up for each
   * contact and then see what has been updated.
   */
  show_active = priv->show_active;
  priv->show_active = FALSE;
  individual_store_remove_individual (self, individual);
  individual_store_add_individual (self, individual);
  priv->show_active = show_active;
}

static gboolean
individual_store_manager_setup (gpointer user_data)
{
  EmpathyIndividualStore *self = user_data;
  EmpathyIndividualStorePriv *priv = GET_PRIV (self);
  GList *individuals;

  /* Signal connection. */

  /* TODO: implement */
  DEBUG ("handling individual renames unimplemented");

  g_signal_connect (priv->manager,
      "members-changed",
      G_CALLBACK (individual_store_members_changed_cb), self);

  g_signal_connect (priv->manager,
      "favourites-changed",
      G_CALLBACK (individual_store_favourites_changed_cb), self);

  g_signal_connect (priv->manager,
      "groups-changed",
      G_CALLBACK (individual_store_groups_changed_cb), self);

  /* Add contacts already created. */
  individuals = empathy_individual_manager_get_members (priv->manager);
  if (individuals != NULL && FOLKS_IS_INDIVIDUAL (individuals->data))
    {
      individual_store_members_changed_cb (priv->manager, "initial add",
          individuals, NULL, 0, self);
      g_list_free (individuals);
    }

  priv->setup_idle_id = 0;
  return FALSE;
}

static void
individual_store_set_individual_manager (EmpathyIndividualStore *self,
    EmpathyIndividualManager *manager)
{
  EmpathyIndividualStorePriv *priv = GET_PRIV (self);

  priv->manager = g_object_ref (manager);

  /* Let a chance to have all properties set before populating */
  priv->setup_idle_id = g_idle_add (individual_store_manager_setup, self);
}

static void
individual_store_member_renamed_cb (EmpathyIndividualManager *manager,
    FolksIndividual *old_individual,
    FolksIndividual *new_individual,
    guint reason,
    gchar *message,
    EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  priv = GET_PRIV (self);

  DEBUG ("Individual %s renamed to %s",
      folks_individual_get_id (old_individual),
      folks_individual_get_id (new_individual));

  /* add the new contact */
  individual_store_add_individual_and_connect (self, new_individual);

  /* remove old contact */
  individual_store_remove_individual_and_disconnect (self, old_individual);
}

static void
individual_store_dispose (GObject *object)
{
  EmpathyIndividualStorePriv *priv = GET_PRIV (object);
  GList *contacts, *l;

  if (priv->dispose_has_run)
    return;
  priv->dispose_has_run = TRUE;

  contacts = empathy_individual_manager_get_members (priv->manager);
  for (l = contacts; l; l = l->next)
    {
      g_signal_handlers_disconnect_by_func (l->data,
          G_CALLBACK (individual_store_contact_updated_cb), object);
    }
  g_list_free (contacts);

  g_signal_handlers_disconnect_by_func (priv->manager,
      G_CALLBACK (individual_store_member_renamed_cb), object);
  g_signal_handlers_disconnect_by_func (priv->manager,
      G_CALLBACK (individual_store_members_changed_cb), object);
  g_signal_handlers_disconnect_by_func (priv->manager,
      G_CALLBACK (individual_store_favourites_changed_cb), object);
  g_signal_handlers_disconnect_by_func (priv->manager,
      G_CALLBACK (individual_store_groups_changed_cb), object);
  g_object_unref (priv->manager);

  if (priv->inhibit_active)
    {
      g_source_remove (priv->inhibit_active);
    }

  if (priv->setup_idle_id != 0)
    {
      g_source_remove (priv->setup_idle_id);
    }

  g_hash_table_destroy (priv->status_icons);
  G_OBJECT_CLASS (empathy_individual_store_parent_class)->dispose (object);
}

static void
individual_store_get_property (GObject *object,
    guint param_id,
    GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualStorePriv *priv;

  priv = GET_PRIV (object);

  switch (param_id)
    {
    case PROP_INDIVIDUAL_MANAGER:
      g_value_set_object (value, priv->manager);
      break;
    case PROP_SHOW_OFFLINE:
      g_value_set_boolean (value, priv->show_offline);
      break;
    case PROP_SHOW_AVATARS:
      g_value_set_boolean (value, priv->show_avatars);
      break;
    case PROP_SHOW_PROTOCOLS:
      g_value_set_boolean (value, priv->show_protocols);
      break;
    case PROP_SHOW_GROUPS:
      g_value_set_boolean (value, priv->show_groups);
      break;
    case PROP_IS_COMPACT:
      g_value_set_boolean (value, priv->is_compact);
      break;
    case PROP_SORT_CRITERIUM:
      g_value_set_enum (value, priv->sort_criterium);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
    };
}

static void
individual_store_set_property (GObject *object,
    guint param_id,
    const GValue *value,
    GParamSpec *pspec)
{
  EmpathyIndividualStorePriv *priv;

  priv = GET_PRIV (object);

  switch (param_id)
    {
    case PROP_INDIVIDUAL_MANAGER:
      individual_store_set_individual_manager (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_object (value));
      break;
    case PROP_SHOW_OFFLINE:
      empathy_individual_store_set_show_offline (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_boolean (value));
      break;
    case PROP_SHOW_AVATARS:
      empathy_individual_store_set_show_avatars (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_boolean (value));
      break;
    case PROP_SHOW_PROTOCOLS:
      empathy_individual_store_set_show_protocols (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_boolean (value));
      break;
    case PROP_SHOW_GROUPS:
      empathy_individual_store_set_show_groups (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_boolean (value));
      break;
    case PROP_IS_COMPACT:
      empathy_individual_store_set_is_compact (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_boolean (value));
      break;
    case PROP_SORT_CRITERIUM:
      empathy_individual_store_set_sort_criterium (EMPATHY_INDIVIDUAL_STORE
          (object), g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
      break;
    };
}

static void
empathy_individual_store_class_init (EmpathyIndividualStoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = individual_store_dispose;
  object_class->get_property = individual_store_get_property;
  object_class->set_property = individual_store_set_property;

  g_object_class_install_property (object_class,
      PROP_INDIVIDUAL_MANAGER,
      g_param_spec_object ("individual-manager",
          "The individual manager",
          "The individual manager",
          EMPATHY_TYPE_INDIVIDUAL_MANAGER,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_OFFLINE,
      g_param_spec_boolean ("show-offline",
          "Show Offline",
          "Whether contact list should display "
          "offline contacts", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_AVATARS,
      g_param_spec_boolean ("show-avatars",
          "Show Avatars",
          "Whether contact list should display "
          "avatars for contacts", TRUE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_PROTOCOLS,
      g_param_spec_boolean ("show-protocols",
          "Show Protocols",
          "Whether contact list should display "
          "protocols for contacts", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_SHOW_GROUPS,
      g_param_spec_boolean ("show-groups",
          "Show Groups",
          "Whether contact list should display "
          "contact groups", TRUE, G_PARAM_READWRITE));
  g_object_class_install_property (object_class,
      PROP_IS_COMPACT,
      g_param_spec_boolean ("is-compact",
          "Is Compact",
          "Whether the contact list is in compact mode or not",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (object_class,
      PROP_SORT_CRITERIUM,
      g_param_spec_enum ("sort-criterium",
          "Sort citerium",
          "The sort criterium to use for sorting the contact list",
          EMPATHY_TYPE_INDIVIDUAL_STORE_SORT,
          EMPATHY_INDIVIDUAL_STORE_SORT_NAME, G_PARAM_READWRITE));

  g_type_class_add_private (object_class,
      sizeof (EmpathyIndividualStorePriv));
}

static gint
get_position (const char **strv,
    const char *str)
{
  int i;

  for (i = 0; strv[i] != NULL; i++)
    {
      if (!tp_strdiff (strv[i], str))
        return i;
    }

  return -1;
}

static gint
compare_separator_and_groups (gboolean is_separator_a,
    gboolean is_separator_b,
    const gchar *name_a,
    const gchar *name_b,
    FolksIndividual *individual_a,
    FolksIndividual *individual_b,
    gboolean fake_group_a,
    gboolean fake_group_b)
{
  /* these two lists are the sorted list of fake groups to include at the
   * top and bottom of the roster */
  const char *top_groups[] = {
    EMPATHY_INDIVIDUAL_STORE_FAVORITE,
    NULL
  };

  const char *bottom_groups[] = {
    EMPATHY_INDIVIDUAL_STORE_UNGROUPED,
    NULL
  };

  if (is_separator_a || is_separator_b)
    {
      /* We have at least one separator */
      if (is_separator_a)
        {
          return -1;
        }
      else if (is_separator_b)
        {
          return 1;
        }
    }

  /* One group and one contact */
  if (!individual_a && individual_b)
    {
      return 1;
    }
  else if (individual_a && !individual_b)
    {
      return -1;
    }
  else if (!individual_a && !individual_b)
    {
      gboolean a_in_top, b_in_top, a_in_bottom, b_in_bottom;

      a_in_top = fake_group_a && tp_strv_contains (top_groups, name_a);
      b_in_top = fake_group_b && tp_strv_contains (top_groups, name_b);
      a_in_bottom = fake_group_a && tp_strv_contains (bottom_groups, name_a);
      b_in_bottom = fake_group_b && tp_strv_contains (bottom_groups, name_b);

      if (a_in_top && b_in_top)
        {
          /* compare positions */
          return CLAMP (get_position (top_groups, name_a) -
              get_position (top_groups, name_b), -1, 1);
        }
      else if (a_in_bottom && b_in_bottom)
        {
          /* compare positions */
          return CLAMP (get_position (bottom_groups, name_a) -
              get_position (bottom_groups, name_b), -1, 1);
        }
      else if (a_in_top || b_in_bottom)
        {
          return -1;
        }
      else if (b_in_top || a_in_bottom)
        {
          return 1;
        }
      else
        {
          return g_utf8_collate (name_a, name_b);
        }
    }

  /* Two contacts, ordering depends of the sorting policy */
  return 0;
}

static gint
individual_store_contact_sort (FolksIndividual *individual_a,
    FolksIndividual *individual_b)
{
  gint ret_val;

  g_return_val_if_fail (individual_a != NULL || individual_b != NULL, 0);

  /* alias */
  ret_val = g_utf8_collate (folks_individual_get_alias (individual_a),
      folks_individual_get_alias (individual_b));

  if (ret_val != 0)
    goto out;

  /* identifier */
  ret_val = g_utf8_collate (folks_individual_get_id (individual_a),
      folks_individual_get_id (individual_b));

  if (ret_val != 0)
    goto out;

out:
  return ret_val;
}

static gint
individual_store_state_sort_func (GtkTreeModel *model,
    GtkTreeIter *iter_a,
    GtkTreeIter *iter_b,
    gpointer user_data)
{
  gint ret_val;
  FolksIndividual *individual_a, *individual_b;
  gchar *name_a, *name_b;
  gboolean is_separator_a, is_separator_b;
  gboolean fake_group_a, fake_group_b;
  FolksPresenceType folks_presence_type_a, folks_presence_type_b;
  TpConnectionPresenceType tp_presence_a, tp_presence_b;

  gtk_tree_model_get (model, iter_a,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name_a,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual_a,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator_a,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, &fake_group_a, -1);
  gtk_tree_model_get (model, iter_b,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name_b,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual_b,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator_b,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, &fake_group_b, -1);

  if (individual_a == NULL || individual_b == NULL)
    {
      ret_val = compare_separator_and_groups (is_separator_a, is_separator_b,
          name_a, name_b, individual_a, individual_b, fake_group_a,
          fake_group_b);
      goto free_and_out;
    }

  /* If we managed to get this far, we can start looking at
   * the presences.
   */
  folks_presence_type_a = folks_individual_get_presence_type (individual_a);
  folks_presence_type_b = folks_individual_get_presence_type (individual_b);
  tp_presence_a = empathy_folks_presence_type_to_tp (folks_presence_type_a);
  tp_presence_b = empathy_folks_presence_type_to_tp (folks_presence_type_b);

  ret_val = -tp_connection_presence_type_cmp_availability (tp_presence_a,
      tp_presence_b);

  if (ret_val == 0)
    {
      /* Fallback: compare by name et al. */
      ret_val = individual_store_contact_sort (individual_a, individual_b);
    }

free_and_out:
  g_free (name_a);
  g_free (name_b);

  if (individual_a)
    {
      g_object_unref (individual_a);
    }

  if (individual_b)
    {
      g_object_unref (individual_b);
    }

  return ret_val;
}

static gint
individual_store_name_sort_func (GtkTreeModel *model,
    GtkTreeIter *iter_a,
    GtkTreeIter *iter_b,
    gpointer user_data)
{
  gchar *name_a, *name_b;
  FolksIndividual *individual_a, *individual_b;
  gboolean is_separator_a = FALSE, is_separator_b = FALSE;
  gint ret_val;
  gboolean fake_group_a, fake_group_b;

  gtk_tree_model_get (model, iter_a,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name_a,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual_a,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator_a,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, &fake_group_a, -1);
  gtk_tree_model_get (model, iter_b,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name_b,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual_b,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator_b,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, &fake_group_b, -1);

  if (individual_a == NULL || individual_b == NULL)
    ret_val = compare_separator_and_groups (is_separator_a, is_separator_b,
        name_a, name_b, individual_a, individual_b, fake_group_a, fake_group_b);
  else
    ret_val = individual_store_contact_sort (individual_a, individual_b);

  if (individual_a)
    {
      g_object_unref (individual_a);
    }

  if (individual_b)
    {
      g_object_unref (individual_b);
    }

  return ret_val;
}

static void
individual_store_setup (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;
  GType types[] = {
    GDK_TYPE_PIXBUF,            /* Status pixbuf */
    GDK_TYPE_PIXBUF,            /* Avatar pixbuf */
    G_TYPE_BOOLEAN,             /* Avatar pixbuf visible */
    G_TYPE_STRING,              /* Name */
    G_TYPE_UINT,                /* Presence type */
    G_TYPE_STRING,              /* Status string */
    G_TYPE_BOOLEAN,             /* Compact view */
    FOLKS_TYPE_INDIVIDUAL,      /* Individual type */
    G_TYPE_BOOLEAN,             /* Is group */
    G_TYPE_BOOLEAN,             /* Is active */
    G_TYPE_BOOLEAN,             /* Is online */
    G_TYPE_BOOLEAN,             /* Is separator */
    G_TYPE_BOOLEAN,             /* Can make audio calls */
    G_TYPE_BOOLEAN,             /* Can make video calls */
    EMPATHY_TYPE_INDIVIDUAL_MANAGER_FLAGS,      /* Flags */
    G_TYPE_BOOLEAN,             /* Is a fake group */
  };

  priv = GET_PRIV (self);

  gtk_tree_store_set_column_types (GTK_TREE_STORE (self),
      EMPATHY_INDIVIDUAL_STORE_COL_COUNT, types);

  /* Set up sorting */
  gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (self),
      EMPATHY_INDIVIDUAL_STORE_COL_NAME,
      individual_store_name_sort_func, self, NULL);
  gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (self),
      EMPATHY_INDIVIDUAL_STORE_COL_STATUS,
      individual_store_state_sort_func, self, NULL);

  priv->sort_criterium = EMPATHY_INDIVIDUAL_STORE_SORT_NAME;
  empathy_individual_store_set_sort_criterium (self, priv->sort_criterium);
}

static gboolean
individual_store_inibit_active_cb (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  priv = GET_PRIV (self);

  priv->show_active = TRUE;
  priv->inhibit_active = 0;

  return FALSE;
}

static void
empathy_individual_store_init (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      EMPATHY_TYPE_INDIVIDUAL_STORE, EmpathyIndividualStorePriv);

  self->priv = priv;
  priv->show_avatars = TRUE;
  priv->show_groups = TRUE;
  priv->show_protocols = FALSE;
  priv->inhibit_active =
      g_timeout_add_seconds (ACTIVE_USER_WAIT_TO_ENABLE_TIME,
      (GSourceFunc) individual_store_inibit_active_cb, self);
  priv->status_icons =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  individual_store_setup (self);
}

EmpathyIndividualStore *
empathy_individual_store_new (EmpathyIndividualManager *manager)
{
  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_MANAGER (manager), NULL);

  return g_object_new (EMPATHY_TYPE_INDIVIDUAL_STORE,
      "individual-manager", manager, NULL);
}

EmpathyIndividualManager *
empathy_individual_store_get_manager (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), FALSE);

  priv = GET_PRIV (self);

  return priv->manager;
}

gboolean
empathy_individual_store_get_show_offline (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), FALSE);

  priv = GET_PRIV (self);

  return priv->show_offline;
}

void
empathy_individual_store_set_show_offline (EmpathyIndividualStore *self,
    gboolean show_offline)
{
  EmpathyIndividualStorePriv *priv;
  GList *contacts, *l;
  gboolean show_active;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self));

  priv = GET_PRIV (self);

  priv->show_offline = show_offline;
  show_active = priv->show_active;

  /* Disable temporarily. */
  priv->show_active = FALSE;

  contacts = empathy_individual_manager_get_members (priv->manager);
  for (l = contacts; l; l = l->next)
    {
      individual_store_contact_update (self, l->data);
    }
  g_list_free (contacts);

  /* Restore to original setting. */
  priv->show_active = show_active;

  g_object_notify (G_OBJECT (self), "show-offline");
}

gboolean
empathy_individual_store_get_show_avatars (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), TRUE);

  priv = GET_PRIV (self);

  return priv->show_avatars;
}

static gboolean
individual_store_update_list_mode_foreach (GtkTreeModel *model,
    GtkTreePath *path,
    GtkTreeIter *iter,
    EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;
  gboolean show_avatar = FALSE;
  FolksIndividual *individual;
  GdkPixbuf *pixbuf_status;

  priv = GET_PRIV (self);

  if (priv->show_avatars && !priv->is_compact)
    {
      show_avatar = TRUE;
    }

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_INDIVIDUAL, &individual, -1);

  if (individual == NULL)
    {
      return FALSE;
    }
  /* get icon from hash_table */
  pixbuf_status =
      empathy_individual_store_get_individual_status_icon (self, individual);

  gtk_tree_store_set (GTK_TREE_STORE (self), iter,
      EMPATHY_INDIVIDUAL_STORE_COL_ICON_STATUS, pixbuf_status,
      EMPATHY_INDIVIDUAL_STORE_COL_PIXBUF_AVATAR_VISIBLE, show_avatar,
      EMPATHY_INDIVIDUAL_STORE_COL_COMPACT, priv->is_compact, -1);

  g_object_unref (individual);

  return FALSE;
}

void
empathy_individual_store_set_show_avatars (EmpathyIndividualStore *self,
    gboolean show_avatars)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self));

  priv = GET_PRIV (self);

  priv->show_avatars = show_avatars;

  model = GTK_TREE_MODEL (self);

  gtk_tree_model_foreach (model,
      (GtkTreeModelForeachFunc)
      individual_store_update_list_mode_foreach, self);

  g_object_notify (G_OBJECT (self), "show-avatars");
}

gboolean
empathy_individual_store_get_show_protocols (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), TRUE);

  priv = GET_PRIV (self);

  return priv->show_protocols;
}

void
empathy_individual_store_set_show_protocols (EmpathyIndividualStore *self,
    gboolean show_protocols)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self));

  priv = GET_PRIV (self);

  priv->show_protocols = show_protocols;

  model = GTK_TREE_MODEL (self);

  gtk_tree_model_foreach (model,
      (GtkTreeModelForeachFunc)
      individual_store_update_list_mode_foreach, self);

  g_object_notify (G_OBJECT (self), "show-protocols");
}

gboolean
empathy_individual_store_get_show_groups (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), TRUE);

  priv = GET_PRIV (self);

  return priv->show_groups;
}

void
empathy_individual_store_set_show_groups (EmpathyIndividualStore *self,
    gboolean show_groups)
{
  EmpathyIndividualStorePriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self));

  priv = GET_PRIV (self);

  if (priv->show_groups == show_groups)
    {
      return;
    }

  priv->show_groups = show_groups;

  if (priv->setup_idle_id == 0)
    {
      /* Remove all contacts and add them back, not optimized but
       * that's the easy way :)
       *
       * This is only done if there's not a pending setup idle
       * callback, otherwise it will race and the contacts will get
       * added twice */
      GList *contacts;

      gtk_tree_store_clear (GTK_TREE_STORE (self));
      contacts = empathy_individual_manager_get_members (priv->manager);

      individual_store_members_changed_cb (priv->manager,
          "re-adding members: toggled group visibility",
          contacts, NULL, 0, self);
      g_list_free (contacts);
    }

  g_object_notify (G_OBJECT (self), "show-groups");
}

gboolean
empathy_individual_store_get_is_compact (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), TRUE);

  priv = GET_PRIV (self);

  return priv->is_compact;
}

void
empathy_individual_store_set_is_compact (EmpathyIndividualStore *self,
    gboolean is_compact)
{
  EmpathyIndividualStorePriv *priv;
  GtkTreeModel *model;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self));

  priv = GET_PRIV (self);

  priv->is_compact = is_compact;

  model = GTK_TREE_MODEL (self);

  gtk_tree_model_foreach (model,
      (GtkTreeModelForeachFunc)
      individual_store_update_list_mode_foreach, self);

  g_object_notify (G_OBJECT (self), "is-compact");
}

EmpathyIndividualStoreSort
empathy_individual_store_get_sort_criterium (EmpathyIndividualStore *self)
{
  EmpathyIndividualStorePriv *priv;

  g_return_val_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self), 0);

  priv = GET_PRIV (self);

  return priv->sort_criterium;
}

void
empathy_individual_store_set_sort_criterium (EmpathyIndividualStore *self,
    EmpathyIndividualStoreSort sort_criterium)
{
  EmpathyIndividualStorePriv *priv;

  g_return_if_fail (EMPATHY_IS_INDIVIDUAL_STORE (self));

  priv = GET_PRIV (self);

  priv->sort_criterium = sort_criterium;

  switch (sort_criterium)
    {
    case EMPATHY_INDIVIDUAL_STORE_SORT_STATE:
      gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (self),
          EMPATHY_INDIVIDUAL_STORE_COL_STATUS, GTK_SORT_ASCENDING);
      break;

    case EMPATHY_INDIVIDUAL_STORE_SORT_NAME:
      gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (self),
          EMPATHY_INDIVIDUAL_STORE_COL_NAME, GTK_SORT_ASCENDING);
      break;
    }

  g_object_notify (G_OBJECT (self), "sort-criterium");
}

gboolean
empathy_individual_store_row_separator_func (GtkTreeModel *model,
    GtkTreeIter *iter,
    gpointer data)
{
  gboolean is_separator = FALSE;

  g_return_val_if_fail (GTK_IS_TREE_MODEL (model), FALSE);

  gtk_tree_model_get (model, iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_SEPARATOR, &is_separator, -1);

  return is_separator;
}

gchar *
empathy_individual_store_get_parent_group (GtkTreeModel *model,
    GtkTreePath *path,
    gboolean *path_is_group,
    gboolean *is_fake_group)
{
  GtkTreeIter parent_iter, iter;
  gchar *name = NULL;
  gboolean is_group;
  gboolean fake;

  g_return_val_if_fail (GTK_IS_TREE_MODEL (model), NULL);

  if (path_is_group)
    {
      *path_is_group = FALSE;
    }

  if (!gtk_tree_model_get_iter (model, &iter, path))
    {
      return NULL;
    }

  gtk_tree_model_get (model, &iter,
      EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
      EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name, -1);

  if (!is_group)
    {
      g_free (name);
      name = NULL;

      if (!gtk_tree_model_iter_parent (model, &parent_iter, &iter))
        {
          return NULL;
        }

      iter = parent_iter;

      gtk_tree_model_get (model, &iter,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_GROUP, &is_group,
          EMPATHY_INDIVIDUAL_STORE_COL_NAME, &name,
          EMPATHY_INDIVIDUAL_STORE_COL_IS_FAKE_GROUP, &fake, -1);
      if (!is_group)
        {
          g_free (name);
          return NULL;
        }
    }

  if (path_is_group)
    {
      *path_is_group = TRUE;
    }

  if (is_fake_group != NULL)
    *is_fake_group = fake;

  return name;
}

static GdkPixbuf *
individual_store_get_individual_status_icon_with_icon_name (
    EmpathyIndividualStore *self,
    FolksIndividual *individual,
    const gchar *status_icon_name)
{
  GdkPixbuf *pixbuf_status = NULL;
  EmpathyIndividualStorePriv *priv;
  const gchar *protocol_name = NULL;
  gchar *icon_name = NULL;
  GList *personas, *l;
  guint contact_count;
  EmpathyContact *contact = NULL;
  gboolean show_protocols_here;

  priv = GET_PRIV (self);

  personas = folks_individual_get_personas (individual);
  for (l = personas, contact_count = 0; l; l = l->next)
    {
      if (TPF_IS_PERSONA (l->data))
        contact_count++;

      if (contact_count > 1)
        break;
    }

  show_protocols_here = priv->show_protocols && (contact_count == 1);
  if (show_protocols_here)
    {
      contact = empathy_contact_from_folks_individual (individual);
      protocol_name = empathy_protocol_name_for_contact (contact);
      icon_name = g_strdup_printf ("%s-%s", status_icon_name, protocol_name);
    }
  else
    {
      icon_name = g_strdup_printf ("%s", status_icon_name);
    }
  if (pixbuf_status == NULL)
    {
      pixbuf_status =
          empathy_pixbuf_contact_status_icon_with_icon_name (contact,
          status_icon_name, priv->show_protocols);
      if (pixbuf_status != NULL)
        {
          g_hash_table_insert (priv->status_icons,
              g_strdup (icon_name), pixbuf_status);
        }
    }

  g_free (icon_name);
  return pixbuf_status;
}

GdkPixbuf *
empathy_individual_store_get_individual_status_icon (
    EmpathyIndividualStore *self,
    FolksIndividual *individual)
{
  GdkPixbuf *pixbuf_status = NULL;
  const gchar *status_icon_name = NULL;

  status_icon_name = empathy_icon_name_for_individual (individual);
  if (status_icon_name == NULL)
    return NULL;

  pixbuf_status =
      individual_store_get_individual_status_icon_with_icon_name (self,
      individual, status_icon_name);

  return pixbuf_status;
}
