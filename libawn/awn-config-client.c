/*
 *  Copyright (C) 2007 Mark Lee <avant-wn@lazymalevolence.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA 02111-1307, USA.
 *
 *  Author : Mark Lee <avant-wn@lazymalevolence.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#ifdef USE_GCONF
#include <gconf/gconf-client.h>
#define AWN_GCONF_KEY_PREFIX "/apps/avant-window-navigator"
#else
#ifdef USE_EGG_CHECKSUM 
#include "egg/eggchecksum.h"
#else
#include <glib/gchecksum.h>
#endif
#endif

/**
 * SECTION: awn-config-client
 * @short_description: The configuration API for both Awn proper and
 * Awn applets.
 * @include libawn/awn-config-client.h
 *
 * A configuration wrapper API that supports both a GConf backend, as well as
 * a GKeyFile-based backend.  Used for both Awn proper and its applets.
 */
#include "libawn/awn-config-client.h"
#include "libawn/awn-vfs.h"

struct _AwnConfigClient
{
#ifdef USE_GCONF
	GConfClient *client;
#else
	GKeyFile *client;
	GData *notify_funcs;
	AwnVfsMonitor *file_monitor;
	gchar *checksum;
	GKeyFile *schema;
#endif
	gchar *path; /* GConf: key prefix; GKeyFile: .ini path */
};

/**
 * AwnConfigClientNotifyData:
 * @callback: The notification callback.
 * @data: Extra data passed to the callback, as defined in the call
 * to awn_config_client_notify_add().
 *
 * A utility structure used to pass callback metadata in the configuration
 * backend implementations.
 */
typedef struct {
#ifdef USE_GCONF
	AwnConfigClient *client;
#endif
	AwnConfigClientNotifyFunc callback;
	gpointer data;
} AwnConfigClientNotifyData;

#ifndef USE_GCONF
static void awn_config_client_gkeyfile_new_schema (AwnConfigClient *client, gchar *base_name);
static void awn_config_client_load_data (AwnConfigClient *client);
static void awn_config_client_save (AwnConfigClient *client, GError **err);
static void awn_config_client_reload (AwnVfsMonitor *monitor, gchar *monitor_path, gchar *event_path, AwnVfsMonitorEvent event, AwnConfigClient *client);
#endif

static AwnConfigClient *awn_config_client_new_with_path (gchar *path)
{
	AwnConfigClient *client = g_new (AwnConfigClient, 1);
	client->path = path;
#ifdef USE_GCONF
	client->client = gconf_client_get_default ();
#else
	client->client = g_key_file_new ();
	GError *err = NULL;
	if (g_file_test (client->path, G_FILE_TEST_EXISTS)) {
		awn_config_client_load_data (client);
	} else {
		g_message ("Creating the config file for you.");
		awn_config_client_save (client, &err);
		if (err) {
			g_warning ("Error loading the config file: '%s'", err->message);
			g_error_free (err);
		}
	}
	client->file_monitor = awn_vfs_monitor_add (client->path, AWN_VFS_MONITOR_FILE,
	                                            (AwnVfsMonitorFunc)awn_config_client_reload, client);
	g_datalist_init (&(client->notify_funcs));
#endif
	return client;
}

static gpointer
_awn_config_client_copy (gpointer boxed)
{
	return boxed;
}

static void
_awn_config_client_free (gpointer boxed)
{
	if (boxed) {
		awn_config_client_unref (AWN_CONFIG_CLIENT (boxed));
	}
}

GType awn_config_client_get_type (void)
{
	static GType type = 0;

	if (type == 0) {
		type = g_boxed_type_register_static ("AwnConfigClient",
						     _awn_config_client_copy,
						     _awn_config_client_free);
	}

	return type;
}

/**
 * awn_config_client_new:
 *
 * Retrieves the configuration client for Awn proper.  If none exists,
 * one is created.
 * Returns: a singleton instance of #AwnConfigClient.
 */
AwnConfigClient *awn_config_client_new ()
{
	static AwnConfigClient *awn_dock_config = NULL;
	if (!awn_dock_config) {
#ifdef USE_GCONF
		awn_dock_config = awn_config_client_new_with_path (AWN_GCONF_KEY_PREFIX);
#else
		awn_dock_config = awn_config_client_new_with_path (g_build_filename (g_get_user_config_dir (), "awn", "awn.ini", NULL));
		awn_config_client_gkeyfile_new_schema (awn_dock_config, NULL);
#endif
	}
	return awn_dock_config;
}

/**
 * awn_config_client_new_for_applet:
 * @name: The name of the applet.
 *
 * Creates a configuration client for the applet named in the parameter.
 * Returns: an instance of #AwnConfigClient for the specified applet.
 */
AwnConfigClient *awn_config_client_new_for_applet (gchar *name)
{
#ifdef USE_GCONF
	return awn_config_client_new_with_path (g_strconcat (AWN_GCONF_KEY_PREFIX, "/applets/", name, NULL));
#else
	/* TODO: add ~/.config/awn/applets if it doesn't exist */
	AwnConfigClient *client = awn_config_client_new_with_path (g_build_filename (g_get_user_config_dir (), "awn", "applets", g_strconcat(name, ".ini", NULL), NULL));
	awn_config_client_gkeyfile_new_schema (client, name);
	return client;
#endif
}

/* returns a newly allocated string */
static gchar *awn_config_client_generate_key (AwnConfigClient *client, const gchar *group, const gchar *key)
{
	if (key == NULL) {
		return g_strconcat(client->path, "/", group, NULL);
	} else {
		return g_strconcat(client->path, "/", group, "/", key, NULL);
	}
}

#ifdef USE_GCONF
static void awn_config_client_notify_proxy (GConfClient *client, guint cid, GConfEntry *entry, AwnConfigClientNotifyData* notify)
{
	AwnConfigClientNotifyEntry *awn_entry = g_new (AwnConfigClientNotifyEntry, 1);
	GConfValue *value = NULL;
	value = gconf_entry_get_value (entry);
	if (value) {
		gchar **exploded = g_strsplit (gconf_entry_get_key (entry), "/", 0);
		guint exploded_len = g_strv_length (exploded);
		g_return_if_fail (exploded_len < 2);
		awn_entry->client = notify->client;
		awn_entry->group = g_strdup (exploded[exploded_len - 2]);
		awn_entry->key = g_strdup (exploded[exploded_len - 1]);
		g_strfreev (exploded);
		switch (value->type) {
			case GCONF_VALUE_BOOL:
				awn_entry->value.bool_val = gconf_value_get_bool (value);
				break;
			case GCONF_VALUE_FLOAT:
				awn_entry->value.float_val = (gfloat)gconf_value_get_float (value);
				break;
			case GCONF_VALUE_INT:
				awn_entry->value.int_val = gconf_value_get_int (value);
				break;
			case GCONF_VALUE_LIST:
				awn_entry->value.list_val = gconf_value_get_list (value);
				break;
			case GCONF_VALUE_STRING:
				awn_entry->value.str_val = (gchar*)gconf_value_get_string (value);
				break;
			default:
				/* FIXME do an error? */
				break;
		}
		(notify->callback) (awn_entry, notify->data);
		g_free (awn_entry->group);
		g_free (awn_entry->key);
	}
	g_free (awn_entry);
}
#else
/**
 * Generic function to free both the elements of a GSList and the GSList itself.
 */
static void awn_config_client_free_gslist (GSList *list)
{
	g_slist_foreach (list, (GFunc)g_free, NULL);
	g_slist_free (list);
}
/* if base_name is NULL, base_name defaults to "awn" */
static void awn_config_client_gkeyfile_new_schema (AwnConfigClient *client, gchar *base_name)
{
	if (!base_name) {
		base_name = "awn";
	}
	client->schema = g_key_file_new ();
	gchar *file_name = g_strconcat(base_name, ".schema-ini", NULL);
	gchar *schema_path = g_build_filename (DATADIR, "awn", "schemas", file_name, NULL);
	GError *err = NULL;
	if (!g_key_file_load_from_file (client->schema, schema_path,
	                                (G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS),
	                                &err)) {
		if (err) {
			g_error ("Error loading the schema file: '%s'", err->message);
			g_error_free (err);
		} else {
			g_error ("Error loading the schema file.");
		}
	}
	g_free (schema_path);
	g_free (file_name);
}
static AwnConfigValueType awn_config_client_get_value_type (AwnConfigClient *client, const gchar *group, const gchar *key, GError **err)
{
	AwnConfigValueType value_type;
	gchar *schema_group = g_strconcat (group, "/", key, NULL);
	if (g_key_file_has_group (client->schema, schema_group)) {
		if (g_key_file_has_key (client->schema, schema_group, "type", err)) {
			gchar *value = g_key_file_get_string (client->schema, schema_group, "type", err);
			if (g_ascii_strcasecmp (value, "bool") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_BOOL;
			} else if (g_ascii_strcasecmp (value, "float") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_FLOAT;
			} else if (g_ascii_strcasecmp (value, "int") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_INT;
			} else if (g_ascii_strcasecmp (value, "string") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_STRING;
			} else if (g_ascii_strcasecmp (value, "list-bool") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_LIST_BOOL;
			} else if (g_ascii_strcasecmp (value, "list-float") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_LIST_FLOAT;
			} else if (g_ascii_strcasecmp (value, "list-int") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_LIST_INT;
			} else if (g_ascii_strcasecmp (value, "list-string") == 0) {
				value_type = AWN_CONFIG_VALUE_TYPE_LIST_STRING;
			} else {
				value_type = AWN_CONFIG_VALUE_TYPE_NULL;
			}
		} else {
			g_error ("Invalid schema file for the config file '%s': all keys must have a value type.", client->path);
			value_type = AWN_CONFIG_VALUE_TYPE_NULL;
		}
	} else {
		value_type = AWN_CONFIG_VALUE_TYPE_NULL;
	}
	g_free (schema_group);
	return value_type;
}
static GSList *awn_config_client_get_gkeyfile_list_value (AwnConfigClient *client, const gchar *group, const gchar *key, AwnConfigListType list_type, GError **err)
{
	GSList *slist = NULL;
	gsize list_len;
	switch (list_type) {
		case AWN_CONFIG_CLIENT_LIST_TYPE_BOOL: {
			gboolean *list = g_key_file_get_boolean_list (client->client, group, key, &list_len, err);
			if (list) {
				gsize i;
				for (i = 0; i < list_len; i++) {
					slist = g_slist_append (slist, (gpointer)(&list[i]));
				}
			}
			break;
		} case AWN_CONFIG_CLIENT_LIST_TYPE_FLOAT: {
			gdouble *list = g_key_file_get_double_list (client->client, group, key, &list_len, err);
			if (list) {
				gsize i;
				for (i = 0; i < list_len; i++) {
					slist = g_slist_append (slist, (gpointer)(&list[i]));
				}
			}
			break;
		} case AWN_CONFIG_CLIENT_LIST_TYPE_INT: {
			gint *list = g_key_file_get_integer_list (client->client, group, key, &list_len, err);
			if (list) {
				gsize i;
				for (i = 0; i < list_len; i++) {
					slist = g_slist_append (slist, (gpointer)(&list[i]));
				}
			}
			break;
		} case AWN_CONFIG_CLIENT_LIST_TYPE_STRING: {
			gchar **list = g_key_file_get_string_list (client->client, group, key, &list_len, err);
			if (list) {
				gsize i;
				for (i = 0; i < list_len; i++) {
					slist = g_slist_append (slist, (gpointer)(list[i]));
				}
			}
			break;
		}
	}
	return slist;
}
static void awn_config_client_load_data (AwnConfigClient *client)
{
	GError *err = NULL;
	if (!g_key_file_load_from_file (client->client, client->path,
	                                (G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS),
	                                &err)) {
		if (err) {
			g_warning ("Error loading the config file: '%s'", err->message);
			g_error_free (err);
		} else {
			g_message ("Config file not found.  It will be created for you.");
		}
	}
	gchar *data;
	gsize len;
	data = g_key_file_to_data (client->client, &len, &err);
	if (err) {
		g_warning ("Could not serialize the loaded configuration data: '%s'", err->message);
		g_error_free (err);
		client->checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, "", 0);
	} else {
		client->checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, data, len);
		g_free (data);
	}
}
static void awn_config_client_save (AwnConfigClient *client, GError **err)
{
	gchar *data;
	gsize len;
	data = g_key_file_to_data (client->client, &len, err);
	if (!err || !*err) {
		g_file_set_contents (client->path, data, len, err);
		g_free (data);
	}
}
/* free the returned value when done */
static gchar **awn_config_client_string_lists_merge (gchar **list1, gsize list1_len, gchar **list2, gsize list2_len, gsize *ret_len)
{
	gchar **ret_list = g_malloc (sizeof(gchar*) * (list1_len + list2_len));
	gsize i, j;
	for (i = 0; i < list1_len; i++) {
		ret_list[i] = g_strdup (list1[i]);
	}
	*ret_len = list1_len;
	for (i = 0; i < list2_len; i++) {
		gchar *value = list2[i];
		gboolean value_exists = FALSE;
		for (j = 0; j < *ret_len; j++) {
			if (strcmp (value, ret_list[j]) == 0) {
				value_exists = TRUE;
				break;
			}
		}
		if (!value_exists) {
			ret_list[(*ret_len)++] = g_strdup (value);
		}
	}
	ret_list[*ret_len] = NULL;
	return ret_list;
}
static void awn_config_client_do_notify (AwnConfigClient *client, const gchar *group, const gchar *key)
{
	if (g_key_file_has_group (client->client, group) && g_key_file_has_key (client->client, group, key, NULL)) {
		AwnConfigClientNotifyEntry *entry = g_new0 (AwnConfigClientNotifyEntry, 1);
		entry->client = client;
		entry->group = (gchar*)group;
		entry->key = (gchar*)key;
		AwnConfigClientValue *value = g_malloc (sizeof (AwnConfigClientValue));
		GError *err = NULL;
		switch (awn_config_client_get_value_type (client, group, key, &err)) {
			case AWN_CONFIG_VALUE_TYPE_BOOL:
				value->bool_val = g_key_file_get_boolean (client->client, group, key, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_FLOAT:
				value->float_val = (gfloat)g_key_file_get_double (client->client, group, key, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_INT:
				value->int_val = g_key_file_get_integer (client->client, group, key, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_STRING:
				value->str_val = g_key_file_get_string (client->client, group, key, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_LIST_BOOL:
				value->list_val = awn_config_client_get_gkeyfile_list_value (client, group, key, AWN_CONFIG_CLIENT_LIST_TYPE_BOOL, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_LIST_FLOAT:
				value->list_val = awn_config_client_get_gkeyfile_list_value (client, group, key, AWN_CONFIG_CLIENT_LIST_TYPE_FLOAT, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_LIST_INT:
				value->list_val = awn_config_client_get_gkeyfile_list_value (client, group, key, AWN_CONFIG_CLIENT_LIST_TYPE_INT, NULL);
				break;
			case AWN_CONFIG_VALUE_TYPE_LIST_STRING:
				value->list_val = awn_config_client_get_gkeyfile_list_value (client, group, key, AWN_CONFIG_CLIENT_LIST_TYPE_STRING, NULL);
				break;
			default: /* NULL */
				return;
				break;
		}
		entry->value = *value;
		gchar *full_key = awn_config_client_generate_key (client, group, key);
		GSList *callbacks = g_datalist_get_data (&(client->notify_funcs), full_key);
		GSList *notify_iter;
		for (notify_iter = callbacks; notify_iter != NULL; notify_iter = g_slist_next (notify_iter)) {
			AwnConfigClientNotifyData *notify = (AwnConfigClientNotifyData*)notify_iter->data;
			(notify->callback) (entry, notify->data);
		}
		g_free (full_key);
		g_free (value);
		g_free (entry);
	} else {
	}
}
static void awn_config_client_reload (AwnVfsMonitor *monitor, gchar *monitor_path, gchar *event_path, AwnVfsMonitorEvent event, AwnConfigClient *client)
{
	switch (event) {
		case AWN_VFS_MONITOR_EVENT_CREATED:
		case AWN_VFS_MONITOR_EVENT_CHANGED: {
			gchar *old_checksum = client->checksum;
			GKeyFile *old_client = client->client;
			client->client = g_key_file_new ();
			awn_config_client_load_data (client);
			if (!old_checksum || strcmp (old_checksum, client->checksum) != 0) {
				/* iterate through all of the groups/keys to see if anything's changed */
				/* merge groups into one array */
				gsize og_len, ng_len, i, groups_len;
				gchar **old_groups = g_key_file_get_groups (old_client, &og_len);
				gchar **new_groups = g_key_file_get_groups (client->client, &ng_len);
				gchar **all_groups = awn_config_client_string_lists_merge (old_groups, og_len, new_groups, ng_len, &groups_len);
				for (i = 0; i < groups_len; i++) {
					gchar *group = all_groups[i];
					if (!g_key_file_has_group (old_client, group)) {
						/* group added */
						gsize keys_len, j;
						gchar **keys = g_key_file_get_keys (client->client, group, &keys_len, NULL);
						for (j = 0; j < keys_len; j++) {
							awn_config_client_do_notify (client, group, keys[j]);
						}
						g_strfreev (keys);
					} else if (!g_key_file_has_group (client->client, group)) {
						/* group removed */
						gsize keys_len, j;
						gchar **keys = g_key_file_get_keys (old_client, group, &keys_len, NULL);
						for (j = 0; j < keys_len; j++) {
							awn_config_client_do_notify (client, group, keys[j]);
						}
						g_strfreev (keys);
					} else {
						/* check for added/deleted/changed keys */
						gsize ok_len, nk_len, j, keys_len;
						gchar **okeys = g_key_file_get_keys (old_client, group, &ok_len, NULL);
						gchar **nkeys = g_key_file_get_keys (client->client, group, &nk_len, NULL);
						gchar **all_keys = awn_config_client_string_lists_merge (okeys, ok_len, nkeys, nk_len, &keys_len);
						for (j = 0; j < keys_len; j++) {
							gchar *key = all_keys[j];
							if (!g_key_file_has_key (old_client, group, key, NULL)) {
								/* key added */
								awn_config_client_do_notify (client, group, key);
							} else if (!g_key_file_has_key (client->client, group, key, NULL)) {
								/* key removed */
								awn_config_client_do_notify (client, group, key);
							} else {
								gchar *old_value = g_key_file_get_value (old_client, group, key, NULL);
								gchar *new_value = g_key_file_get_value (client->client, group, key, NULL);
								if (strcmp (old_value, new_value) != 0) {
									awn_config_client_do_notify (client, group, key);
								}
								g_free (new_value);
								g_free (old_value);
							}
						}
						g_strfreev (nkeys);
						g_strfreev (okeys);
					}
				}
				g_strfreev (new_groups);
				g_strfreev (old_groups);
			}
			g_key_file_free (old_client);
			g_free (old_checksum);
			break;
		} case AWN_VFS_MONITOR_EVENT_DELETED: {
			g_warning ("Your configuration file was deleted.");
			break;
		}
	}
}
#endif

/**
 * awn_config_client_clear:
 * @client: The configuration client that is to be used.
 * @err: The pointer to the #GError structure that contains an error
 * message on failure.
 *
 * Removes all of the configuration entries from the client.
 */
void awn_config_client_clear (AwnConfigClient *client, GError **err)
{
#ifdef USE_GCONF
	/* only do it for applets on gconf */
	if (strcmp (AWN_GCONF_KEY_PREFIX, client->path) != 0) {
		gconf_client_remove_dir (client->client, client->path, err);
	}
#else
	gsize i, group_len;
	gchar **group_list = g_key_file_get_groups (client->client, &group_len);
	for (i = 0; i < group_len; i++) {
		gsize j, key_len;
		gchar **key_list = g_key_file_get_keys (client->client, group_list[i], &key_len, NULL);
		for (j = 0; j < key_len; j++) {
			g_key_file_remove_key (client->client, group_list[i], key_list[j], NULL);
		}
		g_strfreev (key_list);
		g_key_file_remove_group (client->client, group_list[i], NULL);
	}
	g_strfreev (group_list);
	awn_config_client_save (client, err);
#endif
}

/**
 * awn_config_client_ensure_group:
 * @client: The configuration client to be queried.
 * @group: The name of the group.
 *
 * Ensures that the @group named has been created in the configuration backend.
 */
void awn_config_client_ensure_group (AwnConfigClient *client, const gchar *group)
{
#ifdef USE_GCONF
	gchar *gconf_key = awn_config_client_generate_key (client, group, NULL);
	if (!gconf_client_dir_exists (client->client, gconf_key, NULL)) {
		gconf_client_add_dir (client->client, gconf_key, GCONF_CLIENT_PRELOAD_NONE, NULL);
	}
	g_free (gconf_key);
#else
	if (!g_key_file_has_group (client->client, group)) {
		g_warning ("The configuration file does not currently contain the group '%s'.  It will be created when a configuration option is set in that group.", group);
	}
#endif
}

/**
 * awn_config_client_notify_add:
 * @client: The configuration client that is to be used.
 * @group: The name of the group.
 * @key: The name of the key.
 * @callback: The function that is called when the key value has been modified.
 * @data: Extra data that is passed to the callback.
 *
 * Associates a callback function with a group and a key, which is called
 * when that key's value has been modified in some way.
 */
void awn_config_client_notify_add (AwnConfigClient *client, const gchar *group, 
				   const gchar *key, AwnConfigClientNotifyFunc callback,
				   gpointer data)
{
	AwnConfigClientNotifyData *notify = g_new0 (AwnConfigClientNotifyData, 1);
	notify->callback = callback;
	notify->data = data;
	gchar *full_key = awn_config_client_generate_key (client, group, key);
#ifdef USE_GCONF
	notify->client = client;
	gconf_client_notify_add (client->client, full_key, (GConfClientNotifyFunc)awn_config_client_notify_proxy, notify, NULL, NULL);
#else
	GQuark quark = g_quark_from_string (full_key);
	GSList *funcs = g_datalist_id_get_data (&(client->notify_funcs), quark);
	funcs = g_slist_append (funcs, notify);
	g_datalist_id_set_data_full (&(client->notify_funcs), quark, funcs, (GDestroyNotify)awn_config_client_free_gslist);
#endif
	g_free (full_key);
}

/**
 * awn_config_client_entry_exists:
 * @client: The configuration client that is to be queried.
 * @group: The name of the group.
 * @key: The name of the key.
 *
 * Determines whether the group and key exists in the configuration backend.
 * Returns: %TRUE on success, %FALSE otherwise.
 */
gboolean awn_config_client_entry_exists (AwnConfigClient *client, const gchar *group, const gchar *key)
{
#ifdef USE_GCONF
	GConfValue *value = NULL;
	gchar *gconf_key = awn_config_client_generate_key (client, group, key);
	value = gconf_client_get(client->client, gconf_key, NULL);
	g_free (gconf_key);
	return value != NULL;
#else
	return g_key_file_has_key (client->client, group, key, NULL);
#endif
}

/**
 * awn_config_client_get_bool:
 * @client: The configuration client that is to be queried.
 * @group: The name of the group.
 * @key: The name of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Retrieves the value (as a boolean) of the specified group and key.
 * Returns: a boolean value.
 */
gboolean awn_config_client_get_bool (AwnConfigClient *client, const gchar *group, const gchar *key, GError **err)
{
#ifdef USE_GCONF
	gchar *gconf_key = awn_config_client_generate_key (client, group, key);
	gboolean value = gconf_client_get_bool (client->client, gconf_key, err);
	g_free (gconf_key);
	return value;
#else
	return g_key_file_get_boolean (client->client, group, key, err);
#endif
}

/**
 * awn_config_client_set_bool:
 * @client: The configuration client that is to be used.
 * @group: The name of the group.
 * @key: The name of the key.
 * @value: The new value of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Changes the value (as a boolean) of the specified group and key.
 */
void awn_config_client_set_bool (AwnConfigClient *client, const gchar *group, const gchar *key, gboolean value, GError **err)
{
	gchar *full_key = awn_config_client_generate_key (client, group, key);
#ifdef USE_GCONF
	gconf_client_set_bool (client->client, full_key, value, err);
#else
	g_key_file_set_boolean (client->client, group, key, value);
	awn_config_client_save (client, err);
#endif
	g_free (full_key);
}

/**
 * awn_config_client_get_float:
 * @client: The configuration client that is to be queried.
 * @group: The name of the group.
 * @key: The name of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Retrieves the value (as a float) of the specified group and key.
 * Returns: a float value.
 */
gfloat awn_config_client_get_float (AwnConfigClient *client, const gchar *group, const gchar *key, GError **err)
{
#ifdef USE_GCONF
	gchar *gconf_key = awn_config_client_generate_key (client, group, key);
	gfloat value = gconf_client_get_float (client->client, gconf_key, err);
	g_free (gconf_key);
	return value;
#else
	return (gfloat)g_key_file_get_double (client->client, group, key, err);
#endif
}

/**
 * awn_config_client_set_float:
 * @client: The configuration client that is to be used.
 * @group: The name of the group.
 * @key: The name of the key.
 * @value: The new value of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Changes the value (as a float) of the specified group and key.
 * If you need double precision, use a string.
 */
void awn_config_client_set_float (AwnConfigClient *client, const gchar *group, const gchar *key, gfloat value, GError **err)
{
#ifdef USE_GCONF
	gchar *full_key = awn_config_client_generate_key (client, group, key);
	gconf_client_set_float (client->client, full_key, value, err);
	g_free (full_key);
#else
	g_key_file_set_double (client->client, group, key, (gdouble)value);
	awn_config_client_save (client, err);
#endif
}

/**
 * awn_config_client_get_int:
 * @client: The configuration client that is to be queried.
 * @group: The name of the group.
 * @key: The name of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Retrieves the value (as an integer) of the specified group and key.
 * Returns: an integer value.
 */
gint awn_config_client_get_int (AwnConfigClient *client, const gchar *group, const gchar *key, GError **err)
{
#ifdef USE_GCONF
	gchar *gconf_key = awn_config_client_generate_key (client, group, key);
	gint value = gconf_client_get_int (client->client, gconf_key, err);
	g_free (gconf_key);
	return value;
#else
	return g_key_file_get_integer (client->client, group, key, err);
#endif
}

/**
 * awn_config_client_set_int:
 * @client: The configuration client that is to be used.
 * @group: The name of the group.
 * @key: The name of the key.
 * @value: The new value of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Changes the value (as an integer) of the specified group and key.
 */
void awn_config_client_set_int (AwnConfigClient *client, const gchar *group, const gchar *key, gint value, GError **err)
{
#ifdef USE_GCONF
	gchar *full_key = awn_config_client_generate_key (client, group, key);
	gconf_client_set_int (client->client, full_key, value, err);
	g_free (full_key);
#else
	g_key_file_set_integer (client->client, group, key, value);
	awn_config_client_save (client, err);
#endif
}

/**
 * awn_config_client_get_string:
 * @client: The configuration client that is to be queried.
 * @group: The name of the group.
 * @key: The name of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Retrieves the value (as a string) of the specified group and key.
 * Returns: a newly allocated string value.  The caller is responsible
 * for freeing the memory.
 */
gchar *awn_config_client_get_string (AwnConfigClient *client, const gchar *group, const gchar *key, GError **err)
{
#ifdef USE_GCONF
	gchar *gconf_key = awn_config_client_generate_key (client, group, key);
	gchar *value = gconf_client_get_string (client->client, gconf_key, err);
	g_free (gconf_key);
	return value;
#else
	return g_key_file_get_string (client->client, group, key, err);
#endif
}

/**
 * awn_config_client_set_string:
 * @client: The configuration client that is to be used.
 * @group: The name of the group.
 * @key: The name of the key.
 * @value: The new value of the key.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Changes the value (as a string) of the specified group and key.
 */
void awn_config_client_set_string (AwnConfigClient *client, const gchar *group, const gchar *key, gchar *value, GError **err)
{
#ifdef USE_GCONF
	gchar *full_key = awn_config_client_generate_key (client, group, key);
	gconf_client_set_string (client->client, full_key, value, err);
	g_free (full_key);
#else
	g_key_file_set_string (client->client, group, key, value);
	awn_config_client_save (client, err);
#endif
}

/**
 * awn_config_client_get_list:
 * @client: The configuration client that is to be queried.
 * @group: The name of the group.
 * @key: The name of the key.
 * @list_type: The value type of every item in the list.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Retrieves the value (as a #GSList) of the specified group and key.
 * Returns: a newly allocated list value.  The caller is responsible
 * for freeing the memory.
 */
GSList *awn_config_client_get_list (AwnConfigClient *client, const gchar *group, const gchar *key, AwnConfigListType list_type, GError **err)
{
#ifdef USE_GCONF
	GConfValueType value_type;
	switch (list_type) {
		case AWN_CONFIG_CLIENT_LIST_TYPE_BOOL:
			value_type = GCONF_VALUE_BOOL;
			break;
		case AWN_CONFIG_CLIENT_LIST_TYPE_FLOAT:
			value_type = GCONF_VALUE_FLOAT;
			break;
		case AWN_CONFIG_CLIENT_LIST_TYPE_INT:
			value_type = GCONF_VALUE_INT;
			break;
		case AWN_CONFIG_CLIENT_LIST_TYPE_STRING:
			value_type = GCONF_VALUE_STRING;
			break;
	}
	gchar *gconf_key = awn_config_client_generate_key (client, group, key);
	GSList *value = gconf_client_get_list (client->client, gconf_key, value_type, err);
	g_free (gconf_key);
	return value;
#else
	return awn_config_client_get_gkeyfile_list_value (client, group, key, list_type, err);
#endif
}

/**
 * awn_config_client_set_list:
 * @client: The configuration client that is to be used.
 * @group: The name of the group.
 * @key: The name of the key.
 * @value: The new value of the key.
 * @list_type: The value type of every item in the list.
 * @err: A pointer to a #GError structure, which contains an error message
 * if the function fails.
 *
 * Changes the value (as a list of values) of the specified group and key.
 */
void awn_config_client_set_list (AwnConfigClient *client, const gchar *group, const gchar *key, AwnConfigListType list_type, GSList *value, GError **err)
{
#ifdef USE_GCONF
	gchar *full_key = awn_config_client_generate_key (client, group, key);
	GConfValueType value_type;
	switch (list_type) {
		case AWN_CONFIG_CLIENT_LIST_TYPE_BOOL:
			value_type = GCONF_VALUE_BOOL;
			break;
		case AWN_CONFIG_CLIENT_LIST_TYPE_FLOAT:
			value_type = GCONF_VALUE_FLOAT;
			break;
		case AWN_CONFIG_CLIENT_LIST_TYPE_INT:
			value_type = GCONF_VALUE_INT;
			break;
		case AWN_CONFIG_CLIENT_LIST_TYPE_STRING:
			value_type = GCONF_VALUE_STRING;
			break;
	}
	gconf_client_set_list (client->client, full_key, value_type, value, err);
	g_free (full_key);
#else
	gsize list_len = g_slist_length (value);
	gsize i;
	GSList *iter = NULL;
	gboolean list_set = FALSE;
	switch (list_type) {
		case AWN_CONFIG_CLIENT_LIST_TYPE_BOOL: {
			gboolean *list = g_malloc (sizeof(gboolean) * list_len);
			for (iter = value, i = 0; iter != NULL; iter = g_slist_next(iter), i++) {
				list[i] = *(gboolean*)(iter->data);
			}
			/* GKeyFile doesn't like setting NULL lists */
			if (list) {
				list_set = TRUE;
				g_key_file_set_boolean_list (client->client, group, key, list, list_len);
			}
			g_free (list);
			break;
		} case AWN_CONFIG_CLIENT_LIST_TYPE_FLOAT: {
			gdouble *list = g_malloc (sizeof(gdouble) * list_len);
			for (iter = value, i = 0; iter != NULL; iter = g_slist_next(iter), i++) {
				list[i] = *(gdouble*)(iter->data);
			}
			/* GKeyFile doesn't like setting NULL lists */
			if (list) {
				list_set = TRUE;
				g_key_file_set_double_list (client->client, group, key, list, list_len);
			}
			g_free (list);
			break;
		} case AWN_CONFIG_CLIENT_LIST_TYPE_INT: {
			gint *list = g_malloc (sizeof(gint) * list_len);
			for (iter = value, i = 0; iter != NULL; iter = g_slist_next(iter), i++) {
				list[i] = *(gint*)(iter->data);
			}
			/* GKeyFile doesn't like setting NULL lists */
			if (list) {
				list_set = TRUE;
				g_key_file_set_integer_list (client->client, group, key, list, list_len);
			}
			g_free (list);
			break;
		} case AWN_CONFIG_CLIENT_LIST_TYPE_STRING: {
			gchar **list = g_malloc (sizeof(gchar*) * list_len);
			for (iter = value, i = 0; iter != NULL; iter = g_slist_next(iter), i++) {
				list[i] = (gchar*)(iter->data);
			}
			/* GKeyFile doesn't like setting NULL lists */
			if (list) {
				list_set = TRUE;
				g_key_file_set_string_list (client->client, group, key, (const gchar * const *)list, list_len);
			}
			g_free (list);
			break;
		}
	}
	if (list_set) {
		awn_config_client_save (client, err);
	}
#endif
}

/**
 * awn_config_client_unref:
 * @client: The configuration client structure to free.
 *
 * Frees the configuration client structure from memory.
 */
void awn_config_client_unref (AwnConfigClient *client)
{
#ifdef USE_GCONF
	g_object_unref (client->client);
	if (strcmp (AWN_GCONF_KEY_PREFIX, client->path) != 0) {
		g_free (client->path);
	}
#else
	g_key_file_free (client->schema);
	g_datalist_clear (&(client->notify_funcs));
	awn_vfs_monitor_remove (client->file_monitor);
	g_key_file_free (client->client);
	g_free (client->path);
#endif
	g_free (client);
}

/*  vim: set noet ts=8 sw=8 sts=8 : */
