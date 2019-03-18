/* nmcli - command-line tool to control NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2010 - 2018 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-meta-setting-desc.h"

#include <stdlib.h>
#include <arpa/inet.h>

#include "nm-common-macros.h"
#include "nm-utils/nm-enum-utils.h"
#include "nm-libnm-core-utils.h"

#include "nm-vpn-helpers.h"
#include "nm-client-utils.h"
#include "nm-meta-setting-access.h"

/*****************************************************************************/

static char *secret_flags_to_string (guint32 flags, NMMetaAccessorGetType get_type);

#define ALL_SECRET_FLAGS \
	(NM_SETTING_SECRET_FLAG_NONE | \
	 NM_SETTING_SECRET_FLAG_AGENT_OWNED | \
	 NM_SETTING_SECRET_FLAG_NOT_SAVED | \
	 NM_SETTING_SECRET_FLAG_NOT_REQUIRED)

/*****************************************************************************/

static GType
_gobject_property_get_gtype (GObject *gobject, const char *property_name)
{
	GParamSpec *param_spec;

	param_spec = g_object_class_find_property (G_OBJECT_GET_CLASS (gobject), property_name);
	if (param_spec)
		return param_spec->value_type;
	g_return_val_if_reached (G_TYPE_INVALID);
}

static GType
_gtype_property_get_gtype (GType gtype, const char *property_name)
{
	/* given @gtype, a type for a GObject, lookup the property @property_name
	 * and return its value_type. */
	if (G_TYPE_IS_CLASSED (gtype)) {
		GParamSpec *param_spec;
		nm_auto_unref_gtypeclass GTypeClass *gtypeclass = g_type_class_ref (gtype);

		if (G_IS_OBJECT_CLASS (gtypeclass)) {
			param_spec = g_object_class_find_property (G_OBJECT_CLASS (gtypeclass), property_name);
			if (param_spec)
				return param_spec->value_type;
		}
	}
	g_return_val_if_reached (G_TYPE_INVALID);
}

/*****************************************************************************/

static NMIPAddress *
_parse_ip_address (int family, const char *address, GError **error)
{
	gs_free char *ip_str = NULL;
	const int MAX_PREFIX = (family == AF_INET) ? 32 : 128;
	NMIPAddress *addr;
	char *plen;
	int prefix;
	GError *local = NULL;

	g_return_val_if_fail (address, NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	ip_str = g_strstrip (g_strdup (address));

	prefix = MAX_PREFIX;

	plen = strchr (ip_str, '/');
	if (plen) {
		*plen++ = '\0';
		if ((prefix = _nm_utils_ascii_str_to_int64 (plen, 10, 1, MAX_PREFIX, -1)) == -1) {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("invalid prefix '%s'; <1-%d> allowed"), plen, MAX_PREFIX);
			return NULL;
		}
	}

	addr = nm_ip_address_new (family, ip_str, prefix, &local);
	if (!addr) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
		             _("invalid IP address: %s"), local->message);
		g_clear_error (&local);
	}
	return addr;
}

static NMIPRoute *
_parse_ip_route (int family,
                 const char *str,
                 GError **error)
{
	const int MAX_PREFIX = (family == AF_INET) ? 32 : 128;
	const char *next_hop = NULL;
	int prefix;
	NMIPRoute *route = NULL;
	GError *local = NULL;
	gint64 metric = -1;
	guint i;
	gs_free const char **routev = NULL;
	gs_free char *str_clean_free = NULL;
	const char *str_clean;
	gs_free char *dest_clone = NULL;
	const char *dest;
	const char *plen;
	gs_unref_hashtable GHashTable *attrs = NULL;
#define ROUTE_SYNTAX _("The valid syntax is: 'ip[/prefix] [next-hop] [metric] [attribute=val]... [,ip[/prefix] ...]'")

	nm_assert (NM_IN_SET (family, AF_INET, AF_INET6));
	nm_assert (str);
	nm_assert (!error || !*error);

	str_clean = nm_strstrip_avoid_copy (str, &str_clean_free);
	routev = nm_utils_strsplit_set (str_clean, " \t", FALSE);
	if (!routev) {
		g_set_error (error, 1, 0,
		             "'%s' is not valid. %s",
		             str, ROUTE_SYNTAX);
		return NULL;
	}

	dest = routev[0];
	plen = strchr (dest, '/');  /* prefix delimiter */
	if (plen) {
		dest_clone = g_strdup (dest);
		plen = &dest_clone[plen - dest];
		dest = dest_clone;
		*((char *) plen) = '\0';
		plen++;
	}
	prefix = MAX_PREFIX;
	if (plen) {
		if ((prefix = _nm_utils_ascii_str_to_int64 (plen, 10, 1, MAX_PREFIX, -1)) == -1) {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("invalid prefix '%s'; <1-%d> allowed"),
			             plen, MAX_PREFIX);
			return NULL;
		}
	}

	for (i = 1; routev[i]; i++) {
		gint64 tmp64;

		if (nm_utils_ipaddr_valid (family, routev[i])) {
			if (metric != -1 || attrs) {
				g_set_error (error, 1, 0, _("the next hop ('%s') must be first"), routev[i]);
				return NULL;
			}
			next_hop = routev[i];
		} else if ((tmp64 = _nm_utils_ascii_str_to_int64 (routev[i], 10, 0, G_MAXUINT32, -1)) != -1) {
			if (attrs) {
				g_set_error (error, 1, 0, _("the metric ('%s') must be before attributes"), routev[i]);
				return NULL;
			}
			metric = tmp64;
		} else if (strchr (routev[i], '=')) {
			GHashTableIter iter;
			char *iter_key;
			GVariant *iter_value;
			gs_unref_hashtable GHashTable *tmp_attrs = NULL;

			tmp_attrs = nm_utils_parse_variant_attributes (routev[i], ' ', '=', FALSE,
			                                               nm_ip_route_get_variant_attribute_spec(),
			                                               error);
			if (!tmp_attrs) {
				g_prefix_error (error, "invalid option '%s': ", routev[i]);
				return NULL;
			}

			if (!attrs) {
				attrs = g_hash_table_new_full (nm_str_hash,
				                               g_str_equal,
				                               g_free,
				                               (GDestroyNotify) g_variant_unref);
			}

			g_hash_table_iter_init (&iter, tmp_attrs);
			while (g_hash_table_iter_next (&iter, (gpointer *) &iter_key, (gpointer *) &iter_value)) {

				/* need to sink the reference, because nm_utils_parse_variant_attributes() returns
				 * floating refs. */
				g_variant_ref_sink (iter_value);

				if (!nm_ip_route_attribute_validate (iter_key, iter_value, family, NULL, error)) {
					g_prefix_error (error, "%s: ", iter_key);
					return NULL;
				}
				g_hash_table_insert (attrs, iter_key, iter_value);
				g_hash_table_iter_steal (&iter);
			}
		} else {
			g_set_error (error, 1, 0, "%s", ROUTE_SYNTAX);
			return NULL;
		}
	}

	route = nm_ip_route_new (family, dest, prefix, next_hop, metric, &local);
	if (!route) {
		g_set_error (error, 1, 0,
		             _("invalid route: %s. %s"), local->message, ROUTE_SYNTAX);
		g_clear_error (&local);
		return NULL;
	}

	if (attrs) {
		GHashTableIter iter;
		char *name;
		GVariant *variant;

		g_hash_table_iter_init (&iter, attrs);
		while (g_hash_table_iter_next (&iter, (gpointer *) &name, (gpointer *) &variant))
			nm_ip_route_set_attribute (route, name, variant);
	}

	return route;
}

static char *
_dump_team_link_watcher (NMTeamLinkWatcher *watcher)
{
	const char *name;
	NMTeamLinkWatcherArpPingFlags flags;
	GString *w_dump;

	if (!watcher)
		return NULL;

	w_dump = g_string_new (NULL);
	name = nm_team_link_watcher_get_name (watcher);
	g_string_append_printf (w_dump, "name=%s", name);

#define DUMP_WATCHER_INT(str, watcher, name, key) \
	G_STMT_START { \
		int _val = nm_team_link_watcher_get_##key (watcher); \
		\
		if (_val) \
			g_string_append_printf (str, " %s=%d", name, _val); \
	} G_STMT_END;

	if (nm_streq (name, NM_TEAM_LINK_WATCHER_ETHTOOL)) {
		DUMP_WATCHER_INT (w_dump, watcher, "delay-up", delay_up);
		DUMP_WATCHER_INT (w_dump, watcher, "delay-down", delay_down);
		return g_string_free (w_dump, FALSE);
	}
	/* NM_TEAM_LINK_WATCHER_NSNA_PING and NM_TEAM_LINK_WATCHER_ARP_PING */
	DUMP_WATCHER_INT (w_dump, watcher, "init-wait", init_wait);
	DUMP_WATCHER_INT (w_dump, watcher, "interval", interval);
	DUMP_WATCHER_INT (w_dump, watcher, "missed-max", missed_max);
	g_string_append_printf (w_dump, " target-host=%s",
	                        nm_team_link_watcher_get_target_host (watcher));

	if (nm_streq (name, NM_TEAM_LINK_WATCHER_NSNA_PING))
		return g_string_free (w_dump, FALSE);

	DUMP_WATCHER_INT (w_dump, watcher, "vlanid", vlanid);
#undef DUMP_WATCHER_INT
	g_string_append_printf (w_dump, " source-host=%s",
	                        nm_team_link_watcher_get_source_host (watcher));
	flags = nm_team_link_watcher_get_flags (watcher);
	if (flags & NM_TEAM_LINK_WATCHER_ARP_PING_FLAG_VALIDATE_ACTIVE)
		g_string_append_printf (w_dump, " validate-active=true");
	if (flags & NM_TEAM_LINK_WATCHER_ARP_PING_FLAG_VALIDATE_INACTIVE)
		g_string_append_printf (w_dump, " validate-inactive=true");
	if (flags & NM_TEAM_LINK_WATCHER_ARP_PING_FLAG_SEND_ALWAYS)
		g_string_append_printf (w_dump, "send-always=true");

	return g_string_free (w_dump, FALSE);
}

static NMTeamLinkWatcher *
_parse_team_link_watcher (const char *str,
                          GError **error)
{
	gs_free const char **watcherv = NULL;
	gs_free char *str_clean_free = NULL;
	const char *str_clean;
	guint i;
	gs_free const char *name = NULL;
	int val1 = 0, val2 = 0, val3 = 3, val4 = -1;
	gs_free const char *target_host = NULL;
	gs_free const char *source_host = NULL;
	NMTeamLinkWatcherArpPingFlags flags = 0;

	nm_assert (str);
	nm_assert (!error || !*error);

	str_clean = nm_strstrip_avoid_copy (str, &str_clean_free);
	watcherv = nm_utils_strsplit_set (str_clean, " \t", FALSE);
	if (!watcherv) {
		g_set_error (error, 1, 0, "'%s' is not valid", str);
		return NULL;
	}

	for (i = 0; watcherv[i]; i++) {
		gs_free const char **pair = NULL;

		pair = nm_utils_strsplit_set (watcherv[i], "=", FALSE);
		if (!pair) {
			g_set_error (error, 1, 0, "'%s' is not valid: %s", watcherv[i],
			             "properties should be specified as 'key=value'");
			return NULL;
		}
		if (!pair[1]) {
			g_set_error (error, 1, 0, "'%s' is not valid: %s", watcherv[i],
			             "missing key value");
			return NULL;
		}
		if (pair[2]) {
			g_set_error (error, 1, 0, "'%s' is not valid: %s", watcherv[i],
			             "properties should be specified as 'key=value'");
			return NULL;
		}

		if (nm_streq (pair[0], "name"))
			name = g_strdup (pair[1]);
		else if (   nm_streq (pair[0], "delay-up")
		         || nm_streq (pair[0], "init-wait"))
			val1 = _nm_utils_ascii_str_to_int64 (pair[1], 10, 0, G_MAXINT32, -1);
		else if (   nm_streq (pair[0], "delay-down")
		         || nm_streq (pair[0], "interval"))
			val2 = _nm_utils_ascii_str_to_int64 (pair[1], 10, 0, G_MAXINT32, -1);
		else if (nm_streq (pair[0], "missed-max"))
			val3 = _nm_utils_ascii_str_to_int64 (pair[1], 10, 0, G_MAXINT32, -1);
		else if (nm_streq (pair[0], "vlanid"))
			val4 = _nm_utils_ascii_str_to_int64 (pair[1], 10, -1, 4094, -2);
		else if (nm_streq (pair[0], "target-host"))
			target_host = g_strdup (pair[1]);
		else if (nm_streq (pair[0], "source-host"))
			source_host = g_strdup (pair[1]);
		else if (nm_streq (pair[0], "validate-active")) {
			if (nm_streq (pair[1], "true"))
				flags |= NM_TEAM_LINK_WATCHER_ARP_PING_FLAG_VALIDATE_ACTIVE;
		} else if (nm_streq (pair[0], "validate-inactive")) {
			if (nm_streq (pair[1], "true"))
				flags |= NM_TEAM_LINK_WATCHER_ARP_PING_FLAG_VALIDATE_INACTIVE;
		} else if (nm_streq (pair[0], "send-always")) {
			if (nm_streq (pair[1], "true"))
				flags |= NM_TEAM_LINK_WATCHER_ARP_PING_FLAG_SEND_ALWAYS;
		} else {
			g_set_error (error, 1, 0, "'%s' is not valid: %s", watcherv[i],
			             "unknown key");
			return NULL;
		}

		if ((val1 < 0) || (val2 < 0) || (val3 < 0)) {
			g_set_error (error, 1, 0, "'%s' is not valid: %s", watcherv[i],
			             "value is not a valid number [0, MAXINT]");
			return NULL;
		}
		if (val4 < -1) {
			g_set_error (error, 1, 0, "'%s' is not valid: %s", watcherv[i],
			             "value is not a valid number [-1, 4094]");
			return NULL;
		}
	}

	if (nm_streq0 (name, NM_TEAM_LINK_WATCHER_ETHTOOL))
		return nm_team_link_watcher_new_ethtool (val1, val2, error);
	else if (nm_streq0 (name, NM_TEAM_LINK_WATCHER_NSNA_PING))
		return nm_team_link_watcher_new_nsna_ping (val1, val2, val3, target_host, error);
	else if (nm_streq0 (name, NM_TEAM_LINK_WATCHER_ARP_PING))
		return nm_team_link_watcher_new_arp_ping2 (val1, val2, val3, val4, target_host, source_host, flags, error);

	if (!name)
		g_set_error (error, 1, 0, "link watcher name missing");
	else
		g_set_error (error, 1, 0, "unknown link watcher name: '%s'", name);
	return NULL;
}

/* Max priority values from libnm-core/nm-setting-vlan.c */
#define MAX_SKB_PRIO   G_MAXUINT32
#define MAX_8021P_PRIO 7  /* Max 802.1p priority */

/*
 * Parse VLAN priority mappings from the following format: 2:1,3:4,7:3
 * and verify if the priority numbers are valid
 *
 * Return: string array with split maps, or NULL on error
 * Caller is responsible for freeing the array.
 */
static char **
_parse_vlan_priority_maps (const char *priority_map,
                           NMVlanPriorityMap map_type,
                           gboolean allow_wildcard_to,
                           GError **error)
{
	gs_strfreev char **mapping = NULL;
	char **iter;

	nm_assert (priority_map);
	nm_assert (!error || !*error);

	mapping = g_strsplit (priority_map, ",", 0);
	for (iter = mapping; *iter; iter++) {
		if (!nm_utils_vlan_priority_map_parse_str (map_type,
		                                           *iter,
		                                           allow_wildcard_to,
		                                           NULL,
		                                           NULL,
		                                           NULL)) {
			g_set_error (error, 1, 0, _("invalid priority map '%s'"), *iter);
			return NULL;
		}
	}
	return g_steal_pointer (&mapping);
}

/*
 * nmc_proxy_check_script:
 * @script: file name with PAC script, or raw PAC Script data
 * @out_script: raw PAC Script (with removed new-line characters)
 * @error: location to store error, or %NULL
 *
 * Check PAC Script from @script parameter and return the checked/sanitized
 * config in @out_script.
 *
 * Returns: %TRUE if the script is valid, %FALSE if it is invalid
 */
static gboolean
nmc_proxy_check_script (const char *script, char **out_script, GError **error)
{
	enum {
		_PAC_SCRIPT_TYPE_GUESS,
		_PAC_SCRIPT_TYPE_FILE,
		_PAC_SCRIPT_TYPE_JSON,
	} desired_type = _PAC_SCRIPT_TYPE_GUESS;
	const char *filename = NULL;
	size_t c_len = 0;
	gs_free char *script_clone = NULL;

	*out_script = NULL;

	if (!script || !script[0])
		return TRUE;

	if (g_str_has_prefix (script, "file://")) {
		script += NM_STRLEN ("file://");
		desired_type = _PAC_SCRIPT_TYPE_FILE;
	} else if (g_str_has_prefix (script, "js://")) {
		script += NM_STRLEN ("js://");
		desired_type = _PAC_SCRIPT_TYPE_JSON;
	}

	if (NM_IN_SET (desired_type, _PAC_SCRIPT_TYPE_FILE, _PAC_SCRIPT_TYPE_GUESS)) {
		gs_free char *contents = NULL;

		if (!g_file_get_contents (script, &contents, &c_len, NULL)) {
			if (desired_type == _PAC_SCRIPT_TYPE_FILE) {
				g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
				             _("cannot read pac-script from file '%s'"),
				             script);
				return FALSE;
			}
		} else {
			if (c_len != strlen (contents)) {
				g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
				             _("file '%s' contains non-valid utf-8"),
				             script);
				return FALSE;
			}
			filename = script;
			script = script_clone = g_steal_pointer (&contents);
		}
	}

	if (   !strstr (script, "FindProxyForURL")
	    || !g_utf8_validate (script, -1, NULL)) {
		if (filename) {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("'%s' does not contain a valid PAC Script"), filename);
		} else {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("Not a valid PAC Script"));
		}
		return FALSE;
	}

	*out_script = (script == script_clone)
	              ? g_steal_pointer (&script_clone)
	              : g_strdup (script);
	return TRUE;
}

/*
 * nmc_team_check_config:
 * @config: file name with team config, or raw team JSON config data
 * @out_config: raw team JSON config data
 *   The value must be freed with g_free().
 * @error: location to store error, or %NUL
 *
 * Check team config from @config parameter and return the checked
 * config in @out_config.
 *
 * Returns: %TRUE if the config is valid, %FALSE if it is invalid
 */
static gboolean
nmc_team_check_config (const char *config, char **out_config, GError **error)
{
	enum {
		_TEAM_CONFIG_TYPE_GUESS,
		_TEAM_CONFIG_TYPE_FILE,
		_TEAM_CONFIG_TYPE_JSON,
	} desired_type = _TEAM_CONFIG_TYPE_GUESS;
	const char *filename = NULL;
	size_t c_len = 0;
	gs_free char *config_clone = NULL;

	*out_config = NULL;

	if (!config || !config[0])
		return TRUE;

	if (g_str_has_prefix (config, "file://")) {
		config += NM_STRLEN ("file://");
		desired_type = _TEAM_CONFIG_TYPE_FILE;
	} else if (g_str_has_prefix (config, "json://")) {
		config += NM_STRLEN ("json://");
		desired_type = _TEAM_CONFIG_TYPE_JSON;
	}

	if (NM_IN_SET (desired_type, _TEAM_CONFIG_TYPE_FILE, _TEAM_CONFIG_TYPE_GUESS)) {
		gs_free char *contents = NULL;

		if (!g_file_get_contents (config, &contents, &c_len, NULL)) {
			if (desired_type == _TEAM_CONFIG_TYPE_FILE) {
				g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
				             _("cannot read team config from file '%s'"),
				             config);
				return FALSE;
			}
		} else {
			if (c_len != strlen (contents)) {
				g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
				             _("team config file '%s' contains non-valid utf-8"),
				             config);
				return FALSE;
			}
			filename = config;
			config = config_clone = g_steal_pointer (&contents);
		}
	}

	if (!nm_utils_is_json_object (config, NULL)) {
		if (filename) {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("'%s' does not contain a valid team configuration"), filename);
		} else {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("team configuration must be a JSON object"));
		}
		return FALSE;
	}

	*out_config = (config == config_clone)
	              ? g_steal_pointer (&config_clone)
	              : g_strdup (config);
	return TRUE;
}

static const char *
_get_text_hidden (NMMetaAccessorGetType get_type)
{
	if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
		return _(NM_META_TEXT_HIDDEN);
	return NM_META_TEXT_HIDDEN;
}

/*****************************************************************************/

G_GNUC_PRINTF (4, 5)
static void
_env_warn_fcn (const NMMetaEnvironment *environment,
               gpointer environment_user_data,
               NMMetaEnvWarnLevel warn_level,
               const char *fmt_l10n,
               ...)
{
	va_list ap;

	if (!environment || !environment->warn_fcn)
		return;

	va_start (ap, fmt_l10n);
	environment->warn_fcn (environment,
	                       environment_user_data,
	                       warn_level,
	                       fmt_l10n,
	                       ap);
	va_end (ap);
}

/*****************************************************************************/

#define ARGS_DESCRIBE_FCN \
	const NMMetaPropertyInfo *property_info, char **out_to_free

#define ARGS_GET_FCN \
	const NMMetaPropertyInfo *property_info, const NMMetaEnvironment *environment, gpointer environment_user_data, NMSetting *setting, NMMetaAccessorGetType get_type, NMMetaAccessorGetFlags get_flags, NMMetaAccessorGetOutFlags *out_flags, gboolean *out_is_default, gpointer *out_to_free

#define ARGS_SET_FCN \
	const NMMetaPropertyInfo *property_info, const NMMetaEnvironment *environment, gpointer environment_user_data, NMSetting *setting, const char *value, GError **error

#define ARGS_REMOVE_FCN \
	const NMMetaPropertyInfo *property_info, const NMMetaEnvironment *environment, gpointer environment_user_data, NMSetting *setting, const char *value, GError **error

#define ARGS_COMPLETE_FCN \
	const NMMetaPropertyInfo *property_info, const NMMetaEnvironment *environment, gpointer environment_user_data, const NMMetaOperationContext *operation_context, const char *text, char ***out_to_free

#define ARGS_VALUES_FCN \
	const NMMetaPropertyInfo *property_info, char ***out_to_free

#define ARGS_SETTING_INIT_FCN \
	const NMMetaSettingInfoEditor *setting_info, NMSetting *setting, NMMetaAccessorSettingInitType init_type

#define _SET_FCN_DO_RESET_DEFAULT(value) \
	((value) == NULL)

#define RETURN_UNSUPPORTED_GET_TYPE() \
	G_STMT_START { \
		if (!NM_IN_SET (get_type, \
		                NM_META_ACCESSOR_GET_TYPE_PARSABLE, \
		                NM_META_ACCESSOR_GET_TYPE_PRETTY)) { \
			nm_assert_not_reached (); \
			return NULL; \
		} \
	} G_STMT_END;

#define RETURN_STR_TO_FREE(val) \
	G_STMT_START { \
		char *_val = (val); \
		return ((*(out_to_free)) = _val); \
	} G_STMT_END

static gboolean
_gobject_property_is_default (NMSetting *setting, const char *prop_name)
{
	nm_auto_unset_gvalue GValue v = G_VALUE_INIT;
	GParamSpec *pspec;
	GHashTable *ht;
	char **strv;

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (setting)),
	                                      prop_name);
	if (!G_IS_PARAM_SPEC (pspec))
		g_return_val_if_reached (FALSE);

	g_value_init (&v, pspec->value_type);
	g_object_get_property (G_OBJECT (setting), prop_name, &v);

	if (pspec->value_type == G_TYPE_STRV) {
		strv = g_value_get_boxed (&v);
		return !strv || !strv[0];
	} else if (pspec->value_type == G_TYPE_HASH_TABLE) {
		ht = g_value_get_boxed (&v);
		return !ht || !g_hash_table_size (ht);
	}

	return g_param_value_defaults (pspec, &v);
}

static gboolean
_gobject_property_reset_default (NMSetting *setting, const char *prop_name)
{
	nm_auto_unset_gvalue GValue v = G_VALUE_INIT;
	GParamSpec *pspec;

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (setting)),
	                                      prop_name);
	if (!G_IS_PARAM_SPEC (pspec))
		g_return_val_if_reached (FALSE);

	g_value_init (&v, pspec->value_type);
	g_param_value_defaults (pspec, &v);
	g_object_set_property (G_OBJECT (setting), prop_name, &v);
	return TRUE;
}

static gconstpointer
_get_fcn_nmc_with_default (ARGS_GET_FCN)
{
	const char *s;
	char *s_full;
	GValue val = G_VALUE_INIT;

	RETURN_UNSUPPORTED_GET_TYPE ();
	NM_SET_OUT (out_is_default, _gobject_property_is_default (setting, property_info->property_name));

	if (property_info->property_typ_data->subtype.get_with_default.fcn (setting)) {
		if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
			return _("(default)");
		return "";
	}

	g_value_init (&val, G_TYPE_STRING);
	g_object_get_property (G_OBJECT (setting), property_info->property_name, &val);
	s = g_value_get_string (&val);
	if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
		s_full = s ? g_strdup_printf ("\"%s\"", s) : g_strdup ("");
	else
		s_full = g_strdup (s && *s ? s : " ");
	g_value_unset (&val);

	RETURN_STR_TO_FREE (s_full);
}

static gconstpointer
_get_fcn_gobject_impl (const NMMetaPropertyInfo *property_info,
                       NMSetting *setting,
                       NMMetaAccessorGetType get_type,
                       gboolean *out_is_default,
                       gpointer *out_to_free)
{
	char *s;
	const char *s_c;
	GType gtype_prop;
	nm_auto_unset_gvalue GValue val = G_VALUE_INIT;

	RETURN_UNSUPPORTED_GET_TYPE ();
	NM_SET_OUT (out_is_default, _gobject_property_is_default (setting, property_info->property_name));

	gtype_prop = _gobject_property_get_gtype (G_OBJECT (setting), property_info->property_name);

	if (gtype_prop == G_TYPE_BOOLEAN) {
		gboolean b;

		g_value_init (&val, gtype_prop);
		g_object_get_property (G_OBJECT (setting), property_info->property_name, &val);
		b = g_value_get_boolean (&val);
		if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
			s_c = b ? _("yes") : _("no");
		else
			s_c = b ? "yes" : "no";
		return s_c;
	} else {
		/* Note that we register certain transform functions in nmc_value_transforms_register().
		 * This makes G_TYPE_STRV working.
		 *
		 * FIXME: that is particularly ugly because it's non-obvious which code relies
		 * on nmc_value_transforms_register(). Also, nmc_value_transforms_register() is
		 * in clients/cli, while we are here in clients/common. */
		g_value_init (&val, G_TYPE_STRING);
		g_object_get_property (G_OBJECT (setting), property_info->property_name, &val);
		s = g_value_dup_string (&val);
		RETURN_STR_TO_FREE (s);
	}
}

static gconstpointer
_get_fcn_gobject (ARGS_GET_FCN)
{
	return _get_fcn_gobject_impl (property_info, setting, get_type, out_is_default, out_to_free);
}

static gconstpointer
_get_fcn_gobject_int (ARGS_GET_FCN)
{
	GParamSpec *pspec;
	nm_auto_unset_gvalue GValue gval = G_VALUE_INIT;
	gboolean is_uint64 = FALSE;
	NMMetaSignUnsignInt64 v;
	guint base = 10;
	const NMMetaUtilsIntValueInfo *value_infos;
	char *return_str;

	RETURN_UNSUPPORTED_GET_TYPE ();

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (setting)), property_info->property_name);
	if (!G_IS_PARAM_SPEC (pspec))
		g_return_val_if_reached (FALSE);

	g_value_init (&gval, pspec->value_type);
	g_object_get_property (G_OBJECT (setting), property_info->property_name, &gval);
	NM_SET_OUT (out_is_default, g_param_value_defaults (pspec, &gval));
	switch (pspec->value_type) {
	case G_TYPE_INT:
		v.i64 = g_value_get_int (&gval);
		break;
	case G_TYPE_UINT:
		v.u64 = g_value_get_uint (&gval);
		is_uint64 = TRUE;
		break;
	case G_TYPE_INT64:
		v.i64 = g_value_get_int64 (&gval);
		break;
	case G_TYPE_UINT64:
		v.u64 = g_value_get_uint64 (&gval);
		is_uint64 = TRUE;
		break;
	default:
		g_return_val_if_reached (NULL);
		break;
	}

	if (   property_info->property_typ_data
	    && property_info->property_typ_data->subtype.gobject_int.base > 0) {
		base = property_info->property_typ_data->subtype.gobject_int.base;
	}

	switch (base) {
	case 10:
		if (is_uint64)
			return_str = g_strdup_printf ("%"G_GUINT64_FORMAT, v.u64);
		else
			return_str = g_strdup_printf ("%"G_GINT64_FORMAT, v.i64);
		break;
	case 16:
		if (is_uint64)
			return_str = g_strdup_printf ("0x%"G_GINT64_MODIFIER"x", v.u64);
		else
			return_str = g_strdup_printf ("0x%"G_GINT64_MODIFIER"x", (guint64) v.i64);
		break;
	default:
		return_str = NULL;
		g_assert_not_reached ();
	}

	if (   get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY
	    && property_info->property_typ_data
	    && (value_infos = property_info->property_typ_data->subtype.gobject_int.value_infos)) {
		for (; value_infos->nick; value_infos++) {
			if (   ( is_uint64 && value_infos->value.u64 == v.u64)
			    || (!is_uint64 && value_infos->value.i64 == v.i64)) {
				gs_free char *old_str = return_str;

				return_str = g_strdup_printf ("%s (%s)", old_str, value_infos->nick);
				break;
			}
		}
	}

	RETURN_STR_TO_FREE (return_str);
}

static gconstpointer
_get_fcn_gobject_mtu (ARGS_GET_FCN)
{
	guint32 mtu;

	RETURN_UNSUPPORTED_GET_TYPE ();

	if (   !property_info->property_typ_data
	    || !property_info->property_typ_data->subtype.mtu.get_fcn)
		return _get_fcn_gobject_impl (property_info, setting, get_type, out_is_default, out_to_free);

	mtu = property_info->property_typ_data->subtype.mtu.get_fcn (setting);
	if (mtu == 0) {
		NM_SET_OUT (out_is_default, TRUE);
		if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
			return _("auto");
		return "auto";
	}
	RETURN_STR_TO_FREE (g_strdup_printf ("%u", (unsigned) mtu));
}

static gconstpointer
_get_fcn_gobject_secret_flags (ARGS_GET_FCN)
{
	guint v;
	GValue val = G_VALUE_INIT;

	RETURN_UNSUPPORTED_GET_TYPE ();

	g_value_init (&val, G_TYPE_UINT);
	g_object_get_property (G_OBJECT (setting), property_info->property_name, &val);
	v = g_value_get_uint (&val);
	g_value_unset (&val);
	RETURN_STR_TO_FREE (secret_flags_to_string (v, get_type));
}

static gconstpointer
_get_fcn_gobject_enum (ARGS_GET_FCN)
{
	GType gtype = 0;
	nm_auto_unref_gtypeclass GTypeClass *gtype_class = NULL;
	nm_auto_unref_gtypeclass GTypeClass *gtype_prop_class = NULL;
	const struct _NMUtilsEnumValueInfo *value_infos = NULL;
	gboolean has_gtype = FALSE;
	nm_auto_unset_gvalue GValue gval = G_VALUE_INIT;
	gint64 v;
	gboolean format_numeric = FALSE;
	gboolean format_numeric_hex = FALSE;
	gboolean format_numeric_hex_unknown = FALSE;
	gboolean format_text = FALSE;
	gboolean format_text_l10n = FALSE;
	gs_free char *s = NULL;
	char s_numeric[64];
	GParamSpec *pspec;

	RETURN_UNSUPPORTED_GET_TYPE ();

	if (property_info->property_typ_data) {
		if (property_info->property_typ_data->subtype.gobject_enum.get_gtype) {
			gtype = property_info->property_typ_data->subtype.gobject_enum.get_gtype ();
			has_gtype = TRUE;
		}
	}

	if (   property_info->property_typ_data
	    && get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY
	    && NM_FLAGS_ANY (property_info->property_typ_data->typ_flags,   NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_NUMERIC
	                                                                  | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_NUMERIC_HEX
	                                                                  | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT
	                                                                  | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT_L10N)) {
		format_numeric_hex = NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_NUMERIC_HEX);
		format_numeric = format_numeric_hex || NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_NUMERIC);
		format_text_l10n = NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT_L10N);
		format_text = format_text_l10n || NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT);
	} else if (   property_info->property_typ_data
	           && get_type != NM_META_ACCESSOR_GET_TYPE_PRETTY
	           && NM_FLAGS_ANY (property_info->property_typ_data->typ_flags,   NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_NUMERIC
	                                                                         | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_NUMERIC_HEX
	                                                                         | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT)) {
		format_numeric_hex = NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_NUMERIC_HEX);
		format_numeric = format_numeric && NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_NUMERIC);
		format_text = NM_FLAGS_HAS (property_info->property_typ_data->typ_flags, NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT);
	} else if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY) {
		/* by default, output in format "%u (%s)" (with hex for flags and l10n). */
		format_numeric = TRUE;
		format_numeric_hex_unknown = TRUE;
		format_text = TRUE;
		format_text_l10n = TRUE;
	} else {
		/* by default, output only numeric (with hex for flags). */
		format_numeric = TRUE;
		format_numeric_hex_unknown = TRUE;
	}

	nm_assert (format_text || format_numeric);

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (setting), property_info->property_name);
	g_return_val_if_fail (pspec, NULL);

	g_value_init (&gval, pspec->value_type);
	g_object_get_property (G_OBJECT (setting), property_info->property_name, &gval);
	NM_SET_OUT (out_is_default, g_param_value_defaults (pspec, &gval));

	if (   pspec->value_type == G_TYPE_INT
	    || (    G_TYPE_IS_CLASSED (pspec->value_type)
	        &&  G_IS_ENUM_CLASS ((gtype_prop_class ?: (gtype_prop_class = g_type_class_ref (pspec->value_type)))))) {
		if (pspec->value_type == G_TYPE_INT) {
			if (!has_gtype)
				g_return_val_if_reached (NULL);
			v = g_value_get_int (&gval);
		} else
		    v = g_value_get_enum (&gval);
	} else if (   pspec->value_type == G_TYPE_UINT
	           || (   G_TYPE_IS_CLASSED (pspec->value_type)
	               && G_IS_FLAGS_CLASS ((gtype_prop_class ?: (gtype_prop_class = g_type_class_ref (pspec->value_type)))))) {
		if (pspec->value_type == G_TYPE_UINT) {
			if (!has_gtype)
				g_return_val_if_reached (NULL);
			v = g_value_get_uint (&gval);
		} else
		    v = g_value_get_flags (&gval);
	} else
		g_return_val_if_reached (NULL);

	if (!has_gtype) {
		gtype = pspec->value_type;
		gtype_class = g_steal_pointer (&gtype_prop_class);
	}

	nm_assert (({
		nm_auto_unref_gtypeclass GTypeClass *t = NULL;

		(   G_TYPE_IS_CLASSED (gtype)
		 && (t = g_type_class_ref (gtype))
		 && (G_IS_ENUM_CLASS (t) || G_IS_FLAGS_CLASS (t)));
	}));

	if (format_numeric && !format_text) {
		s =    format_numeric_hex
		    || (   format_numeric_hex_unknown
		        && !G_IS_ENUM_CLASS (gtype_class ?: (gtype_class = g_type_class_ref (gtype))))
		    ? g_strdup_printf ("0x%"G_GINT64_FORMAT, v)
		    : g_strdup_printf ("%"G_GINT64_FORMAT, v);
		RETURN_STR_TO_FREE (g_steal_pointer (&s));
	}

	/* the gobject_enum.value_infos are currently ignored for the getter. They
	 * only declare additional aliases for the setter. */

	if (property_info->property_typ_data)
		value_infos = property_info->property_typ_data->subtype.gobject_enum.value_infos_get;
	s = _nm_utils_enum_to_str_full (gtype, (int) v, ", ", value_infos);

	if (!format_numeric)
		RETURN_STR_TO_FREE (g_steal_pointer (&s));

	if (   format_numeric_hex
	    || (   format_numeric_hex_unknown
	        && !G_IS_ENUM_CLASS (gtype_class ?: (gtype_class = g_type_class_ref (gtype)))))
		nm_sprintf_buf (s_numeric, "0x%"G_GINT64_FORMAT, v);
	else
		nm_sprintf_buf (s_numeric, "%"G_GINT64_FORMAT, v);

	if (nm_streq0 (s, s_numeric))
		RETURN_STR_TO_FREE (g_steal_pointer (&s));

	if (format_text_l10n)
		RETURN_STR_TO_FREE (g_strdup_printf (_("%s (%s)"), s_numeric, s));
	else
		RETURN_STR_TO_FREE (g_strdup_printf ("%s (%s)", s_numeric, s));
}

/*****************************************************************************/

static gboolean
_set_fcn_gobject_string (ARGS_SET_FCN)
{
	gs_free char *to_free = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (property_info->property_typ_data) {
		if (property_info->property_typ_data->subtype.gobject_string.validate_fcn) {
			value = property_info->property_typ_data->subtype.gobject_string.validate_fcn (value, &to_free, error);
			if (!value)
				return FALSE;
		} else if (property_info->property_typ_data->values_static) {
			value = nmc_string_is_valid (value,
			                             (const char **) property_info->property_typ_data->values_static,
			                             error);
			if (!value)
				return FALSE;
		}
	}
	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static gboolean
_set_fcn_gobject_bool (ARGS_SET_FCN)
{
	gboolean val_bool;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!nmc_string_to_bool (value, &val_bool, error))
		return FALSE;

	g_object_set (setting, property_info->property_name, val_bool, NULL);
	return TRUE;
}

static gboolean
_set_fcn_gobject_int (ARGS_SET_FCN)
{
	int errsv;
	const GParamSpec *pspec;
	nm_auto_unset_gvalue GValue gval = G_VALUE_INIT;
	gboolean is_uint64;
	NMMetaSignUnsignInt64 v;
	gboolean has_minmax = FALSE;
	NMMetaSignUnsignInt64 min = { 0 };
	NMMetaSignUnsignInt64 max = { 0 };
	guint base = 10;
	const NMMetaUtilsIntValueInfo *value_infos;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (setting)), property_info->property_name);
	if (!G_IS_PARAM_SPEC (pspec))
		g_return_val_if_reached (FALSE);

	is_uint64 = NM_IN_SET (pspec->value_type, G_TYPE_UINT, G_TYPE_UINT64);

	if (property_info->property_typ_data) {
		if (   value
		    && (value_infos = property_info->property_typ_data->subtype.gobject_int.value_infos)) {
			gs_free char *vv_free = NULL;
			const char *vv;

			vv = nm_strstrip_avoid_copy (value, &vv_free);
			for (; value_infos->nick; value_infos++) {
				if (nm_streq (value_infos->nick, vv)) {
					v = value_infos->value;
					goto have_value_from_nick;
				}
			}
		}

		if (property_info->property_typ_data->subtype.gobject_int.base > 0)
			base = property_info->property_typ_data->subtype.gobject_int.base;

		if (   (   is_uint64
		        && (   property_info->property_typ_data->subtype.gobject_int.min.u64
		            || property_info->property_typ_data->subtype.gobject_int.max.u64))
		    || (   !is_uint64
		        && (   property_info->property_typ_data->subtype.gobject_int.min.i64
		            || property_info->property_typ_data->subtype.gobject_int.max.i64))) {
			min = property_info->property_typ_data->subtype.gobject_int.min;
			max = property_info->property_typ_data->subtype.gobject_int.max;
			has_minmax = TRUE;
		}
	}

	if (!has_minmax) {
		switch (pspec->value_type) {
		case G_TYPE_INT:
			{
				const GParamSpecInt *p = (GParamSpecInt *) pspec;

				min.i64 = p->minimum;
				max.i64 = p->maximum;
			}
			break;
		case G_TYPE_UINT:
			{
				const GParamSpecUInt *p = (GParamSpecUInt *) pspec;

				min.u64 = p->minimum;
				max.u64 = p->maximum;
			}
			break;
		case G_TYPE_INT64:
			{
				const GParamSpecInt64 *p = (GParamSpecInt64 *) pspec;

				min.i64 = p->minimum;
				max.i64 = p->maximum;
			}
			break;
		case G_TYPE_UINT64:
			{
				const GParamSpecUInt64 *p = (GParamSpecUInt64 *) pspec;

				min.u64 = p->minimum;
				max.u64 = p->maximum;
			}
			break;
		default:
			g_return_val_if_reached (FALSE);
		}
	}

	if (is_uint64)
		v.u64 = _nm_utils_ascii_str_to_uint64 (value, base, min.u64, max.u64, 0);
	else
		v.i64 = _nm_utils_ascii_str_to_int64 (value, base, min.i64, max.i64, 0);

	if ((errsv = errno) != 0) {
		if (errsv == ERANGE) {
			if (is_uint64) {
				g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
				             _("'%s' is out of range [%"G_GUINT64_FORMAT", %"G_GUINT64_FORMAT"]"),
				             value, min.u64, max.u64);
			} else {
				g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
				             _("'%s' is out of range [%"G_GINT64_FORMAT", %"G_GINT64_FORMAT"]"),
				             value, min.i64, max.i64);
			}
		} else {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("'%s' is not a valid number"), value);
		}
		return FALSE;
	}

have_value_from_nick:

	g_value_init (&gval, pspec->value_type);
	switch (pspec->value_type) {
	case G_TYPE_INT:
		g_value_set_int (&gval, v.i64);
		break;
	case G_TYPE_UINT:
		g_value_set_uint (&gval, v.u64);
		break;
	case G_TYPE_INT64:
		g_value_set_int64 (&gval, v.i64);
		break;
	case G_TYPE_UINT64:
		g_value_set_uint64 (&gval, v.u64);
		break;
	default:
		g_return_val_if_reached (FALSE);
		break;
	}

	/* Validate the number according to the property spec */
	if (!nm_g_object_set_property (G_OBJECT (setting),
	                               property_info->property_name,
	                               &gval,
	                               error))
		g_return_val_if_reached (FALSE);

	return TRUE;
}

static gboolean
_set_fcn_gobject_mtu (ARGS_SET_FCN)
{
	nm_auto_unset_gvalue GValue gval = G_VALUE_INIT;
	const GParamSpec *pspec;
	gint64 v;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (nm_streq (value, "auto"))
		value = "0";

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (setting)),
	                                      property_info->property_name);
	if (!pspec || pspec->value_type != G_TYPE_UINT)
		g_return_val_if_reached (FALSE);

	v = _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXUINT32, -1);
	if (v < 0) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
		             _("'%s' is out of range [0, %u]"), value, (unsigned) G_MAXUINT32);
		return FALSE;
	}

	g_value_init (&gval, pspec->value_type);
	g_value_set_uint (&gval, v);

	if (!nm_g_object_set_property (G_OBJECT (setting),
	                               property_info->property_name,
	                               &gval,
	                               error))
		g_return_val_if_reached (FALSE);

	return TRUE;
}

/* Ideally we'll be able to get this from a public header. */
#ifndef IEEE802154_ADDR_LEN
#define IEEE802154_ADDR_LEN 8
#endif

static gboolean
_set_fcn_gobject_mac (ARGS_SET_FCN)
{
	NMMetaPropertyTypeMacMode mode;
	gboolean valid;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (property_info->property_typ_data)
		mode = property_info->property_typ_data->subtype.mac.mode;
	else
		mode = NM_META_PROPERTY_TYPE_MAC_MODE_DEFAULT;

	if (mode == NM_META_PROPERTY_TYPE_MAC_MODE_INFINIBAND) {
		valid = nm_utils_hwaddr_valid (value, INFINIBAND_ALEN);
	} else if (mode == NM_META_PROPERTY_TYPE_MAC_MODE_WPAN) {
		valid = nm_utils_hwaddr_valid (value, IEEE802154_ADDR_LEN);
	} else {
		valid =    nm_utils_hwaddr_valid (value, ETH_ALEN)
		        || (   mode == NM_META_PROPERTY_TYPE_MAC_MODE_CLONED
		            && NM_CLONED_MAC_IS_SPECIAL (value));
	}

	if (!valid) {
		g_set_error (error, 1, 0, _("'%s' is not a valid Ethernet MAC"), value);
		return FALSE;
	}

	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static gboolean
_set_fcn_gobject_enum (ARGS_SET_FCN)
{
	GType gtype = 0;
	GType gtype_prop;
	gboolean has_gtype = FALSE;
	nm_auto_unset_gvalue GValue gval = G_VALUE_INIT;
	nm_auto_unref_gtypeclass GTypeClass *gtype_class = NULL;
	gboolean is_flags;
	int v;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (property_info->property_typ_data) {
		if (property_info->property_typ_data->subtype.gobject_enum.get_gtype) {
			gtype = property_info->property_typ_data->subtype.gobject_enum.get_gtype ();
			has_gtype = TRUE;
		}
	}

	gtype_prop = _gobject_property_get_gtype (G_OBJECT (setting), property_info->property_name);

	if (   has_gtype
	    && NM_IN_SET (gtype_prop,
	                  G_TYPE_INT,
	                  G_TYPE_UINT)
	    && G_TYPE_IS_CLASSED (gtype)
	    && (gtype_class = g_type_class_ref (gtype))
	    && (   (is_flags = G_IS_FLAGS_CLASS (gtype_class))
	        || G_IS_ENUM_CLASS (gtype_class))) {
		/* valid */
	} else if (   !has_gtype
	           && G_TYPE_IS_CLASSED (gtype_prop)
	           && (gtype_class = g_type_class_ref (gtype_prop))
	           && (   (is_flags = G_IS_FLAGS_CLASS (gtype_class))
	               || G_IS_ENUM_CLASS (gtype_class))) {
		gtype = gtype_prop;
	} else
		g_return_val_if_reached (FALSE);

	if (!_nm_utils_enum_from_str_full (gtype, value, &v, NULL,
	                                   property_info->property_typ_data
	                                       ? property_info->property_typ_data->subtype.gobject_enum.value_infos
	                                       : NULL))
		goto fail;

	if (   property_info->property_typ_data
	    && property_info->property_typ_data->subtype.gobject_enum.pre_set_notify) {
		property_info->property_typ_data->subtype.gobject_enum.pre_set_notify (property_info,
		                                                                       environment,
		                                                                       environment_user_data,
		                                                                       setting,
		                                                                       v);
	}

	g_value_init (&gval, gtype_prop);
	if (gtype_prop == G_TYPE_INT)
		g_value_set_int (&gval, v);
	else if (gtype_prop == G_TYPE_UINT)
		g_value_set_uint (&gval, v);
	else if (is_flags) {
		nm_assert (G_IS_FLAGS_CLASS (gtype_class));
		g_value_set_flags (&gval, v);
	} else {
		nm_assert (G_IS_ENUM_CLASS (gtype_class));
		g_value_set_enum (&gval, v);
	}

	if (!nm_g_object_set_property (G_OBJECT (setting), property_info->property_name, &gval, NULL))
		goto fail;

	return TRUE;

fail:
	if (error) {
		gs_free const char **valid_all = NULL;
		gs_free const char *valid_str = NULL;
		gboolean has_minmax = FALSE;
		int min = G_MININT;
		int max = G_MAXINT;

		if (property_info->property_typ_data) {
			if (   property_info->property_typ_data->subtype.gobject_enum.min
			    || property_info->property_typ_data->subtype.gobject_enum.max) {
				min = property_info->property_typ_data->subtype.gobject_enum.min;
				max = property_info->property_typ_data->subtype.gobject_enum.max;
				has_minmax = TRUE;
			}
		}

		if (!has_minmax && is_flags) {
			min = 0;
			max = (int) G_MAXUINT;
		}

		valid_all = nm_utils_enum_get_values (gtype, min, max);
		valid_str = g_strjoinv (",", (char **) valid_all);
		if (is_flags) {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("invalid option '%s', use a combination of [%s]"),
			             value,
			             valid_str);
		} else {
			g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
			             _("invalid option '%s', use one of [%s]"),
			             value,
			             valid_str);
		}
	}
	return FALSE;
}

/*****************************************************************************/

static const char *const*
_values_fcn_gobject_enum (ARGS_VALUES_FCN)
{
	GType gtype = 0;
	gboolean has_gtype = FALSE;
	gboolean has_minmax = FALSE;
	int min = G_MININT;
	int max = G_MAXINT;
	char **v;

	if (property_info->property_typ_data) {
		if (   property_info->property_typ_data->subtype.gobject_enum.min
		    || property_info->property_typ_data->subtype.gobject_enum.max) {
			min = property_info->property_typ_data->subtype.gobject_enum.min;
			max = property_info->property_typ_data->subtype.gobject_enum.max;
			has_minmax = TRUE;
		}
		if (property_info->property_typ_data->subtype.gobject_enum.get_gtype) {
			gtype = property_info->property_typ_data->subtype.gobject_enum.get_gtype ();
			has_gtype = TRUE;
		}
	}

	if (!has_gtype) {
		gtype = _gtype_property_get_gtype (property_info->setting_info->general->get_setting_gtype (),
		                                   property_info->property_name);
	}

	if (   !has_minmax
	    && G_TYPE_IS_CLASSED (gtype)) {
		nm_auto_unref_gtypeclass GTypeClass *class = NULL;

		class = g_type_class_ref (gtype);
		if (G_IS_FLAGS_CLASS (class)) {
			min = 0;
			max = (int) G_MAXUINT;
		}
	}

	/* the gobject_enum.value_infos are currently ignored for the list of
	 * values. They only declare additional (hidden) aliases for the setter. */

	v = nm_utils_strv_make_deep_copied (nm_utils_enum_get_values (gtype, min, max));
	return (const char *const*) (*out_to_free = v);
}

/*****************************************************************************/

static const char *const*
_complete_fcn_gobject_bool (ARGS_COMPLETE_FCN)
{
	static const char *const v[] = {
		"true",
		"false",
		"on",
		"off",
		"1",
		"0",
		"yes",
		"no",
		NULL,
	};

	if (!text || !text[0])
		return &v[6];
	return v;
}

static const char *const*
_complete_fcn_gobject_devices (ARGS_COMPLETE_FCN)
{
	NMDevice *const*devices = NULL;
	guint i, j;
	guint len = 0;
	char **ifnames;

	if (   environment
	    && environment->get_nm_devices) {
		devices = environment->get_nm_devices (environment,
		                                       environment_user_data,
		                                       &len);
	}

	if (len == 0)
		return NULL;

	ifnames = g_new (char *, len + 1);
	for (i = 0, j = 0; i < len; i++) {
		const char *ifname;

		nm_assert (NM_IS_DEVICE (devices[i]));

		ifname = nm_device_get_iface (devices[i]);
		if (ifname)
			ifnames[j++] = g_strdup (ifname);
	}
	ifnames[j++] = NULL;

	*out_to_free = ifnames;
	return (const char *const*) ifnames;
}

/*****************************************************************************/

static char *
wep_key_type_to_string (NMWepKeyType type)
{
	switch (type) {
	case NM_WEP_KEY_TYPE_KEY:
		return g_strdup_printf (_("%d (key)"), type);
	case NM_WEP_KEY_TYPE_PASSPHRASE:
		return g_strdup_printf (_("%d (passphrase)"), type);
	case NM_WEP_KEY_TYPE_UNKNOWN:
	default:
		return g_strdup_printf (_("%d (unknown)"), type);
	}
}

static char *
bytes_to_string (GBytes *bytes)
{
	const guint8 *data;
	gsize len;

	if (!bytes)
		return NULL;

	data = g_bytes_get_data (bytes, &len);
	return nm_utils_bin2hexstr_full (data,
	                                 len,
	                                 '\0',
	                                 TRUE,
	                                 NULL);
}

static char *
vlan_flags_to_string (guint32 flags, NMMetaAccessorGetType get_type)
{
	GString *flag_str;

	if (get_type != NM_META_ACCESSOR_GET_TYPE_PRETTY)
		return g_strdup_printf ("%u", flags);

	if (flags == 0)
		return g_strdup (_("0 (NONE)"));

	flag_str = g_string_new (NULL);
	g_string_printf (flag_str, "%d (", flags);

	if (flags & NM_VLAN_FLAG_REORDER_HEADERS)
		g_string_append (flag_str, _("REORDER_HEADERS, "));
	if (flags & NM_VLAN_FLAG_GVRP)
		g_string_append (flag_str, _("GVRP, "));
	if (flags & NM_VLAN_FLAG_LOOSE_BINDING)
		g_string_append (flag_str, _("LOOSE_BINDING, "));
	if (flags & NM_VLAN_FLAG_MVRP)
		g_string_append (flag_str, _("MVRP, "));

	if (flag_str->str[flag_str->len-1] == '(')
		g_string_append (flag_str, _("unknown"));
	else
		g_string_truncate (flag_str, flag_str->len-2);  /* chop off trailing ', ' */

	g_string_append_c (flag_str, ')');

	return g_string_free (flag_str, FALSE);
}

static char *
vlan_priorities_to_string (NMSettingVlan *s_vlan, NMVlanPriorityMap map)
{
	GString *priorities;
	int i;

	priorities = g_string_new (NULL);
	for (i = 0; i < nm_setting_vlan_get_num_priorities (s_vlan, map); i++) {
		guint32 from, to;

		if (nm_setting_vlan_get_priority (s_vlan, map, i, &from, &to))
			g_string_append_printf (priorities, "%d:%d,", from, to);
	}
	if (priorities->len)
		g_string_truncate (priorities, priorities->len-1);  /* chop off trailing ',' */

	return g_string_free (priorities, FALSE);
}

static char *
secret_flags_to_string (guint32 flags, NMMetaAccessorGetType get_type)
{
	GString *flag_str;

	if (get_type != NM_META_ACCESSOR_GET_TYPE_PRETTY)
		return g_strdup_printf ("%u", flags);

	if (flags == 0)
		return g_strdup (_("0 (none)"));

	flag_str = g_string_new (NULL);
	g_string_printf (flag_str, "%u (", flags);

	if (flags & NM_SETTING_SECRET_FLAG_AGENT_OWNED)
		g_string_append (flag_str, _("agent-owned, "));
	if (flags & NM_SETTING_SECRET_FLAG_NOT_SAVED)
		g_string_append (flag_str, _("not saved, "));
	if (flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED)
		g_string_append (flag_str, _("not required, "));

	if (flag_str->str[flag_str->len-1] == '(')
		g_string_append (flag_str, _("unknown"));
	else
		g_string_truncate (flag_str, flag_str->len-2);  /* chop off trailing ', ' */

	g_string_append_c (flag_str, ')');

	return g_string_free (flag_str, FALSE);
}

static void
vpn_data_item (const char *key, const char *value, gpointer user_data)
{
	GString *ret_str = (GString *) user_data;

	if (ret_str->len != 0)
		g_string_append (ret_str, ", ");

	g_string_append_printf (ret_str, "%s = %s", key, value);
}

static gboolean
_set_fcn_multilist (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	gsize i;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	if (strv) {
		for (i = 0; strv[i]; i++) {
			const char *item = strv[i];

			if (property_info->property_typ_data->values_static) {
				item = nmc_string_is_valid (item,
				                            (const char **) property_info->property_typ_data->values_static,
				                            error);
				if (!item)
					return FALSE;
			}

			property_info->property_typ_data->subtype.multilist.add_fcn (setting, item);
		}
	}
	return TRUE;
}

static gboolean
_remove_fcn_multilist (ARGS_REMOVE_FCN)
{
	gs_free char *value_to_free = NULL;
	gint64 idx;

	idx = _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXINT64, -1);
	if (idx != -1) {
		guint num;

		num = property_info->property_typ_data->subtype.multilist.get_num_fcn (setting);
		if (idx < (gint64) num)
			property_info->property_typ_data->subtype.multilist.remove_by_idx_fcn (setting, idx);
		return TRUE;
	}

	value = nm_strstrip_avoid_copy (value, &value_to_free);

	if (property_info->property_typ_data->values_static) {
		value = nmc_string_is_valid (value,
		                             (const char **) property_info->property_typ_data->values_static,
		                             error);
		if (!value)
			return FALSE;
	}

	property_info->property_typ_data->subtype.multilist.remove_by_value_fcn (setting, value);
	return TRUE;
}

#define DEFINE_SETTER_OPTIONS(def_func, s_macro, s_type, add_func, valid_func1, valid_func2) \
	static gboolean \
	def_func (ARGS_SET_FCN) \
	{ \
		gs_free const char **strv = NULL; \
		const char **iter; \
		const char **(*valid_func1_p) (s_type *) = valid_func1; \
		const char * (*valid_func2_p) (const char *, const char *, GError **) = valid_func2; \
		const char *opt_name, *opt_val; \
		\
		nm_assert (!error || !*error); \
		\
		if (_SET_FCN_DO_RESET_DEFAULT (value)) \
			return _gobject_property_reset_default (setting, property_info->property_name); \
		\
		strv = nm_utils_strsplit_set (value, ",", FALSE); \
		for (iter = strv; iter && *iter; iter++) { \
			char *left; \
			char *right; \
			\
			left = g_strstrip ((char *) *iter); \
			right = strchr (left, '='); \
			\
			if (!right) { \
				g_set_error (error, 1, 0, _("'%s' is not valid; use <option>=<value>"), *iter); \
				return FALSE; \
			} \
			*right++ = '\0'; \
			g_strchomp (left); \
			\
			if (valid_func1_p) { \
				const char **valid_options = valid_func1_p (s_macro (setting)); \
				if (!(opt_name = nmc_string_is_valid (left, valid_options, error))) { \
					return FALSE; \
				} \
			} else \
				opt_name = left;\
			\
			opt_val = g_strchug (right); \
			if (valid_func2_p) { \
				if (!(opt_val = valid_func2_p ((const char *) left, (const char *) opt_val, error))) { \
					return FALSE; \
				}\
			}\
			add_func (s_macro (setting), opt_name, opt_val); \
		} \
		return TRUE; \
	}

#define _DEFINE_REMOVER_INDEX_OR_VALUE(def_func, s_macro, num_func, rem_func_idx, rem_func_cmd) \
	static gboolean \
	def_func (ARGS_REMOVE_FCN) \
	{ \
		gint64 idx; \
		gint64 num; \
		\
		idx = _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXINT64, -1); \
		if (idx != -1) { \
			num = num_func (s_macro (setting)); \
			if (idx < num) \
				rem_func_idx (s_macro (setting), idx); \
			return TRUE; \
		} \
		rem_func_cmd \
	}

#define DEFINE_REMOVER_INDEX_OR_VALUE_VALIDATING(def_func, s_macro, num_func, rem_func_idx, rem_func_val) \
	_DEFINE_REMOVER_INDEX_OR_VALUE (def_func, s_macro, num_func, rem_func_idx, { \
		gs_free char *value_to_free = NULL; \
		\
		return rem_func_val (s_macro (setting), \
		                     nm_strstrip_avoid_copy (value, &value_to_free), \
		                     error); \
	})

#define DEFINE_REMOVER_INDEX_OR_VALUE_DIRECT(def_func, s_macro, num_func, rem_func_idx, rem_func_val) \
	_DEFINE_REMOVER_INDEX_OR_VALUE (def_func, s_macro, num_func, rem_func_idx, { \
		gs_free char *value_to_free = NULL; \
		\
		value = nm_strstrip_avoid_copy (value, &value_to_free); \
		if (property_info->property_typ_data->values_static) { \
			value = nmc_string_is_valid (value, \
			                             (const char **) property_info->property_typ_data->values_static, \
			                             error); \
			if (!value) \
				return FALSE; \
		} \
		\
		rem_func_val (s_macro (setting), value); \
		return TRUE; \
	})

#define DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX(def_func, s_macro, num_func, rem_func_idx, rem_func_val) \
	DEFINE_REMOVER_INDEX_OR_VALUE_VALIDATING (def_func, s_macro, num_func, rem_func_idx, rem_func_val)

#define DEFINE_REMOVER_OPTION(def_func, s_macro, rem_func) \
	static gboolean \
	def_func (ARGS_REMOVE_FCN) \
	{ \
		if (value && *value) \
			rem_func (s_macro (setting), value); \
		return TRUE; \
	}

#define DEFINE_SETTER_MAC_BLACKLIST(def_func, s_macro, add_func) \
	static gboolean \
	def_func (ARGS_SET_FCN) \
	{ \
		guint8 buf[32]; \
		gs_free const char **strv = NULL; \
		const char *const*iter; \
		\
		if (_SET_FCN_DO_RESET_DEFAULT (value)) \
			return _gobject_property_reset_default (setting, property_info->property_name); \
		\
		strv = nm_utils_strsplit_set (value, " \t,", FALSE); \
		for (iter = strv; strv && *iter; iter++) { \
			if (!nm_utils_hwaddr_aton (*iter, buf, ETH_ALEN)) { \
				g_set_error (error, 1, 0, _("'%s' is not a valid MAC"), *iter); \
				return FALSE; \
			} \
		} \
		\
		for (iter = strv; strv && *iter; iter++) \
			add_func (s_macro (setting), *iter); \
		\
		return TRUE; \
	}

static gboolean
verify_string_list (const char *const*strv,
                    const char *prop,
                    gboolean (*validate_func) (const char *),
                    GError **error)
{
	const char *const*iter;

	nm_assert (!error || !*error);

	if (strv) {
		for (iter = strv; *iter; iter++) {
			if (**iter == '\0')
				continue;
			if (validate_func) {
				if (!validate_func (*iter)) {
					g_set_error (error, 1, 0, _("'%s' is not valid"),
					             *iter);
					return FALSE;
				}
			}
		}
	}
	return TRUE;
}

static char *
flag_values_to_string (GFlagsValue *array, guint n)
{
	GString *str;
	guint i;

	str = g_string_new (NULL);
	for (i = 0; i < n; i++)
		g_string_append_printf (str, "%u, ", array[i].value);
	if (str->len)
		g_string_truncate (str, str->len-2);  /* chop off trailing ', ' */
	return g_string_free (str, FALSE);
}

static gboolean
validate_flags (NMSetting *setting, const char* prop, guint val, GError **error)
{
	GParamSpec *pspec;
	GValue value = G_VALUE_INIT;
	gboolean success = TRUE;

	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (G_OBJECT (setting)), prop);
	g_assert (G_IS_PARAM_SPEC (pspec));

	g_value_init (&value, pspec->value_type);
	g_value_set_flags (&value, val);

	if (g_param_value_validate (pspec, &value)) {
		GParamSpecFlags *pspec_flags = (GParamSpecFlags *) pspec;
		gs_free char *flag_values = NULL;

		flag_values = flag_values_to_string (pspec_flags->flags_class->values,
		                                     pspec_flags->flags_class->n_values);

		g_set_error (error, 1, 0, _("'%u' flags are not valid; use combination of %s"),
		             val, flag_values);
		success = FALSE;
	}
	g_value_unset (&value);
	return success;
}

static gboolean
_set_fcn_gobject_flags (ARGS_SET_FCN)
{
	unsigned long val_int;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!nmc_string_to_uint (value, TRUE, 0, G_MAXUINT, &val_int)) {
		g_set_error (error, 1, 0, _("'%s' is not a valid number (or out of range)"), value);
		return FALSE;
	}

	/* Validate the flags according to the property spec */
	if (!validate_flags (setting, property_info->property_name, (guint) val_int, error))
		return FALSE;

	g_object_set (setting, property_info->property_name, (guint) val_int, NULL);
	return TRUE;
}

static gboolean
_set_fcn_gobject_ssid (ARGS_SET_FCN)
{
	gs_unref_bytes GBytes *ssid = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (strlen (value) > 32) {
		g_set_error (error, 1, 0, _("'%s' is not valid"), value);
		return FALSE;
	}

	ssid = g_bytes_new (value, strlen (value));
	g_object_set (setting, property_info->property_name, ssid, NULL);
	return TRUE;
}

static gboolean
_set_fcn_gobject_ifname (ARGS_SET_FCN)
{
	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!nm_utils_is_valid_iface_name (value, error))
		return FALSE;
	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static gboolean
_set_fcn_vpn_service_type (ARGS_SET_FCN)
{
	gs_free char *service_name = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	service_name = nm_vpn_plugin_info_list_find_service_type (nm_vpn_get_plugin_infos (), value);
	g_object_set (setting, property_info->property_name, service_name ?: value, NULL);
	return TRUE;
}

static const char *const*
_complete_fcn_vpn_service_type (ARGS_COMPLETE_FCN)
{
	gsize i, j;
	char **values;

	values = nm_vpn_plugin_info_list_get_service_types (nm_vpn_get_plugin_infos (), FALSE, TRUE);
	if (!values)
		return NULL;

	if (!text || !*text) {
		/* If the prompt text is empty or contains no '.',
		 * filter out full names. */
		for (i = 0, j = 0; values[i]; i++) {
			if (strchr (values[i], '.')) {
				g_free (values[i]);
				continue;
			}

			if (i != j)
				values[j] = values[i];
			j++;
		}
		if (j)
			values[j++] = NULL;
		else {
			g_free (values);
			values = NULL;
		}
	}
	return (const char *const*) (*out_to_free = values);
}

static gboolean
nmc_util_is_domain (const char *domain)
{
	//FIXME: implement
	return TRUE;
}

static gboolean
_set_fcn_gobject_bytes (ARGS_SET_FCN)
{
	gs_free char *val_strip_free = NULL;
	gs_free const char **strv = NULL;
	const char *val_strip;
	const char **iter;
	gs_unref_bytes GBytes *bytes = NULL;
	GByteArray *array;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	val_strip = nm_strstrip_avoid_copy (value, &val_strip_free);

	/* First try hex string in the format of AAbbCCDd */
	bytes = nm_utils_hexstr2bin (val_strip);
	if (bytes)
		goto done;

	if (   !property_info->property_typ_data
	    || !property_info->property_typ_data->subtype.gobject_bytes.legacy_format) {
		if (value && value[0]) {
			g_set_error_literal (error, 1, 0, _("not a valid hex-string"));
			return FALSE;
		}
		/* accept the empty word to reset the property to %NULL. */
		goto done;
	}

	/* Otherwise, consider the following format: AA b 0xCc D */
	strv = nm_utils_strsplit_set (value, " \t", FALSE);
	array = g_byte_array_sized_new (NM_PTRARRAY_LEN (strv));
	for (iter = strv; iter && *iter; iter++) {
		int v;
		guint8 v8;

		v = _nm_utils_ascii_str_to_int64 (*iter, 16, 0, 255, -1);
		if (v == -1) {
			g_set_error (error, 1, 0, _("'%s' is not a valid hex character"), *iter);
			g_byte_array_free (array, TRUE);
			return FALSE;
		}
		v8 = v;
		g_byte_array_append (array, &v8, 1);
	}
	bytes = g_byte_array_free_to_bytes (array);

done:
	g_object_set (setting, property_info->property_name, bytes, NULL);
	return TRUE;
}

/*****************************************************************************/

static gconstpointer
_get_fcn_802_1x_ca_cert (ARGS_GET_FCN)
{
	NMSetting8021x *s_8021X = NM_SETTING_802_1X (setting);
	char *ca_cert_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_802_1x_get_ca_cert_scheme (s_8021X)) {
	case NM_SETTING_802_1X_CK_SCHEME_BLOB:
		ca_cert_str = bytes_to_string (nm_setting_802_1x_get_ca_cert_blob (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PATH:
		ca_cert_str = g_strdup (nm_setting_802_1x_get_ca_cert_path (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PKCS11:
		ca_cert_str = g_strdup (nm_setting_802_1x_get_ca_cert_uri (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_UNKNOWN:
		break;
	}

	NM_SET_OUT (out_is_default, !ca_cert_str || !ca_cert_str[0]);
	RETURN_STR_TO_FREE (ca_cert_str);
}

static gconstpointer
_get_fcn_802_1x_client_cert (ARGS_GET_FCN)
{
	NMSetting8021x *s_8021X = NM_SETTING_802_1X (setting);
	char *cert_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_802_1x_get_client_cert_scheme (s_8021X)) {
	case NM_SETTING_802_1X_CK_SCHEME_BLOB:
		if (NM_FLAGS_HAS (get_flags, NM_META_ACCESSOR_GET_FLAGS_SHOW_SECRETS))
			cert_str = bytes_to_string (nm_setting_802_1x_get_client_cert_blob (s_8021X));
		else
			return _get_text_hidden (get_type);
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PATH:
		cert_str = g_strdup (nm_setting_802_1x_get_client_cert_path (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PKCS11:
		cert_str = g_strdup (nm_setting_802_1x_get_client_cert_uri (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_UNKNOWN:
		break;
	}

	NM_SET_OUT (out_is_default, !cert_str || !cert_str[0]);
	RETURN_STR_TO_FREE (cert_str);
}

static gconstpointer
_get_fcn_802_1x_phase2_ca_cert (ARGS_GET_FCN)
{
	NMSetting8021x *s_8021X = NM_SETTING_802_1X (setting);
	char *phase2_ca_cert_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_802_1x_get_phase2_ca_cert_scheme (s_8021X)) {
	case NM_SETTING_802_1X_CK_SCHEME_BLOB:
		phase2_ca_cert_str = bytes_to_string (nm_setting_802_1x_get_phase2_ca_cert_blob (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PATH:
		phase2_ca_cert_str = g_strdup (nm_setting_802_1x_get_phase2_ca_cert_path (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PKCS11:
		phase2_ca_cert_str = g_strdup (nm_setting_802_1x_get_phase2_ca_cert_uri (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_UNKNOWN:
		break;
	}

	NM_SET_OUT (out_is_default, !phase2_ca_cert_str || !phase2_ca_cert_str[0]);
	RETURN_STR_TO_FREE (phase2_ca_cert_str);
}

static gconstpointer
_get_fcn_802_1x_phase2_client_cert (ARGS_GET_FCN)
{
	NMSetting8021x *s_8021X = NM_SETTING_802_1X (setting);
	char *cert_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_802_1x_get_phase2_client_cert_scheme (s_8021X)) {
	case NM_SETTING_802_1X_CK_SCHEME_BLOB:
		if (NM_FLAGS_HAS (get_flags, NM_META_ACCESSOR_GET_FLAGS_SHOW_SECRETS))
			cert_str = bytes_to_string (nm_setting_802_1x_get_phase2_client_cert_blob (s_8021X));
		else
			return _get_text_hidden (get_type);
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PATH:
		cert_str = g_strdup (nm_setting_802_1x_get_phase2_client_cert_path (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PKCS11:
		cert_str = g_strdup (nm_setting_802_1x_get_phase2_client_cert_uri (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_UNKNOWN:
		break;
	}

	NM_SET_OUT (out_is_default, !cert_str || !cert_str[0]);
	RETURN_STR_TO_FREE (cert_str);
}

static gconstpointer
_get_fcn_gobject_bytes (ARGS_GET_FCN)
{
	gs_unref_bytes GBytes *bytes = NULL;
	char *str;

	RETURN_UNSUPPORTED_GET_TYPE ();

	g_object_get (setting, property_info->property_name, &bytes, NULL);

	str = bytes_to_string (bytes);
	NM_SET_OUT (out_is_default, !str || !str[0]);
	RETURN_STR_TO_FREE (str);
}

static gconstpointer
_get_fcn_802_1x_private_key (ARGS_GET_FCN)
{
	NMSetting8021x *s_8021X = NM_SETTING_802_1X (setting);
	char *key_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_802_1x_get_private_key_scheme (s_8021X)) {
	case NM_SETTING_802_1X_CK_SCHEME_BLOB:
		if (NM_FLAGS_HAS (get_flags, NM_META_ACCESSOR_GET_FLAGS_SHOW_SECRETS))
			key_str = bytes_to_string (nm_setting_802_1x_get_private_key_blob (s_8021X));
		else
			return _get_text_hidden (get_type);
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PATH:
		key_str = g_strdup (nm_setting_802_1x_get_private_key_path (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PKCS11:
		key_str = g_strdup (nm_setting_802_1x_get_private_key_uri (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_UNKNOWN:
		break;
	}

	NM_SET_OUT (out_is_default, !key_str || !key_str[0]);
	RETURN_STR_TO_FREE (key_str);
}

static gconstpointer
_get_fcn_802_1x_phase2_private_key (ARGS_GET_FCN)
{
	NMSetting8021x *s_8021X = NM_SETTING_802_1X (setting);
	char *key_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_802_1x_get_phase2_private_key_scheme (s_8021X)) {
	case NM_SETTING_802_1X_CK_SCHEME_BLOB:
		if (NM_FLAGS_HAS (get_flags, NM_META_ACCESSOR_GET_FLAGS_SHOW_SECRETS))
			key_str = bytes_to_string (nm_setting_802_1x_get_phase2_private_key_blob (s_8021X));
		else
			return _get_text_hidden (get_type);
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PATH:
		key_str = g_strdup (nm_setting_802_1x_get_phase2_private_key_path (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_PKCS11:
		key_str = g_strdup (nm_setting_802_1x_get_phase2_private_key_uri (s_8021X));
		break;
	case NM_SETTING_802_1X_CK_SCHEME_UNKNOWN:
		break;
	}

	NM_SET_OUT (out_is_default, !key_str || !key_str[0]);
	RETURN_STR_TO_FREE (key_str);
}

#define DEFINE_SETTER_CERT(def_func, set_func) \
	static gboolean \
	def_func (ARGS_SET_FCN) \
	{ \
		gs_free char *value_to_free = NULL; \
		NMSetting8021xCKScheme scheme = NM_SETTING_802_1X_CK_SCHEME_PATH; \
		\
		if (_SET_FCN_DO_RESET_DEFAULT (value)) \
			return _gobject_property_reset_default (setting, property_info->property_name); \
		\
		value = nm_strstrip_avoid_copy (value, &value_to_free); \
		\
		if (strncmp (value, NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PKCS11, NM_STRLEN (NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PKCS11)) == 0) \
			scheme = NM_SETTING_802_1X_CK_SCHEME_PKCS11; \
		else if (strncmp (value, NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PATH, NM_STRLEN (NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PATH)) == 0) \
			value += NM_STRLEN (NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PATH); \
		\
		return set_func (NM_SETTING_802_1X (setting), value, scheme, NULL, error); \
	}

#define DEFINE_SETTER_PRIV_KEY(def_func, pwd_func, set_func) \
	static gboolean \
	def_func (ARGS_SET_FCN) \
	{ \
		gs_free char *path = NULL; \
		gs_free char *password_free = NULL; \
		char *password; \
		NMSetting8021xCKScheme scheme = NM_SETTING_802_1X_CK_SCHEME_PATH; \
		\
		if (_SET_FCN_DO_RESET_DEFAULT (value)) \
			return _gobject_property_reset_default (setting, property_info->property_name); \
		\
		value = nm_str_skip_leading_spaces (value); \
		\
		if (strncmp (value, NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PKCS11, NM_STRLEN (NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PKCS11)) == 0) \
			scheme = NM_SETTING_802_1X_CK_SCHEME_PKCS11; \
		else if (strncmp (value, NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PATH, NM_STRLEN (NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PATH)) == 0) \
			value += NM_STRLEN (NM_SETTING_802_1X_CERT_SCHEME_PREFIX_PATH); \
		\
		path = g_strdup (value); \
		password = path + strcspn (path, " \t"); \
		if (password[0] != '\0') { \
			password[0] = '\0'; \
			while (nm_utils_is_separator (password[0])) \
				password++; \
		} else \
			password = password_free = g_strdup (pwd_func (NM_SETTING_802_1X (setting))); \
		return set_func (NM_SETTING_802_1X (setting), path, password, scheme, NULL, error); \
	}

DEFINE_SETTER_CERT (_set_fcn_802_1x_ca_cert, nm_setting_802_1x_set_ca_cert)

DEFINE_SETTER_CERT (_set_fcn_802_1x_client_cert, nm_setting_802_1x_set_client_cert)

DEFINE_SETTER_CERT (_set_fcn_802_1x_phase2_ca_cert, nm_setting_802_1x_set_phase2_ca_cert)

DEFINE_SETTER_CERT (_set_fcn_802_1x_phase2_client_cert, nm_setting_802_1x_set_phase2_client_cert)

DEFINE_SETTER_PRIV_KEY (_set_fcn_802_1x_private_key,
                        nm_setting_802_1x_get_private_key_password,
                        nm_setting_802_1x_set_private_key)

DEFINE_SETTER_PRIV_KEY (_set_fcn_802_1x_phase2_private_key,
                        nm_setting_802_1x_get_phase2_private_key_password,
                        nm_setting_802_1x_set_phase2_private_key)

static gconstpointer
_get_fcn_bond_options (ARGS_GET_FCN)
{
	NMSettingBond *s_bond = NM_SETTING_BOND (setting);
	GString *bond_options_s;
	int i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	bond_options_s = g_string_new (NULL);
	for (i = 0; i < nm_setting_bond_get_num_options (s_bond); i++) {
		const char *key, *value;
		gs_free char *tmp_value = NULL;
		char *p;

		nm_setting_bond_get_option (s_bond, i, &key, &value);

		if (nm_streq0 (key, NM_SETTING_BOND_OPTION_ARP_IP_TARGET)) {
			value = tmp_value = g_strdup (value);
			for (p = tmp_value; p && *p; p++) {
				if (*p == ',')
					*p = ' ';
			}
		}

		g_string_append_printf (bond_options_s, "%s=%s,", key, value);
	}
	g_string_truncate (bond_options_s, bond_options_s->len-1);  /* chop off trailing ',' */

	NM_SET_OUT (out_is_default, bond_options_s->len == 0);
	RETURN_STR_TO_FREE (g_string_free (bond_options_s, FALSE));
}

/*  example: miimon=100,mode=balance-rr, updelay=5 */
static gboolean
_validate_and_remove_bond_option (NMSettingBond *setting, const char *option)
{
	const char *opt;
	const char **valid_options;

	valid_options = nm_setting_bond_get_valid_options (setting);
	opt = nmc_string_is_valid (option, valid_options, NULL);

	if (opt)
		return nm_setting_bond_remove_option (setting, opt);
	else
		return FALSE;
}

static const char *
_validate_bond_option_value (const char *option, const char *value, GError **error)
{
	if (!g_strcmp0 (option, NM_SETTING_BOND_OPTION_MODE))
		return nmc_bond_validate_mode (value, error);

	return value;
}

static gboolean
_bond_add_option (NMSettingBond *setting,
                  const char *name,
                  const char *value)
{
	gs_free char *tmp_value = NULL;
	char *p;

	if (nm_streq0 (name, NM_SETTING_BOND_OPTION_ARP_IP_TARGET)) {
		value = tmp_value = g_strdup (value);
		for (p = tmp_value; p && *p; p++)
			if (*p == ' ')
				*p = ',';
	}

	return nm_setting_bond_add_option (setting, name, value);
}

DEFINE_SETTER_OPTIONS (_set_fcn_bond_options,
                       NM_SETTING_BOND,
                       NMSettingBond,
                       _bond_add_option,
                       nm_setting_bond_get_valid_options,
                       _validate_bond_option_value)
DEFINE_REMOVER_OPTION (_remove_fcn_bond_options,
                       NM_SETTING_BOND,
                       _validate_and_remove_bond_option)

static const char *
_describe_fcn_bond_options (ARGS_DESCRIBE_FCN)
{
	gs_free char *options_str = NULL;
	const char **valid_options;
	char *s;

	valid_options = nm_setting_bond_get_valid_options (NULL);
	options_str = g_strjoinv (", ", (char **) valid_options);

	s = g_strdup_printf (_("Enter a list of bonding options formatted as:\n"
	                       "  option = <value>, option = <value>,... \n"
	                       "Valid options are: %s\n"
	                       "'mode' can be provided as a name or a number:\n"
	                       "balance-rr    = 0\n"
	                       "active-backup = 1\n"
	                       "balance-xor   = 2\n"
	                       "broadcast     = 3\n"
	                       "802.3ad       = 4\n"
	                       "balance-tlb   = 5\n"
	                       "balance-alb   = 6\n\n"
	                       "Example: mode=2,miimon=120\n"), options_str);
	return (*out_to_free = s);
}

static const char *const*
_values_fcn_bond_options (ARGS_VALUES_FCN)
{
	return nm_setting_bond_get_valid_options (NULL);
}

static gconstpointer
_get_fcn_connection_permissions (ARGS_GET_FCN)
{
	NMSettingConnection *s_con = NM_SETTING_CONNECTION (setting);
	GString *perm = NULL;
	const char *perm_item;
	const char *perm_type;
	int i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	perm = g_string_new (NULL);
	for (i = 0; i < nm_setting_connection_get_num_permissions (s_con); i++) {
		if (nm_setting_connection_get_permission (s_con, i, &perm_type, &perm_item, NULL))
			g_string_append_printf (perm, "%s:%s,", perm_type, perm_item);
	}

	NM_SET_OUT (out_is_default, perm->len == 0);

	if (perm->len > 0) {
		g_string_truncate (perm, perm->len-1); /* remove trailing , */
		RETURN_STR_TO_FREE (g_string_free (perm, FALSE));
	}

	/* No value from get_permission */
	g_string_free (perm, TRUE);
	return NULL;
}

static gboolean
_set_fcn_connection_type (ARGS_SET_FCN)
{
	gs_free char *uuid = NULL;

	if (nm_setting_connection_get_uuid (NM_SETTING_CONNECTION (setting))) {
		/* Don't allow setting type unless the connection is brand new.
		 * Just because it's a bad idea and the user wouldn't probably want that.
		 * No technical reason, really.
		 * Also, using uuid to see if the connection is brand new is a bit
		 * hacky: we can not see if the type is already set, because
		 * nmc_setting_set_property() is called only after the property
		 * we're setting (type) has been removed. */
		g_set_error (error, 1, 0, _("Can not change the connection type"));
		return FALSE;
	}

	if (_SET_FCN_DO_RESET_DEFAULT (value)) {
		g_object_set (G_OBJECT (setting), property_info->property_name, NULL, NULL);
		return TRUE;
	}

	uuid = nm_utils_uuid_generate ();
	g_object_set (G_OBJECT (setting),
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NULL);

	g_object_set (G_OBJECT (setting), property_info->property_name, value, NULL);
	return TRUE;
}

static const char *const*
_complete_fcn_connection_type (ARGS_COMPLETE_FCN)
{
	guint i, j;
	char **result;
	gsize text_len;

	result = g_new (char *, _NM_META_SETTING_TYPE_NUM * 2 + 1);

	text_len = text ? strlen (text) : 0;

	for (i = 0, j = 0; i < _NM_META_SETTING_TYPE_NUM; i++) {
		const NMMetaSettingInfoEditor *setting_info = &nm_meta_setting_infos_editor[i];
		const char *v;

		if (!setting_info->valid_parts)
			continue;

		v = setting_info->alias;
		if (v) {
			if (!text || strncmp (text, v, text_len) == 0)
				result[j++] = g_strdup (v);
		}
		v = setting_info->general->setting_name;
		if (!text || strncmp (text, v, text_len) == 0)
			result[j++] = g_strdup (v);
	}
	if (j)
		result[j++] = NULL;
	else {
		g_free (result);
		result = NULL;
	}

	return (const char *const*) (*out_to_free = result);
}

/* define from libnm-core/nm-setting-connection.c */
#define PERM_USER_PREFIX  "user:"

static gboolean
permissions_valid (const char *perm)
{
	if (!perm || perm[0] == '\0')
		return FALSE;

	if (strncmp (perm, PERM_USER_PREFIX, strlen (PERM_USER_PREFIX)) == 0) {
		if (   strlen (perm) <= strlen (PERM_USER_PREFIX)
		    || strchr (perm + strlen (PERM_USER_PREFIX), ':'))
			return  FALSE;
	} else {
		if (strchr (perm, ':'))
			return  FALSE;
	}

	return TRUE;
}

static gboolean
_set_fcn_connection_permissions (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	gsize i;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	if (!verify_string_list (strv, property_info->property_name, permissions_valid, error))
		return FALSE;

	for (i = 0; strv && strv[i]; i++) {
		const char *user = strv[i];

		if (strncmp (user, PERM_USER_PREFIX, NM_STRLEN (PERM_USER_PREFIX)) == 0)
			user += NM_STRLEN (PERM_USER_PREFIX);
		nm_setting_connection_add_permission (NM_SETTING_CONNECTION (setting), "user", user, NULL);
	}

	return TRUE;
}

DEFINE_REMOVER_INDEX_OR_VALUE_DIRECT (_remove_fcn_connection_permissions,
                                      NM_SETTING_CONNECTION,
                                      nm_setting_connection_get_num_permissions,
                                      nm_setting_connection_remove_permission,
                                      nm_setting_connection_remove_permission_user)

static gboolean
_set_fcn_connection_master (ARGS_SET_FCN)
{
	nm_assert (!error || !*error);

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		value = NULL;
	else if (!*value)
		value = NULL;
	else if (   !nm_utils_is_valid_iface_name (value, NULL)
	         && !nm_utils_is_uuid (value)) {
		g_set_error (error, 1, 0,
		             _("'%s' is not valid master; use ifname or connection UUID"),
		             value);
		return FALSE;
	}
	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static const char *const*
_complete_fcn_connection_master (ARGS_COMPLETE_FCN)
{
	NMRemoteConnection *const*connections = NULL;
	guint len = 0;
	guint i, j;
	char **result;
	NMSettingConnection *s_con;
	const char *expected_type = NULL;
	gsize text_len;

	if (   environment
	    && environment->get_nm_connections) {
		connections = environment->get_nm_connections (environment,
		                                               environment_user_data,
		                                               &len);
	}
	if (!len)
		return NULL;

	if (   (!text || !*text)
	    && operation_context
	    && operation_context->connection) {
		/* if we have no text yet, initially only complete for matching
		 * slave-type. */
		s_con = nm_connection_get_setting_connection (operation_context->connection);
		if (s_con)
			expected_type = nm_setting_connection_get_slave_type (s_con);
	}

	text_len = text ? strlen (text) : 0;

	result = g_new (char *, (2 * len) + 1);
	for (i = 0, j = 0; i < len; i++) {
		const char *v;

		s_con = nm_connection_get_setting_connection (NM_CONNECTION (connections[i]));
		if (!s_con)
			continue;

		if (   expected_type
		    && !nm_streq0 (nm_setting_connection_get_connection_type (s_con),
		                   expected_type))
			continue;

		if (text && text[0]) {
			/* if we have text, also complete for the UUID. */
			v = nm_setting_connection_get_uuid (s_con);
			if (v && (!text || strncmp (text, v, text_len) == 0))
				result[j++] = g_strdup (v);
		}

		v = nm_setting_connection_get_interface_name (s_con);
		if (v && (!text || strncmp (text, v, text_len) == 0))
			result[j++] = g_strdup (v);
	}
	if (j)
		result[j++] = NULL;
	else {
		g_free (result);
		result = NULL;
	}

	return (const char *const*) (*out_to_free = result);
}

static gboolean
_set_fcn_connection_secondaries (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	if (strv) {
		for (iter = strv; *iter; iter++)
			nm_setting_connection_add_secondary (NM_SETTING_CONNECTION (setting), *iter);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_connection_secondary (NMSettingConnection *setting,
                                           const char *secondary_uuid,
                                           GError **error)
{
	if (!nm_utils_is_uuid (secondary_uuid)) {
		nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
		                    _("the value '%s' is not a valid UUID"), secondary_uuid);
		return FALSE;
	}

	nm_setting_connection_remove_secondary_by_value (setting, secondary_uuid);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_VALIDATING (_remove_fcn_connection_secondaries,
                                          NM_SETTING_CONNECTION,
                                          nm_setting_connection_get_num_secondaries,
                                          nm_setting_connection_remove_secondary,
                                          _validate_and_remove_connection_secondary)

static gconstpointer
_get_fcn_connection_metered (ARGS_GET_FCN)
{
	NMSettingConnection *s_conn = NM_SETTING_CONNECTION (setting);
	const char *s;

	RETURN_UNSUPPORTED_GET_TYPE ();

	switch (nm_setting_connection_get_metered (s_conn)) {
	case NM_METERED_YES:
		s = N_("yes");
		break;
	case NM_METERED_NO:
		s = N_("no");
		break;
	case NM_METERED_UNKNOWN:
	default:
		NM_SET_OUT (out_is_default, TRUE);
		s = N_("unknown");
		break;
	}

	if (get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
		return _(s);
	return s;
}

static gboolean
_set_fcn_connection_metered (ARGS_SET_FCN)
{
	NMMetered metered;
	NMTernary ts_val;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!nmc_string_to_ternary (value, &ts_val, error))
		return FALSE;

	switch (ts_val) {
	case NM_TERNARY_TRUE:
		metered = NM_METERED_YES;
		break;
	case NM_TERNARY_FALSE:
		metered = NM_METERED_NO;
		break;
	case NM_TERNARY_DEFAULT:
		metered = NM_METERED_UNKNOWN;
		break;
	default:
		g_assert_not_reached();
	}

	g_object_set (setting, property_info->property_name, metered, NULL);
	return TRUE;
}

static char *
dcb_flags_to_string (NMSettingDcbFlags flags)
{
	GString *flag_str;

	if (flags == 0)
		return g_strdup (_("0 (disabled)"));

	flag_str = g_string_new (NULL);
	g_string_printf (flag_str, "%d (", flags);

	if (flags & NM_SETTING_DCB_FLAG_ENABLE)
		g_string_append (flag_str, _("enabled, "));
	if (flags & NM_SETTING_DCB_FLAG_ADVERTISE)
		g_string_append (flag_str, _("advertise, "));
	if (flags & NM_SETTING_DCB_FLAG_WILLING)
		g_string_append (flag_str, _("willing, "));

	if (flag_str->str[flag_str->len-1] == '(')
		g_string_append (flag_str, _("unknown"));
	else
		g_string_truncate (flag_str, flag_str->len-2);  /* chop off trailing ', ' */

	g_string_append_c (flag_str, ')');

	return g_string_free (flag_str, FALSE);
}

static gconstpointer
_get_fcn_dcb (ARGS_GET_FCN)
{
	NMSettingDcb *s_dcb = NM_SETTING_DCB (setting);
	GString *str;
	guint i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	str = g_string_new (NULL);
	for (i = 0; i < 8; i++) {
		guint v;

		v = property_info->property_typ_data->subtype.dcb.get_fcn (s_dcb, i);

		if (i > 0)
			g_string_append_c (str, ',');
		g_string_append_printf (str, "%u", v);
	}

	RETURN_STR_TO_FREE (g_string_free (str, FALSE));
}

#define DCB_ALL_FLAGS (NM_SETTING_DCB_FLAG_ENABLE | NM_SETTING_DCB_FLAG_ADVERTISE | NM_SETTING_DCB_FLAG_WILLING)

static gconstpointer
_get_fcn_dcb_flags (ARGS_GET_FCN)
{
	nm_auto_unset_gvalue GValue val = G_VALUE_INIT;
	guint v;

	RETURN_UNSUPPORTED_GET_TYPE ();

	g_value_init (&val, G_TYPE_UINT);
	g_object_get_property (G_OBJECT (setting), property_info->property_name, &val);
	v = g_value_get_uint (&val);

	RETURN_STR_TO_FREE (dcb_flags_to_string (v));
}

static gboolean
_set_fcn_dcb_flags (ARGS_SET_FCN)
{
	NMSettingDcbFlags flags = NM_SETTING_DCB_FLAG_NONE;
	long int t;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	/* Check for overall hex numeric value */
	t = _nm_utils_ascii_str_to_int64 (value, 0, 0, DCB_ALL_FLAGS, -1);
	if (t != -1)
		flags = (guint) t;
	else {
		gs_free const char **strv = NULL;
		const char *const*iter;

		/* Check for individual flag numbers */
		strv = nm_utils_strsplit_set (value, " \t,", FALSE);
		for (iter = strv; iter && *iter; iter++) {
			t = _nm_utils_ascii_str_to_int64 (*iter, 0, 0, DCB_ALL_FLAGS, -1);

			if (   g_ascii_strcasecmp (*iter, "enable") == 0
			    || g_ascii_strcasecmp (*iter, "enabled") == 0
			    || t == NM_SETTING_DCB_FLAG_ENABLE)
				flags |= NM_SETTING_DCB_FLAG_ENABLE;
			else if (   g_ascii_strcasecmp (*iter, "advertise") == 0
				 || t == NM_SETTING_DCB_FLAG_ADVERTISE)
				flags |= NM_SETTING_DCB_FLAG_ADVERTISE;
			else if (   g_ascii_strcasecmp (*iter, "willing") == 0
				 || t == NM_SETTING_DCB_FLAG_WILLING)
				flags |= NM_SETTING_DCB_FLAG_WILLING;
			else if (   g_ascii_strcasecmp (*iter, "disable") == 0
				 || g_ascii_strcasecmp (*iter, "disabled") == 0
				 || t == 0) {
				/* pass */
			} else {
				g_set_error (error, 1, 0, _("'%s' is not a valid DCB flag"), *iter);
				return FALSE;
			}
		}
	}

	/* Validate the flags according to the property spec */
	if (!validate_flags (setting, property_info->property_name, (guint) flags, error))
		return FALSE;

	g_object_set (setting, property_info->property_name, (guint) flags, NULL);
	return TRUE;
}

static gboolean
dcb_parse_uint_array (const char *val,
                      guint max,
                      guint other,
                      guint out_array[static 8],
                      GError **error)
{
	gs_strfreev char **items = NULL;
	char **iter;
	gsize i;

	items = g_strsplit_set (val, ",", -1);
	if (g_strv_length (items) != 8) {
		g_set_error_literal (error, 1, 0, _("must contain 8 comma-separated numbers"));
		return FALSE;
	}

	i = 0;
	for (iter = items; *iter; iter++) {
		gint64 num;

		*iter = g_strstrip (*iter);

		num = _nm_utils_ascii_str_to_int64 (*iter, 10, 0, other ?: max, -1);

		/* If number is greater than 'max' it must equal 'other' */
		if (   num == -1
		    || (other && (num > max) && (num != other))) {
			if (other) {
				g_set_error (error, 1, 0, _("'%s' not a number between 0 and %u (inclusive) or %u"),
				             *iter, max, other);
			} else {
				g_set_error (error, 1, 0, _("'%s' not a number between 0 and %u (inclusive)"),
				             *iter, max);
			}
			return FALSE;
		}
		nm_assert (i < 8);
		out_array[i++] = (guint) num;
	}

	return TRUE;
}

static void
dcb_check_feature_enabled (const NMMetaEnvironment *environment, gpointer *environment_user_data, NMSettingDcb *s_dcb, const char *flags_prop)
{
	NMSettingDcbFlags flags = NM_SETTING_DCB_FLAG_NONE;

	g_object_get (s_dcb, flags_prop, &flags, NULL);
	if (!(flags & NM_SETTING_DCB_FLAG_ENABLE)) {
		_env_warn_fcn (environment, environment_user_data,
		               NM_META_ENV_WARN_LEVEL_WARN,
		               N_("changes will have no effect until '%s' includes 1 (enabled)"),
		               flags_prop);
	}
}

static gboolean
_set_fcn_dcb (ARGS_SET_FCN)
{
	guint i = 0;
	guint nums[8] = { 0, };

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!dcb_parse_uint_array (value,
	                           property_info->property_typ_data->subtype.dcb.max,
	                           property_info->property_typ_data->subtype.dcb.other,
	                           nums,
	                           error))
		return FALSE;

	if (property_info->property_typ_data->subtype.dcb.is_percent) {
		guint sum = 0;

		for (i = 0; i < 8; i++) {
			sum += nums[i];
			if (nums[i] > 100 || sum > 100)
				break;
		}
		if (sum != 100) {
			g_set_error_literal (error, 1, 0, _("bandwidth percentages must total 100%%"));
			return FALSE;
		}
	}

	for (i = 0; i < 8; i++)
		property_info->property_typ_data->subtype.dcb.set_fcn (NM_SETTING_DCB (setting), i, nums[i]);

	dcb_check_feature_enabled (environment, environment_user_data, NM_SETTING_DCB (setting), NM_SETTING_DCB_PRIORITY_GROUP_FLAGS);
	return TRUE;
}

static gconstpointer
_get_fcn_dcb_bool (ARGS_GET_FCN)
{
	NMSettingDcb *s_dcb = NM_SETTING_DCB (setting);
	GString *str;
	guint i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	str = g_string_new (NULL);
	for (i = 0; i < 8; i++) {
		gboolean v;

		v = property_info->property_typ_data->subtype.dcb_bool.get_fcn (s_dcb, i);

		if (i > 0)
			g_string_append_c (str, ',');
		g_string_append_c (str, v ? '1': '0');
	}

	RETURN_STR_TO_FREE (g_string_free (str, FALSE));
}

static gboolean
_set_fcn_dcb_bool (ARGS_SET_FCN)
{
	guint i = 0;
	guint nums[8] = { 0, };

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!dcb_parse_uint_array (value, 1, 0, nums, error))
		return FALSE;

	for (i = 0; i < 8; i++) {
		property_info->property_typ_data->subtype.dcb_bool.set_fcn (NM_SETTING_DCB (setting),
		                                                            i,
		                                                            !!nums[i]);
	}

	dcb_check_feature_enabled (environment,
	                           environment_user_data,
	                           NM_SETTING_DCB (setting),
	                           (  property_info->property_typ_data->subtype.dcb_bool.with_flow_control_flags
	                            ? NM_SETTING_DCB_PRIORITY_FLOW_CONTROL_FLAGS
	                            : NM_SETTING_DCB_PRIORITY_GROUP_FLAGS));
	return TRUE;
}

static gboolean
_set_fcn_gsm_sim_operator_id (ARGS_SET_FCN)
{
	const char *p = value;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!NM_IN_SET (strlen (value), 5, 6)) {
		g_set_error_literal (error, 1, 0, _("SIM operator ID must be a 5 or 6 number MCCMNC code"));
		return FALSE;
	}

	while (p && *p) {
		if (!g_ascii_isdigit (*p++)) {
			g_set_error_literal (error, 1, 0, _("SIM operator ID must be a 5 or 6 number MCCMNC code"));
			return FALSE;
		}
	}
	g_object_set (G_OBJECT (setting),
	              NM_SETTING_GSM_SIM_OPERATOR_ID,
	              value,
	              NULL);
	return TRUE;
}

static gboolean
_set_fcn_infiniband_p_key (ARGS_SET_FCN)
{
	gint64 p_key;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (nm_streq (value, "default"))
		p_key = -1;
	else {
		p_key = _nm_utils_ascii_str_to_int64 (value, 0, -1, G_MAXUINT16, -2);
		if (p_key == -2) {
			g_set_error (error, 1, 0, _("'%s' is not a valid IBoIP P_Key"), value);
			return FALSE;
		}
	}

	g_object_set (setting, property_info->property_name, (int) p_key, NULL);
	return TRUE;
}

static gconstpointer
_get_fcn_infiniband_p_key (ARGS_GET_FCN)
{
	NMSettingInfiniband *s_infiniband = NM_SETTING_INFINIBAND (setting);
	int p_key;

	RETURN_UNSUPPORTED_GET_TYPE ();

	p_key = nm_setting_infiniband_get_p_key (s_infiniband);
	if (p_key == -1) {
		NM_SET_OUT (out_is_default, TRUE);
		if (get_type != NM_META_ACCESSOR_GET_TYPE_PRETTY)
			return "default";
		else
			return _("default");
	}

	RETURN_STR_TO_FREE (g_strdup_printf ("0x%04x", p_key));
}

static gconstpointer
_get_fcn_ip_config_addresses (ARGS_GET_FCN)
{
	NMSettingIPConfig *s_ip = NM_SETTING_IP_CONFIG (setting);
	GString *printable;
	guint num_addresses, i;
	NMIPAddress *addr;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_addresses = NM_MIN ((guint) G_MAXINT, nm_setting_ip_config_get_num_addresses (s_ip));
	for (i = 0; i < num_addresses; i++) {
		addr = nm_setting_ip_config_get_address (s_ip, i);

		if (printable->len > 0)
			g_string_append (printable, ", ");

		g_string_append_printf (printable, "%s/%u",
		                        nm_ip_address_get_address (addr),
		                        nm_ip_address_get_prefix (addr));
	}

	NM_SET_OUT (out_is_default, num_addresses == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gconstpointer
_get_fcn_ip_config_routes (ARGS_GET_FCN)
{
	NMSettingIPConfig *s_ip = NM_SETTING_IP_CONFIG (setting);
	GString *printable;
	guint num_routes, i;
	NMIPRoute *route;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_routes = NM_MIN ((guint) G_MAXINT, nm_setting_ip_config_get_num_routes (s_ip));
	for (i = 0; i < num_routes; i++) {
		gs_free char *attr_str = NULL;
		gs_strfreev char **attr_names = NULL;
		gs_unref_hashtable GHashTable *hash = g_hash_table_new (nm_str_hash, g_str_equal);
		int j;

		route = nm_setting_ip_config_get_route (s_ip, i);

		attr_names = nm_ip_route_get_attribute_names (route);
		for (j = 0; attr_names && attr_names[j]; j++) {
			g_hash_table_insert (hash, attr_names[j],
			                     nm_ip_route_get_attribute (route, attr_names[j]));
		}

		attr_str = nm_utils_format_variant_attributes (hash, ' ', '=');

		if (get_type != NM_META_ACCESSOR_GET_TYPE_PRETTY) {
			if (printable->len > 0)
				g_string_append (printable, ", ");

			g_string_append_printf (printable, "%s/%u",
			                        nm_ip_route_get_dest (route),
			                        nm_ip_route_get_prefix (route));

			if (nm_ip_route_get_next_hop (route))
				g_string_append_printf (printable, " %s", nm_ip_route_get_next_hop (route));
			if (nm_ip_route_get_metric (route) != -1)
				g_string_append_printf (printable, " %u", (guint32) nm_ip_route_get_metric (route));
			if (attr_str)
				g_string_append_printf (printable, " %s", attr_str);
		} else {

			if (printable->len > 0)
				g_string_append (printable, "; ");

			g_string_append (printable, "{ ");

			g_string_append_printf (printable, "ip = %s/%u",
			                        nm_ip_route_get_dest (route),
			                        nm_ip_route_get_prefix (route));

			if (nm_ip_route_get_next_hop (route)) {
				g_string_append_printf (printable, ", nh = %s",
				                        nm_ip_route_get_next_hop (route));
			}

			if (nm_ip_route_get_metric (route) != -1)
				g_string_append_printf (printable, ", mt = %u", (guint32) nm_ip_route_get_metric (route));
			if (attr_str)
				g_string_append_printf (printable, " %s", attr_str);

			g_string_append (printable, " }");
		}
	}

	NM_SET_OUT (out_is_default, num_routes == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gboolean
_set_fcn_ip_config_method (ARGS_SET_FCN)
{
	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	/* Silently accept "static" and convert to "manual" */
	if (   strlen (value) > 1
	    && matches (value, "static")) {
		if (nm_setting_ip_config_get_addr_family (NM_SETTING_IP_CONFIG (setting)) == AF_INET)
			value = NM_SETTING_IP4_CONFIG_METHOD_MANUAL;
		else
			value = NM_SETTING_IP6_CONFIG_METHOD_MANUAL;
	}

	value = nmc_string_is_valid (value,
	                             (const char **) property_info->property_typ_data->values_static,
	                             error);
	if (!value)
		return FALSE;

	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static gboolean
_set_fcn_ip_config_dns (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	if (strv) {
		int addr_family = nm_setting_ip_config_get_addr_family (NM_SETTING_IP_CONFIG (setting));

		for (iter = strv; *iter; iter++) {
			const char *addr;

			addr = g_strstrip ((char *) *iter);

			if (!nm_utils_parse_inaddr (addr_family, addr, NULL)) {
				nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
				                    _("invalid IPv%c address '%s'"),
				                    nm_utils_addr_family_to_char (addr_family),
				                    addr);
				return FALSE;
			}
			nm_setting_ip_config_add_dns (NM_SETTING_IP_CONFIG (setting), addr);
		}
	}
	return TRUE;
}

static gboolean
_validate_and_remove_ip_dns (NMSettingIPConfig *setting,
                               const char *dns,
                               GError **error)
{
	int addr_family = nm_setting_ip_config_get_addr_family (setting);

	if (!nm_utils_parse_inaddr (addr_family, dns, NULL)) {
		nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
		                    _("invalid IPv%c address '%s'"),
		                    nm_utils_addr_family_to_char (addr_family),
		                    dns);
		return FALSE;
	}

	nm_setting_ip_config_remove_dns_by_value (setting, dns);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_VALIDATING (_remove_fcn_ip_config_dns,
                                          NM_SETTING_IP_CONFIG,
                                          nm_setting_ip_config_get_num_dns,
                                          nm_setting_ip_config_remove_dns,
                                          _validate_and_remove_ip_dns)

static gboolean
_set_fcn_ip_config_dns_search (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	gsize i;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	if (!verify_string_list (strv, property_info->property_name, nmc_util_is_domain, error))
		return FALSE;

	if (strv) {
		for (i = 0; strv[i]; i++)
			nm_setting_ip_config_add_dns_search (NM_SETTING_IP_CONFIG (setting), strv[i]);
	}
	return TRUE;
}

DEFINE_REMOVER_INDEX_OR_VALUE_DIRECT (_remove_fcn_ip_config_dns_search,
                                      NM_SETTING_IP_CONFIG,
                                      nm_setting_ip_config_get_num_dns_searches,
                                      nm_setting_ip_config_remove_dns_search,
                                      nm_setting_ip_config_remove_dns_search_by_value)

static gboolean
_set_fcn_ip_config_dns_options (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	NMSettingIPConfig *s_ip = NM_SETTING_IP_CONFIG (setting);
	gsize i;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	if (strv) {
		for (i = 0; strv[i]; i++) {
			nm_setting_ip_config_remove_dns_option_by_value (s_ip, strv[i]);
			nm_setting_ip_config_add_dns_option (s_ip, strv[i]);
		}
	}
	return TRUE;
}

DEFINE_REMOVER_INDEX_OR_VALUE_DIRECT (_remove_fcn_ip_config_dns_options,
                                      NM_SETTING_IP_CONFIG,
                                      nm_setting_ip_config_get_num_dns_options,
                                      nm_setting_ip_config_remove_dns_option,
                                      nm_setting_ip_config_remove_dns_option_by_value)

static gboolean
_set_fcn_ip4_config_addresses (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; *iter; iter++) {
		nm_auto_unref_ip_address NMIPAddress *addr = NULL;

		addr = _parse_ip_address (AF_INET, *iter, error);
		if (!addr)
			return FALSE;
		nm_setting_ip_config_add_address (NM_SETTING_IP_CONFIG (setting), addr);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_ip_address (NMSettingIPConfig *setting,
                                 const char *value,
                                 GError **error)
{
	nm_auto_unref_ip_address NMIPAddress *addr = NULL;
	int addr_family = nm_setting_ip_config_get_addr_family (NM_SETTING_IP_CONFIG (setting));

	addr = _parse_ip_address (addr_family, value, error);
	if (!addr)
		return FALSE;

	nm_setting_ip_config_remove_address_by_value (setting, addr);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_ip_config_addresses,
                                       NM_SETTING_IP_CONFIG,
                                       nm_setting_ip_config_get_num_addresses,
                                       nm_setting_ip_config_remove_address,
                                       _validate_and_remove_ip_address)

static gboolean
_set_fcn_ip4_config_gateway (ARGS_SET_FCN)
{
	gs_free char *value_to_free = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	value = nm_strstrip_avoid_copy (value, &value_to_free);

	if (!nm_utils_ipaddr_valid (AF_INET, value)) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
	                 _("invalid gateway address '%s'"), value);
		return FALSE;
	}
	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static gboolean
_set_fcn_ip4_config_routes (ARGS_SET_FCN)
{
	nm_auto_unref_ip_route NMIPRoute *ip4route = NULL;
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value)) {
		nm_setting_ip_config_clear_routes (NM_SETTING_IP_CONFIG (setting));
		return TRUE;
	}

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; *iter; iter++) {
		nm_auto_unref_ip_route NMIPRoute *route = NULL;

		route = _parse_ip_route (AF_INET, *iter, error);
		if (!route)
			return FALSE;
		nm_setting_ip_config_add_route (NM_SETTING_IP_CONFIG (setting), route);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_ipv4_route (NMSettingIPConfig *setting,
                                 const char *value,
                                 GError **error)
{
	nm_auto_unref_ip_route NMIPRoute *route = NULL;

	route = _parse_ip_route (AF_INET, value, error);
	if (!route)
		return FALSE;

	nm_setting_ip_config_remove_route_by_value (setting, route);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_ipv4_config_routes,
                                       NM_SETTING_IP_CONFIG,
                                       nm_setting_ip_config_get_num_routes,
                                       nm_setting_ip_config_remove_route,
                                       _validate_and_remove_ipv4_route)

static gboolean
_dns_options_is_default (NMSettingIPConfig *setting)
{
	return    nm_setting_ip_config_has_dns_options (setting)
	       && !nm_setting_ip_config_get_num_dns_options (setting);
}

static gboolean
_set_fcn_ip6_config_addresses (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_ip_address NMIPAddress *addr = NULL;

		addr = _parse_ip_address (AF_INET6, *iter, error);
		if (!addr)
			return FALSE;
		nm_setting_ip_config_add_address (NM_SETTING_IP_CONFIG (setting), addr);
	}
	return TRUE;
}

static gboolean
_set_fcn_ip6_config_gateway (ARGS_SET_FCN)
{
	gs_free char *value_to_free = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	value = nm_strstrip_avoid_copy (value, &value_to_free);

	if (!nm_utils_ipaddr_valid (AF_INET6, value)) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
		             _("invalid gateway address '%s'"),
		             value);
		return FALSE;
	}

	g_object_set (setting, property_info->property_name, value, NULL);
	return TRUE;
}

static gboolean
_set_fcn_ip6_config_routes (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_ip_route NMIPRoute *route = NULL;

		route = _parse_ip_route (AF_INET6, *iter, error);
		if (!route)
			return FALSE;
		nm_setting_ip_config_add_route (NM_SETTING_IP_CONFIG (setting), route);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_ipv6_route (NMSettingIPConfig *setting,
                                 const char *value,
                                 GError **error)
{
	nm_auto_unref_ip_route NMIPRoute *route = NULL;

	route = _parse_ip_route (AF_INET6, value, error);
	if (!route)
		return FALSE;

	nm_setting_ip_config_remove_route_by_value (setting, route);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_ipv6_config_routes,
                                       NM_SETTING_IP_CONFIG,
                                       nm_setting_ip_config_get_num_routes,
                                       nm_setting_ip_config_remove_route,
                                       _validate_and_remove_ipv6_route)

static gconstpointer
_get_fcn_match_interface_name (ARGS_GET_FCN)
{
	NMSettingMatch *s_match = NM_SETTING_MATCH (setting);
	GString *str = NULL;
	guint i, num;

	RETURN_UNSUPPORTED_GET_TYPE ();

	num = nm_setting_match_get_num_interface_names (s_match);
	for (i = 0; i < num; i++) {
		const char *name;
		gs_free char *to_free = NULL;

		if (i == 0)
			str = g_string_new ("");
		else
			g_string_append_c (str, ' ');
		name = nm_setting_match_get_interface_name (s_match, i);
		g_string_append (str, _nm_utils_escape_spaces (name, &to_free));
	}
	RETURN_STR_TO_FREE (g_string_free (str, FALSE));
}

static gboolean
_set_fcn_match_interface_name (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	gsize i;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t", TRUE);
	if (strv) {
		for (i = 0; strv[i]; i++) {
			nm_setting_match_add_interface_name (NM_SETTING_MATCH (setting),
			                                     _nm_utils_unescape_spaces ((char *) strv[i]));
		}
	}
	return TRUE;
}

DEFINE_REMOVER_INDEX_OR_VALUE_DIRECT (_remove_fcn_match_interface_name,
                                      NM_SETTING_MATCH,
                                      nm_setting_match_get_num_interface_names,
                                      nm_setting_match_remove_interface_name,
                                      nm_setting_match_remove_interface_name_by_value)

static gconstpointer
_get_fcn_olpc_mesh_ssid (ARGS_GET_FCN)
{
	NMSettingOlpcMesh *s_olpc_mesh = NM_SETTING_OLPC_MESH (setting);
	GBytes *ssid;
	char *ssid_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	ssid = nm_setting_olpc_mesh_get_ssid (s_olpc_mesh);
	if (ssid) {
		ssid_str = nm_utils_ssid_to_utf8 (g_bytes_get_data (ssid, NULL),
		                                  g_bytes_get_size (ssid));
	}

	NM_SET_OUT (out_is_default, !ssid_str);
	RETURN_STR_TO_FREE (ssid_str);
}

static gboolean
_set_fcn_olpc_mesh_channel (ARGS_SET_FCN)
{
	unsigned long chan_int;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!nmc_string_to_uint (value, TRUE, 1, 13, &chan_int)) {
		g_set_error (error, 1, 0, _("'%s' is not a valid channel; use <1-13>"), value);
		return FALSE;
	}
	g_object_set (setting, property_info->property_name, chan_int, NULL);
	return TRUE;
}

static const char *
_validate_fcn_proxy_pac_script (const char *value, char **out_to_free, GError **error)
{
	char *script = NULL;

	if (!nmc_proxy_check_script (value, &script, error))
		return NULL;
	RETURN_STR_TO_FREE (script);
}

static gconstpointer
_get_fcn_sriov_vfs (ARGS_GET_FCN)
{
	NMSettingSriov *s_sriov = NM_SETTING_SRIOV (setting);
	GString *printable;
	guint num_vfs, i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_vfs = nm_setting_sriov_get_num_vfs (s_sriov);
	for (i = 0; i < num_vfs; i++) {
		gs_free char *str = NULL;
		NMSriovVF *vf;

		vf = nm_setting_sriov_get_vf (s_sriov, i);
		str = nm_utils_sriov_vf_to_str (vf, FALSE, NULL);
		if (!str)
			continue;

		if (printable->len > 0)
			g_string_append (printable, ", ");
		g_string_append (printable, str);
	}

	NM_SET_OUT (out_is_default, num_vfs == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gconstpointer
_get_fcn_tc_config_qdiscs (ARGS_GET_FCN)
{
	NMSettingTCConfig *s_tc = NM_SETTING_TC_CONFIG (setting);
	GString *printable;
	guint num_qdiscs, i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_qdiscs = nm_setting_tc_config_get_num_qdiscs (s_tc);
	for (i = 0; i < num_qdiscs; i++) {
		gs_free char *str = NULL;
		NMTCQdisc *qdisc;

		qdisc = nm_setting_tc_config_get_qdisc (s_tc, i);

		str = nm_utils_tc_qdisc_to_str (qdisc, NULL);
		if (!str)
			continue;

		if (printable->len > 0)
			g_string_append (printable, ", ");
		g_string_append (printable, str);
	}

	NM_SET_OUT (out_is_default, num_qdiscs == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gboolean
_set_fcn_sriov_vfs (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;
	gs_free_error GError *local = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_sriov_vf NMSriovVF *vf = NULL;

		vf = nm_utils_sriov_vf_from_str (*iter, &local);
		if (!vf) {
			nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
			                    "%s. %s",
			                    local->message,
			                    _("The valid syntax is: vf [attribute=value]... [,vf [attribute=value]...]"));
			return FALSE;
		}
		nm_setting_sriov_add_vf (NM_SETTING_SRIOV (setting), vf);
	}
	return TRUE;
}

static gboolean
_set_fcn_tc_config_qdiscs (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;
	gs_free_error GError *local = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_tc_qdisc NMTCQdisc *tc_qdisc = NULL;

		tc_qdisc = nm_utils_tc_qdisc_from_str (*iter, &local);
		if (!tc_qdisc) {
			g_set_error (error, 1, 0, "%s %s", local->message,
			             _("The valid syntax is: '[root | parent <handle>] [handle <handle>] <qdisc>'"));
			return FALSE;
		}
		nm_setting_tc_config_add_qdisc (NM_SETTING_TC_CONFIG (setting), tc_qdisc);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_sriov_vf (NMSettingSriov *setting,
                               const char *value,
                               GError **error)
{
	nm_auto_unref_sriov_vf NMSriovVF *vf = NULL;

	vf = nm_utils_sriov_vf_from_str (value, error);
	if (!vf)
		return FALSE;

	nm_setting_sriov_remove_vf_by_index (setting, nm_sriov_vf_get_index (vf));
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_sriov_vfs,
                                       NM_SETTING_SRIOV,
                                       nm_setting_sriov_get_num_vfs,
                                       nm_setting_sriov_remove_vf,
                                       _validate_and_remove_sriov_vf)

static gboolean
_validate_and_remove_tc_qdisc (NMSettingTCConfig *setting,
                               const char *value,
                               GError **error)
{
	nm_auto_unref_tc_qdisc NMTCQdisc *qdisc = NULL;

	qdisc = nm_utils_tc_qdisc_from_str (value, error);
	if (!qdisc)
		return FALSE;

	nm_setting_tc_config_remove_qdisc_by_value (setting, qdisc);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_tc_config_qdiscs,
                                       NM_SETTING_TC_CONFIG,
                                       nm_setting_tc_config_get_num_qdiscs,
                                       nm_setting_tc_config_remove_qdisc,
                                       _validate_and_remove_tc_qdisc)

static gconstpointer
_get_fcn_tc_config_tfilters (ARGS_GET_FCN)
{
	NMSettingTCConfig *s_tc = NM_SETTING_TC_CONFIG (setting);
	GString *printable;
	guint num_tfilters, i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_tfilters = nm_setting_tc_config_get_num_tfilters (s_tc);
	for (i = 0; i < num_tfilters; i++) {
		NMTCTfilter *tfilter;
		gs_free char *str = NULL;

		tfilter = nm_setting_tc_config_get_tfilter (s_tc, i);

		str = nm_utils_tc_tfilter_to_str (tfilter, NULL);
		if (!str)
			continue;

		if (printable->len > 0)
			g_string_append (printable, ", ");
		g_string_append (printable, str);
	}

	NM_SET_OUT (out_is_default, num_tfilters == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gboolean
_set_fcn_tc_config_tfilters (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;
	gs_free_error GError *local = NULL;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_tc_tfilter NMTCTfilter *tc_tfilter = NULL;

		tc_tfilter = nm_utils_tc_tfilter_from_str (*iter, &local);
		if (!tc_tfilter) {
			nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
			                    "%s %s",
			                    local->message,
			                    _("The valid syntax is: '[root | parent <handle>] [handle <handle>] <tfilter>'"));
			return FALSE;
		}
		nm_setting_tc_config_add_tfilter (NM_SETTING_TC_CONFIG (setting), tc_tfilter);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_tc_tfilter (NMSettingTCConfig *setting,
                               const char *value,
                               GError **error)
{
	nm_auto_unref_tc_tfilter NMTCTfilter *tfilter = NULL;

	tfilter = nm_utils_tc_tfilter_from_str (value, error);
	if (!tfilter)
		return FALSE;

	nm_setting_tc_config_remove_tfilter_by_value (setting, tfilter);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_tc_config_tfilters,
                                       NM_SETTING_TC_CONFIG,
                                       nm_setting_tc_config_get_num_tfilters,
                                       nm_setting_tc_config_remove_tfilter,
                                       _validate_and_remove_tc_tfilter)

static const char *
_validate_fcn_team_config (const char *value, char **out_to_free, GError **error)
{
	char *json = NULL;

	if (!nmc_team_check_config (value, &json, error))
		return NULL;
	RETURN_STR_TO_FREE (json);
}

static gboolean
_is_valid_team_runner_tx_hash_element (const char *tx_hash_element,
                                       GError **error)
{
	nm_assert (!error || !*error);

	if (NM_IN_STRSET (tx_hash_element,
	                  "eth", "vlan", "ipv4", "ipv6", "ip",
	                  "l3", "tcp", "udp", "sctp", "l4")) {
		return TRUE;
	}

	g_set_error (error, 1, 0, "'%s' is not valid. %s", tx_hash_element,
	             "Valid tx-hashes: [eth, vlan, ipv4, ipv6, ip, l3, tcp, udp, sctp, l4]");
	return FALSE;
}

static gboolean
_set_fcn_team_runner_tx_hash (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " \t,", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		if (!_is_valid_team_runner_tx_hash_element (*iter, error))
			return FALSE;
	}

	while (nm_setting_team_get_num_runner_tx_hash (NM_SETTING_TEAM (setting)))
		nm_setting_team_remove_runner_tx_hash (NM_SETTING_TEAM (setting), 0);

	for (iter = strv; strv && *iter; iter++)
		nm_setting_team_add_runner_tx_hash (NM_SETTING_TEAM (setting), *iter);

	return TRUE;
}

DEFINE_REMOVER_INDEX_OR_VALUE_DIRECT (_remove_fcn_team_runner_tx_hash,
                                      NM_SETTING_TEAM,
                                      nm_setting_team_get_num_runner_tx_hash,
                                      nm_setting_team_remove_runner_tx_hash,
                                      nm_setting_team_remove_runner_tx_hash_by_value)

static gconstpointer
_get_fcn_team_link_watchers (ARGS_GET_FCN)
{
	NMSettingTeam *s_team = NM_SETTING_TEAM (setting);
	GString *printable;
	guint num_watchers, i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_watchers = nm_setting_team_get_num_link_watchers (s_team);
	for (i = 0; i < num_watchers; i++) {
		NMTeamLinkWatcher *watcher;
		gs_free char *str = NULL;

		watcher = nm_setting_team_get_link_watcher (s_team, i);

		str = _dump_team_link_watcher (watcher);
		if (!str)
			continue;

		if (printable->len > 0)
			g_string_append (printable, ", ");
		g_string_append (printable, str);
	}

	NM_SET_OUT (out_is_default, num_watchers == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gboolean
_set_fcn_team_link_watchers (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_team_link_watcher NMTeamLinkWatcher *watcher = NULL;

		watcher = _parse_team_link_watcher (*iter, error);
		if (!watcher)
			return FALSE;
		nm_setting_team_add_link_watcher (NM_SETTING_TEAM (setting), watcher);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_team_link_watcher (NMSettingTeam *setting,
                                        const char *value,
                                        GError **error)
{
	nm_auto_unref_team_link_watcher NMTeamLinkWatcher *watcher = NULL;

	watcher = _parse_team_link_watcher (value, error);
	if (!watcher)
		return FALSE;

	nm_setting_team_remove_link_watcher_by_value (setting, watcher);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_team_link_watchers,
                                       NM_SETTING_TEAM,
                                       nm_setting_team_get_num_link_watchers,
                                       nm_setting_team_remove_link_watcher,
                                       _validate_and_remove_team_link_watcher)

static gconstpointer
_get_fcn_team_port_link_watchers (ARGS_GET_FCN)
{
	NMSettingTeamPort *s_team_port = NM_SETTING_TEAM_PORT (setting);
	GString *printable;
	guint num_watchers, i;

	RETURN_UNSUPPORTED_GET_TYPE ();

	printable = g_string_new (NULL);

	num_watchers = nm_setting_team_port_get_num_link_watchers (s_team_port);
	for (i = 0; i < num_watchers; i++) {
		NMTeamLinkWatcher *watcher;
		gs_free char *str = NULL;

		watcher = nm_setting_team_port_get_link_watcher (s_team_port, i);
		str = _dump_team_link_watcher (watcher);
		if (!str)
			continue;

		if (printable->len > 0)
			g_string_append (printable, ", ");
		g_string_append (printable, str);
	}

	NM_SET_OUT (out_is_default, num_watchers == 0);
	RETURN_STR_TO_FREE (g_string_free (printable, FALSE));
}

static gboolean
_set_fcn_team_port_link_watchers (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	const char *const*iter;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, ",", FALSE);
	for (iter = strv; strv && *iter; iter++) {
		nm_auto_unref_team_link_watcher NMTeamLinkWatcher *watcher = NULL;

		watcher = _parse_team_link_watcher (*iter, error);
		if (!watcher)
			return FALSE;
		nm_setting_team_port_add_link_watcher (NM_SETTING_TEAM_PORT (setting), watcher);
	}
	return TRUE;
}

static gboolean
_validate_and_remove_team_port_link_watcher (NMSettingTeamPort *setting,
                                             const char *value,
                                             GError **error)
{
	nm_auto_unref_team_link_watcher NMTeamLinkWatcher *watcher = NULL;

	watcher = _parse_team_link_watcher (value, error);
	if (!watcher)
		return FALSE;

	nm_setting_team_port_remove_link_watcher_by_value (setting, watcher);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_COMPLEX (_remove_fcn_team_port_link_watchers,
                                       NM_SETTING_TEAM_PORT,
                                       nm_setting_team_port_get_num_link_watchers,
                                       nm_setting_team_port_remove_link_watcher,
                                       _validate_and_remove_team_port_link_watcher)

static gconstpointer
_get_fcn_vlan_flags (ARGS_GET_FCN)
{
	NMSettingVlan *s_vlan = NM_SETTING_VLAN (setting);
	guint32 flags;

	RETURN_UNSUPPORTED_GET_TYPE ();

	flags = nm_setting_vlan_get_flags (s_vlan);
	NM_SET_OUT (out_is_default, flags == 0);
	RETURN_STR_TO_FREE (vlan_flags_to_string (flags, get_type));
}

static NMVlanPriorityMap
_vlan_priority_map_type_from_property_info (const NMMetaPropertyInfo *property_info)
{
	nm_assert (property_info);
	nm_assert (property_info->setting_info == &nm_meta_setting_infos_editor[NM_META_SETTING_TYPE_VLAN]);
	nm_assert (NM_IN_STRSET (property_info->property_name, NM_SETTING_VLAN_INGRESS_PRIORITY_MAP,
	                                                       NM_SETTING_VLAN_EGRESS_PRIORITY_MAP));

	return   nm_streq (property_info->property_name, NM_SETTING_VLAN_INGRESS_PRIORITY_MAP)
	       ? NM_VLAN_INGRESS_MAP
	       : NM_VLAN_EGRESS_MAP;
}

static gconstpointer
_get_fcn_vlan_xgress_priority_map (ARGS_GET_FCN)
{
	NMSettingVlan *s_vlan = NM_SETTING_VLAN (setting);
	char *str;

	RETURN_UNSUPPORTED_GET_TYPE ();

	str = vlan_priorities_to_string (s_vlan, _vlan_priority_map_type_from_property_info (property_info));
	NM_SET_OUT (out_is_default, !str || !str[0]);
	RETURN_STR_TO_FREE (str);
}

static gboolean
_set_fcn_vlan_xgress_priority_map (ARGS_SET_FCN)
{
	gs_strfreev char **prio_map = NULL;
	char **p;
	NMVlanPriorityMap map_type = _vlan_priority_map_type_from_property_info (property_info);

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	prio_map = _parse_vlan_priority_maps (value, map_type, FALSE, error);
	if (!prio_map)
		return FALSE;

	for (p = prio_map; p && *p; p++)
		nm_setting_vlan_add_priority_str (NM_SETTING_VLAN (setting), map_type, *p);
	return TRUE;
}

static gboolean
_remove_fcn_vlan_xgress_priority_map (ARGS_REMOVE_FCN)
{
	gs_strfreev char **prio_map = NULL;
	guint32 num;
	gint64 idx;
	gsize i;
	NMVlanPriorityMap map_type = _vlan_priority_map_type_from_property_info (property_info);

	idx = _nm_utils_ascii_str_to_int64 (value, 10, 0, G_MAXINT64, -1);
	if (idx != -1) {
		/* FIXME: this is ugly, if the caller only provides one number, we accept it as
		 * index to remove.
		 * The ugly-ness comes from the fact, that
		 *   " -vlan.ingress-priority-map 0,1,2"
		 * cannot be used to remove the first 3 elements. */
		num = nm_setting_vlan_get_num_priorities (NM_SETTING_VLAN (setting), map_type);
		if (idx < (gint64) num)
			nm_setting_vlan_remove_priority (NM_SETTING_VLAN (setting), map_type, idx);
		return TRUE;
	}

	prio_map = _parse_vlan_priority_maps (value, map_type, TRUE, error);
	if (!prio_map)
		return FALSE;

	for (i = 0; prio_map[i]; i++) {
		nm_setting_vlan_remove_priority_str_by_value (NM_SETTING_VLAN (setting),
		                                              map_type,
		                                              prio_map[i]);
	}
	return TRUE;
}

static gconstpointer
_get_fcn_vpn_data (ARGS_GET_FCN)
{
	NMSettingVpn *s_vpn = NM_SETTING_VPN (setting);
	GString *data_item_str;

	RETURN_UNSUPPORTED_GET_TYPE ();

	data_item_str = g_string_new (NULL);
	nm_setting_vpn_foreach_data_item (s_vpn, &vpn_data_item, data_item_str);
	NM_SET_OUT (out_is_default, data_item_str->len == 0);
	RETURN_STR_TO_FREE (g_string_free (data_item_str, FALSE));
}

static gconstpointer
_get_fcn_vpn_secrets (ARGS_GET_FCN)
{
	NMSettingVpn *s_vpn = NM_SETTING_VPN (setting);
	GString *secret_str;

	RETURN_UNSUPPORTED_GET_TYPE ();

	secret_str = g_string_new (NULL);
	nm_setting_vpn_foreach_secret (s_vpn, &vpn_data_item, secret_str);
	NM_SET_OUT (out_is_default, secret_str->len == 0);
	RETURN_STR_TO_FREE (g_string_free (secret_str, FALSE));
}

static const char *
_validate_vpn_hash_value (const char *option, const char *value, GError **error)
{
	/* nm_setting_vpn_add_data_item() and nm_setting_vpn_add_secret() does not
	 * allow empty strings */
	if (!value || !*value) {
		g_set_error (error, 1, 0, _("'%s' cannot be empty"), option);
		return NULL;
	}
	return value;
}

DEFINE_SETTER_OPTIONS (_set_fcn_vpn_data,
                       NM_SETTING_VPN,
                       NMSettingVpn,
                       nm_setting_vpn_add_data_item,
                       NULL,
                       _validate_vpn_hash_value)
DEFINE_REMOVER_OPTION (_remove_fcn_vpn_data,
                       NM_SETTING_VPN,
                       nm_setting_vpn_remove_data_item)

DEFINE_SETTER_OPTIONS (_set_fcn_vpn_secrets,
                       NM_SETTING_VPN,
                       NMSettingVpn,
                       nm_setting_vpn_add_secret,
                       NULL,
                       _validate_vpn_hash_value)
DEFINE_REMOVER_OPTION (_remove_fcn_vpn_secrets,
                       NM_SETTING_VPN,
                       nm_setting_vpn_remove_secret)

DEFINE_SETTER_MAC_BLACKLIST (_set_fcn_wired_mac_address_blacklist,
                             NM_SETTING_WIRED,
                             nm_setting_wired_add_mac_blacklist_item)

static gboolean
_validate_and_remove_wired_mac_blacklist_item (NMSettingWired *setting,
                                              const char *mac,
                                              GError **error)
{
	guint8 buf[32];

	if (!nm_utils_hwaddr_aton (mac, buf, ETH_ALEN)) {
		nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
		                    _("'%s' is not a valid MAC address"), mac);
		return FALSE;
	}

	nm_setting_wired_remove_mac_blacklist_item_by_value (setting, mac);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_VALIDATING (_remove_fcn_wired_mac_address_blacklist,
                                          NM_SETTING_WIRED,
                                          nm_setting_wired_get_num_mac_blacklist_items,
                                          nm_setting_wired_remove_mac_blacklist_item,
                                          _validate_and_remove_wired_mac_blacklist_item)

static gboolean
_set_fcn_wired_s390_subchannels (ARGS_SET_FCN)
{
	gs_free const char **strv = NULL;
	gsize len;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	strv = nm_utils_strsplit_set (value, " ,\t", FALSE);
	len = NM_PTRARRAY_LEN (strv);
	if (len != 2 && len != 3) {
		g_set_error (error, 1, 0, _("'%s' is not valid; 2 or 3 strings should be provided"),
		             value);
		return FALSE;
	}

	g_object_set (setting, property_info->property_name, strv, NULL);
	return TRUE;
}

static const char *
_validate_s390_option_value (const char *option, const char *value, GError **error)
{
	/*  nm_setting_wired_add_s390_option() requires value len in <1,199> interval */
	if (!value || !*value || strlen (value) >= 200) {
		g_set_error (error, 1, 0, _("'%s' string value should consist of 1 - 199 characters"), option);
		return NULL;
	}
	return value;
}
DEFINE_SETTER_OPTIONS (_set_fcn_wired_s390_options,
                       NM_SETTING_WIRED,
                       NMSettingWired,
                       nm_setting_wired_add_s390_option,
                       nm_setting_wired_get_valid_s390_options,
                       _validate_s390_option_value)
DEFINE_REMOVER_OPTION (_remove_fcn_wired_s390_options,
                       NM_SETTING_WIRED,
                       nm_setting_wired_remove_s390_option)

static const char *const*
_values_fcn__wired_s390_options (ARGS_VALUES_FCN)
{
	return nm_setting_wired_get_valid_s390_options (NULL);
}

static const char *
_describe_fcn_wired_s390_options (ARGS_DESCRIBE_FCN)
{
	gs_free char *options_str = NULL;
	const char **valid_options;
	char *s;

	valid_options = nm_setting_wired_get_valid_s390_options (NULL);

	options_str = g_strjoinv (", ", (char **) valid_options);

	s = g_strdup_printf (_("Enter a list of S/390 options formatted as:\n"
	                       "  option = <value>, option = <value>,...\n"
	                       "Valid options are: %s\n"),
	                       options_str);
	return (*out_to_free = s);
}

static gconstpointer
_get_fcn_wireless_ssid (ARGS_GET_FCN)
{
	NMSettingWireless *s_wireless = NM_SETTING_WIRELESS (setting);
	GBytes *ssid;
	char *ssid_str = NULL;

	RETURN_UNSUPPORTED_GET_TYPE ();

	ssid = nm_setting_wireless_get_ssid (s_wireless);
	if (ssid) {
		ssid_str = nm_utils_ssid_to_utf8 (g_bytes_get_data (ssid, NULL),
		                                  g_bytes_get_size (ssid));
	}

	NM_SET_OUT (out_is_default, !ssid_str || !ssid_str[0]);
	RETURN_STR_TO_FREE (ssid_str);
}

static gboolean
_set_fcn_wireless_channel (ARGS_SET_FCN)
{
	unsigned long chan_int;

	if (_SET_FCN_DO_RESET_DEFAULT (value))
		return _gobject_property_reset_default (setting, property_info->property_name);

	if (!nmc_string_to_uint (value, FALSE, 0, 0, &chan_int)) {
		g_set_error (error, 1, 0, _("'%s' is not a valid channel"), value);
		return FALSE;
	}

	if (   !nm_utils_wifi_is_channel_valid (chan_int, "a")
	    && !nm_utils_wifi_is_channel_valid (chan_int, "bg")) {
		g_set_error (error, 1, 0, _("'%ld' is not a valid channel"), chan_int);
		return FALSE;
	}

	g_object_set (setting, property_info->property_name, chan_int, NULL);
	return TRUE;
}

DEFINE_SETTER_MAC_BLACKLIST (_set_fcn_wireless_mac_address_blacklist,
                             NM_SETTING_WIRELESS,
                             nm_setting_wireless_add_mac_blacklist_item)

static gboolean
_validate_and_remove_wifi_mac_blacklist_item (NMSettingWireless *setting,
                                              const char *mac,
                                              GError **error)
{
	guint8 buf[32];

	if (!nm_utils_hwaddr_aton (mac, buf, ETH_ALEN)) {
		nm_utils_error_set (error, NM_UTILS_ERROR_INVALID_ARGUMENT,
		                    _("'%s' is not a valid MAC address"), mac);
		return FALSE;
	}

	nm_setting_wireless_remove_mac_blacklist_item_by_value (setting, mac);
	return TRUE;
}
DEFINE_REMOVER_INDEX_OR_VALUE_VALIDATING (_remove_fcn_wireless_mac_address_blacklist,
                                          NM_SETTING_WIRELESS,
                                          nm_setting_wireless_get_num_mac_blacklist_items,
                                          nm_setting_wireless_remove_mac_blacklist_item,
                                          _validate_and_remove_wifi_mac_blacklist_item)

static gconstpointer
_get_fcn_wireless_security_wep_key (ARGS_GET_FCN)
{
	NMSettingWirelessSecurity *s_wireless_sec = NM_SETTING_WIRELESS_SECURITY (setting);
	char *key;
	guint index;

	RETURN_UNSUPPORTED_GET_TYPE ();

	nm_assert (g_str_has_prefix (property_info->property_name, "wep-key"));
	nm_assert (NM_IN_SET (property_info->property_name[7], '0', '1', '2', '3'));
	nm_assert (property_info->property_name[8] == '\0');

	index = property_info->property_name[7] - '0';

	key = g_strdup (nm_setting_wireless_security_get_wep_key (s_wireless_sec, index));
	NM_SET_OUT (out_is_default, !key);
	RETURN_STR_TO_FREE (key);
}

static gboolean
_set_fcn_wireless_wep_key (ARGS_SET_FCN)
{
	NMWepKeyType guessed_type = NM_WEP_KEY_TYPE_UNKNOWN;
	NMWepKeyType type;
	guint32 prev_idx, idx;

	nm_assert (!error || !*error);

	if (_SET_FCN_DO_RESET_DEFAULT (value)) {
		g_object_set (setting, property_info->property_name, NULL, NULL);
		return TRUE;
	}

	/* Get currently set type */
	type = nm_setting_wireless_security_get_wep_key_type (NM_SETTING_WIRELESS_SECURITY (setting));

	/* Guess key type */
	if (nm_utils_wep_key_valid (value, NM_WEP_KEY_TYPE_KEY))
		guessed_type = NM_WEP_KEY_TYPE_KEY;
	else if (nm_utils_wep_key_valid (value, NM_WEP_KEY_TYPE_PASSPHRASE))
		guessed_type = NM_WEP_KEY_TYPE_PASSPHRASE;

	if (guessed_type == NM_WEP_KEY_TYPE_UNKNOWN) {
		g_set_error (error, 1, 0, _("'%s' is not valid"), value);
		return FALSE;
	}

	if (type != NM_WEP_KEY_TYPE_UNKNOWN && type != guessed_type) {
		if (nm_utils_wep_key_valid (value, type))
			guessed_type = type;
		else {
			g_set_error (error, 1, 0,
			             _("'%s' not compatible with %s '%s', please change the key or set the right %s first."),
			             value, NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE, wep_key_type_to_string (type),
			             NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE);
			return FALSE;
		}
	}
	prev_idx = nm_setting_wireless_security_get_wep_tx_keyidx (NM_SETTING_WIRELESS_SECURITY (setting));
	idx = property_info->property_name[strlen (property_info->property_name) - 1] - '0';
	_env_warn_fcn (environment, environment_user_data,
	               NM_META_ENV_WARN_LEVEL_INFO,
	               N_("WEP key is guessed to be of '%s'"),
	               wep_key_type_to_string (guessed_type));
	if (idx != prev_idx) {
		_env_warn_fcn (environment, environment_user_data,
		               NM_META_ENV_WARN_LEVEL_INFO,
		               N_("WEP key index set to '%d'"),
		               idx);
	}

	g_object_set (setting, property_info->property_name, value, NULL);
	g_object_set (setting, NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE, guessed_type, NULL);
	if (idx != prev_idx)
		g_object_set (setting, NM_SETTING_WIRELESS_SECURITY_WEP_TX_KEYIDX, idx, NULL);
	return TRUE;
}

static void
_gobject_enum_pre_set_notify_fcn_wireless_security_wep_key_type (const NMMetaPropertyInfo *property_info,
                                                                 const NMMetaEnvironment *environment,
                                                                 gpointer environment_user_data,
                                                                 NMSetting *setting,
                                                                 int value)
{
	guint i;
	const char *key;
	const char *keynames[] = {
		NM_SETTING_WIRELESS_SECURITY_WEP_KEY0,
		NM_SETTING_WIRELESS_SECURITY_WEP_KEY1,
		NM_SETTING_WIRELESS_SECURITY_WEP_KEY2,
		NM_SETTING_WIRELESS_SECURITY_WEP_KEY3,
	};

	/* Check type compatibility with set keys */
	if (!NM_IN_SET (value,
	                NM_WEP_KEY_TYPE_UNKNOWN,
	                NM_WEP_KEY_TYPE_KEY,
	                NM_WEP_KEY_TYPE_PASSPHRASE))
		return;

	for (i = 0; i < 4; i++) {
		key = nm_setting_wireless_security_get_wep_key (NM_SETTING_WIRELESS_SECURITY (setting), i);
		if (key && !nm_utils_wep_key_valid (key, value)) {
			_env_warn_fcn (environment, environment_user_data,
			               NM_META_ENV_WARN_LEVEL_WARN,
			               N_("'%s' is not compatible with '%s' type, please change or delete the key."),
			               keynames[i], wep_key_type_to_string (value));
		}
	}
}

/*****************************************************************************/

static gconstpointer
_get_fcn_ethtool (ARGS_GET_FCN)
{
	const char *s;
	NMTernary val;
	NMEthtoolID ethtool_id = property_info->property_typ_data->subtype.ethtool.ethtool_id;

	RETURN_UNSUPPORTED_GET_TYPE ();

	val = nm_setting_ethtool_get_feature (NM_SETTING_ETHTOOL (setting),
	                                      nm_ethtool_data[ethtool_id]->optname);

	if (val == NM_TERNARY_TRUE)
		s = N_("on");
	else if (val == NM_TERNARY_FALSE)
		s = N_("off");
	else {
		s = NULL;
		NM_SET_OUT (out_is_default, TRUE);
		*out_flags |= NM_META_ACCESSOR_GET_OUT_FLAGS_HIDE;
	}

	if (s && get_type == NM_META_ACCESSOR_GET_TYPE_PRETTY)
		s = gettext (s);
	return s;
}

static gboolean
_set_fcn_ethtool (ARGS_SET_FCN)
{
	gs_free char *value_to_free = NULL;
	NMTernary val;
	NMEthtoolID ethtool_id = property_info->property_typ_data->subtype.ethtool.ethtool_id;

	if (_SET_FCN_DO_RESET_DEFAULT (value)) {
		val = NM_TERNARY_DEFAULT;
		goto set;
	}

	value = nm_strstrip_avoid_copy (value, &value_to_free);

	if (NM_IN_STRSET (value, "1", "yes", "true", "on"))
		val = NM_TERNARY_TRUE;
	else if (NM_IN_STRSET (value, "0", "no", "false", "off"))
		val = NM_TERNARY_FALSE;
	else if (NM_IN_STRSET (value, "", "ignore", "default"))
		val = NM_TERNARY_DEFAULT;
	else {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_INVALID_ARGUMENT,
		             _("'%s' is not valid; use 'on', 'off', or 'ignore'"),
		             value);
		return FALSE;
	}

set:
	nm_setting_ethtool_set_feature (NM_SETTING_ETHTOOL (setting),
	                                nm_ethtool_data[ethtool_id]->optname,
	                                val);
	return TRUE;
}

static const char *const*
_complete_fcn_ethtool (ARGS_COMPLETE_FCN)
{
	static const char *const v[] = {
		"true",
		"false",
		"1",
		"0",
		"yes",
		"no",
		"default",
		"on",
		"off",
		"ignore",
		NULL,
	};

	if (!text || !text[0])
		return &v[7];
	return v;
}

/*****************************************************************************/

static const NMMetaPropertyInfo property_info_BOND_OPTIONS;

#define NESTED_PROPERTY_INFO_BOND(...) \
	.parent_info =                  &property_info_BOND_OPTIONS, \
	.base = { \
		.meta_type =                &nm_meta_type_nested_property_info, \
		.setting_info =             &nm_meta_setting_infos_editor[NM_META_SETTING_TYPE_BOND], \
		__VA_ARGS__ \
	}

static const NMMetaNestedPropertyInfo meta_nested_property_infos_bond[] = {
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "mode",
			.prompt =                   NM_META_TEXT_PROMPT_BOND_MODE,
			.def_hint =                 "[balance-rr]",
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "primary",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("Bonding primary interface [none]"),
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			/* this is a virtual property, only needed during "ask" mode. */
			.prompt =                   N_("Bonding monitoring mode"),
			.def_hint =                 NM_META_TEXT_PROMPT_BOND_MON_MODE_CHOICES,
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "miimon",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("Bonding miimon [100]"),
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "downdelay",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("Bonding downdelay [0]"),
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "updelay",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("Bonding updelay [0]"),
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "arp-interval",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("Bonding arp-interval [0]"),
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "arp-ip-target",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("Bonding arp-ip-target [none]"),
		)
	},
	{
		NESTED_PROPERTY_INFO_BOND (
			.property_name =            NM_SETTING_BOND_OPTIONS,
			.property_alias =           "lacp-rate",
			.inf_flags =                NM_META_PROPERTY_INF_FLAG_DONT_ASK,
			.prompt =                   N_("LACP rate ('slow' or 'fast') [slow]"),
		)
	},
};

const NMMetaPropertyTypDataNested nm_meta_property_typ_data_bond = {
	.nested =               meta_nested_property_infos_bond,
	.nested_len =           G_N_ELEMENTS (meta_nested_property_infos_bond),
};

/*****************************************************************************/

#define DEFINE_PROPERTY_TYPE(...) \
	(&((NMMetaPropertyType) { __VA_ARGS__ } ))

#define DEFINE_PROPERTY_TYP_DATA(...) \
	(&((NMMetaPropertyTypData) { __VA_ARGS__ } ))

#define PROPERTY_TYP_DATA_SUBTYPE(stype, ...) \
	.subtype = { \
		.stype = { __VA_ARGS__ }, \
	}

#define DEFINE_PROPERTY_TYP_DATA_SUBTYPE(stype, ...) \
	DEFINE_PROPERTY_TYP_DATA ( \
		PROPERTY_TYP_DATA_SUBTYPE (stype, __VA_ARGS__), \
	)

static const NMMetaPropertyType _pt_gobject_readonly = {
	.get_fcn =                      _get_fcn_gobject,
};

static const NMMetaPropertyType _pt_gobject_string = {
	.get_fcn =                      _get_fcn_gobject,
	.set_fcn =                      _set_fcn_gobject_string,
};

static const NMMetaPropertyType _pt_gobject_bool = {
	.get_fcn =                      _get_fcn_gobject,
	.set_fcn =                      _set_fcn_gobject_bool,
	.complete_fcn =                 _complete_fcn_gobject_bool,
};

static const NMMetaPropertyType _pt_gobject_int = {
	.get_fcn =                      _get_fcn_gobject_int,
	.set_fcn =                      _set_fcn_gobject_int,
};

static const NMMetaPropertyType _pt_gobject_mtu = {
	.get_fcn =                      _get_fcn_gobject_mtu,
	.set_fcn =                      _set_fcn_gobject_mtu,
};

static const NMMetaPropertyType _pt_gobject_bytes = {
	.get_fcn =                     _get_fcn_gobject_bytes,
	.set_fcn =                     _set_fcn_gobject_bytes,
};

static const NMMetaPropertyType _pt_gobject_mac = {
	.get_fcn =                      _get_fcn_gobject,
	.set_fcn =                      _set_fcn_gobject_mac,
};

static const NMMetaPropertyType _pt_gobject_secret_flags = {
	.get_fcn =                      _get_fcn_gobject_secret_flags,
	.set_fcn =                      _set_fcn_gobject_enum,
	.values_fcn =                   _values_fcn_gobject_enum,
};

static const NMMetaPropertyType _pt_gobject_enum = {
	.get_fcn =                      _get_fcn_gobject_enum,
	.set_fcn =                      _set_fcn_gobject_enum,
	.values_fcn =                   _values_fcn_gobject_enum,
};

static const NMMetaPropertyType _pt_gobject_devices = {
	.get_fcn =                      _get_fcn_gobject,
	.set_fcn =                      _set_fcn_gobject_string,
	.complete_fcn =                 _complete_fcn_gobject_devices,
};

static const NMMetaPropertyType _pt_dcb_flags = {
	.get_fcn =                      _get_fcn_dcb_flags,
	.set_fcn =                      _set_fcn_dcb_flags,
};

static const NMMetaPropertyType _pt_dcb_bool = {
	.get_fcn =                      _get_fcn_dcb_bool,
	.set_fcn =                      _set_fcn_dcb_bool,
};

static const NMMetaPropertyType _pt_dcb = {
	.get_fcn =                      _get_fcn_dcb,
	.set_fcn =                      _set_fcn_dcb,
};

static const NMMetaPropertyType _pt_ethtool = {
	.get_fcn =                      _get_fcn_ethtool,
	.set_fcn =                      _set_fcn_ethtool,
	.complete_fcn =                 _complete_fcn_ethtool,
};

#define MULTILIST_GET_NUM_FCN(type, func)         (((func) == ((guint32  (*) (type *              )) (func))) ? ((guint32  (*) (NMSetting *              )) (func)) : NULL)
#define MULTILIST_ADD_FCN(type, func)             (((func) == ((gboolean (*) (type *, const char *)) (func))) ? ((gboolean (*) (NMSetting *, const char *)) (func)) : NULL)
#define MULTILIST_REMOVE_BY_IDX_FCN(type, func)   (((func) == ((void     (*) (type *, guint32     )) (func))) ? ((void     (*) (NMSetting *, guint32     )) (func)) : NULL)
#define MULTILIST_REMOVE_BY_VALUE_FCN(type, func) (((func) == ((gboolean (*) (type *, const char *)) (func))) ? ((gboolean (*) (NMSetting *, const char *)) (func)) : NULL)

static const NMMetaPropertyType _pt_multilist = {
	.get_fcn =                      _get_fcn_gobject,
	.set_fcn =                      _set_fcn_multilist,
	.remove_fcn =                   _remove_fcn_multilist,
};

/*****************************************************************************/

#include "settings-docs.h"

/*****************************************************************************/

#define PROPERTY_INFO_INIT(name, doc, ...) \
	{ \
		.meta_type =                    &nm_meta_type_property_info, \
		.setting_info =                 &nm_meta_setting_infos_editor[_CURRENT_NM_META_SETTING_TYPE], \
		.property_name =                name, \
		.describe_doc =                 doc, \
		__VA_ARGS__ \
	}

#define PROPERTY_INFO(name, doc, ...) \
	(&((const NMMetaPropertyInfo) PROPERTY_INFO_INIT (name, doc, __VA_ARGS__)))

#define PROPERTY_INFO_WITH_DESC(name, ...) \
	PROPERTY_INFO (name, DESCRIBE_DOC_##name, ##__VA_ARGS__)

#define ENUM_VALUE_INFOS(...)  (((const NMUtilsEnumValueInfo    []) { __VA_ARGS__ { .nick = NULL, }, }))
#define INT_VALUE_INFOS(...)   (((const NMMetaUtilsIntValueInfo []) { __VA_ARGS__ { .nick = NULL, }, }))

#define GET_FCN_WITH_DEFAULT(type, func) \
	/* macro that returns @func as const (gboolean(*)(NMSetting*)) type, but checks
	 * that the actual type is (gboolean(*)(type *)). */ \
	((gboolean (*) (NMSetting *)) ((sizeof (func == ((gboolean (*) (type *)) func))) ? func : func) )

#define MTU_GET_FCN(type, func) \
	/* macro that returns @func as const (guint32(*)(NMSetting*)) type, but checks
	 * that the actual type is (guint32(*)(type *)). */ \
	((guint32 (*) (NMSetting *)) ((sizeof (func == ((guint32 (*) (type *)) func))) ? func : func) )

#define TEAM_DESCRIBE_MESSAGE \
	N_("nmcli can accepts both direct JSON configuration data and a file name containing " \
	   "the configuration. In the latter case the file is read and the contents is put " \
	   "into this property.\n\n" \
	   "Examples: set team.config " \
	   "{ \"device\": \"team0\", \"runner\": {\"name\": \"roundrobin\"}, \"ports\": {\"eth1\": {}, \"eth2\": {}} }\n" \
	   "          set team.config /etc/my-team.conf\n")

#define TEAM_LINK_WATCHERS_DESCRIBE_MESSAGE \
	N_("Enter a list of link watchers formatted as dictionaries where the keys " \
	   "are teamd properties. Dictionary pairs are in the form: key=value and pairs " \
	   "are separated by ' '. Dictionaries are separated with ','.\n" \
	   "The keys allowed/required in the dictionary change on the basis of the link " \
	   "watcher type, while the only property common to all the link watchers is " \
	   " 'name'*, which defines the link watcher to be specified.\n\n" \
	   "Properties available for the 'ethtool' link watcher:\n" \
	   "  'delay-up', 'delay-down'\n\n" \
	   "Properties available for the 'nsna_ping' link watcher:\n" \
	   "  'init-wait', 'interval', 'missed-max', 'target-host'*\n\n" \
	   "Properties available for the 'arp_ping' include all the ones for 'nsna_ping' and:\n" \
	   "  'source-host'*, 'validate-active', 'validate-inactive', 'send-always'.\n\n" \
	   "Properties flagged with a '*' are mandatory.\n\n" \
	   "Example:\n" \
	   "   name=arp_ping source-host=172.16.1.1 target-host=172.16.1.254, name=ethtool delay-up=3\n")

#define DEFINE_DCB_PROPRITY_PROPERTY_TYPE \
		.property_type =                &_pt_gobject_int, \
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int, \
			.value_infos =              INT_VALUE_INFOS ( \
				{ \
					.value.i64 = -1, \
					.nick = "unset", \
				}, \
			), \
		),

#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_6LOWPAN
static const NMMetaPropertyInfo *const property_infos_6LOWPAN[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_6LOWPAN_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "dev",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("IEEE 802.15.4 (WPAN) parent device or connection UUID"),
		.property_type =                &_pt_gobject_string,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_802_1X
static const NMMetaPropertyInfo *const property_infos_802_1X[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_EAP,
		.property_type =                &_pt_multilist,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (multilist,
				.get_num_fcn =          MULTILIST_GET_NUM_FCN         (NMSetting8021x, nm_setting_802_1x_get_num_eap_methods),
				.add_fcn =              MULTILIST_ADD_FCN             (NMSetting8021x, nm_setting_802_1x_add_eap_method),
				.remove_by_idx_fcn =    MULTILIST_REMOVE_BY_IDX_FCN   (NMSetting8021x, nm_setting_802_1x_remove_eap_method),
				.remove_by_value_fcn =  MULTILIST_REMOVE_BY_VALUE_FCN (NMSetting8021x, nm_setting_802_1x_remove_eap_method_by_value),
			),
			.values_static =            NM_MAKE_STRV ("leap", "md5", "tls", "peap", "ttls", "sim", "fast", "pwd"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_IDENTITY,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_ANONYMOUS_IDENTITY,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PAC_FILE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CA_CERT,
		.describe_message =
		    N_("Enter file path to CA certificate (optionally prefixed with file://).\n"
		       "  [file://]<file path>\n"
		       "Note that nmcli does not support specifying certificates as raw blob data.\n"
		       "Example: /home/cimrman/cacert.crt\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_802_1x_ca_cert,
			.set_fcn =                  _set_fcn_802_1x_ca_cert,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CA_CERT_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CA_CERT_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CA_PATH,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_SUBJECT_MATCH,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_ALTSUBJECT_MATCHES,
		.property_type =                &_pt_multilist,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (multilist,
				.get_num_fcn =          MULTILIST_GET_NUM_FCN         (NMSetting8021x, nm_setting_802_1x_get_num_altsubject_matches),
				.add_fcn =              MULTILIST_ADD_FCN             (NMSetting8021x, nm_setting_802_1x_add_altsubject_match),
				.remove_by_idx_fcn =    MULTILIST_REMOVE_BY_IDX_FCN   (NMSetting8021x, nm_setting_802_1x_remove_altsubject_match),
				.remove_by_value_fcn =  MULTILIST_REMOVE_BY_VALUE_FCN (NMSetting8021x, nm_setting_802_1x_remove_altsubject_match_by_value),
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_DOMAIN_SUFFIX_MATCH,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CLIENT_CERT,
		.describe_message =
		    N_("Enter file path to client certificate (optionally prefixed with file://).\n"
		       "  [file://]<file path>\n"
		       "Note that nmcli does not support specifying certificates as raw blob data.\n"
		       "Example: /home/cimrman/jara.crt\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_802_1x_client_cert,
			.set_fcn =                  _set_fcn_802_1x_client_cert,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CLIENT_CERT_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_CLIENT_CERT_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE1_PEAPVER,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("0", "1"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE1_PEAPLABEL,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("0", "1"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE1_FAST_PROVISIONING,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("0", "1", "2", "3"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE1_AUTH_FLAGS,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_802_1x_auth_flags_get_type,
			),
			.typ_flags =                NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_AUTH,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("pap", "chap", "mschap", "mschapv2", "gtc", "otp", "md5", "tls"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_AUTHEAP,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("md5", "mschapv2", "otp", "gtc", "tls"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CA_CERT,
		.describe_message =
		    N_("Enter file path to CA certificate for inner authentication (optionally prefixed\n"
		       "with file://).\n"
		       "  [file://]<file path>\n"
		       "Note that nmcli does not support specifying certificates as raw blob data.\n"
		       "Example: /home/cimrman/ca-zweite-phase.crt\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_802_1x_phase2_ca_cert,
			.set_fcn =                  _set_fcn_802_1x_phase2_ca_cert,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CA_CERT_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CA_CERT_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CA_PATH,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_SUBJECT_MATCH,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_ALTSUBJECT_MATCHES,
		.property_type =                &_pt_multilist,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (multilist,
				.get_num_fcn =          MULTILIST_GET_NUM_FCN         (NMSetting8021x, nm_setting_802_1x_get_num_phase2_altsubject_matches),
				.add_fcn =              MULTILIST_ADD_FCN             (NMSetting8021x, nm_setting_802_1x_add_phase2_altsubject_match),
				.remove_by_idx_fcn =    MULTILIST_REMOVE_BY_IDX_FCN   (NMSetting8021x, nm_setting_802_1x_remove_phase2_altsubject_match),
				.remove_by_value_fcn =  MULTILIST_REMOVE_BY_VALUE_FCN (NMSetting8021x, nm_setting_802_1x_remove_phase2_altsubject_match_by_value),
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_DOMAIN_SUFFIX_MATCH,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CLIENT_CERT,
		.describe_message =
		    N_("Enter file path to client certificate for inner authentication (optionally prefixed\n"
		       "with file://).\n"
		       "  [file://]<file path>\n"
		       "Note that nmcli does not support specifying certificates as raw blob data.\n"
		       "Example: /home/cimrman/jara-zweite-phase.crt\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_802_1x_phase2_client_cert,
			.set_fcn =                  _set_fcn_802_1x_phase2_client_cert,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CLIENT_CERT_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_CLIENT_CERT_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PASSWORD_RAW,
		.is_secret =                    TRUE,
		.describe_message =
		    N_("Enter bytes as a list of hexadecimal values.\n"
		       "Two formats are accepted:\n"
		       "(a) a string of hexadecimal digits, where each two digits represent one byte\n"
		       "(b) space-separated list of bytes written as hexadecimal digits "
		       "(with optional 0x/0X prefix, and optional leading 0).\n\n"
		       "Examples: ab0455a6ea3a74C2\n"
		       "          ab 4 55 0xa6 ea 3a 74 C2\n"),
		.property_type =                 &_pt_gobject_bytes,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_bytes,
			.legacy_format =            TRUE,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PASSWORD_RAW_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PRIVATE_KEY,
		.describe_message =
		    N_("Enter path to a private key and the key password (if not set yet):\n"
		       "  [file://]<file path> [<password>]\n"
		       "Note that nmcli does not support specifying private key as raw blob data.\n"
		       "Example: /home/cimrman/jara-priv-key Dardanely\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_802_1x_private_key,
			.set_fcn =                  _set_fcn_802_1x_private_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PRIVATE_KEY_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PRIVATE_KEY_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_PRIVATE_KEY,
		.describe_message =
		    N_("Enter path to a private key and the key password (if not set yet):\n"
		       "  [file://]<file path> [<password>]\n"
		       "Note that nmcli does not support specifying private key as raw blob data.\n"
		       "Example: /home/cimrman/jara-priv-key Dardanely\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_802_1x_phase2_private_key,
			.set_fcn =                  _set_fcn_802_1x_phase2_private_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_PRIVATE_KEY_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PHASE2_PRIVATE_KEY_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PIN,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_PIN_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_SYSTEM_CA_CERTS,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_802_1X_AUTH_TIMEOUT,
		.property_type =                &_pt_gobject_int,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_ADSL
static const NMMetaPropertyInfo *const property_infos_ADSL[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_USERNAME,
		.is_cli_option =                TRUE,
		.property_alias =               "username",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("Username"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_PASSWORD,
		.is_cli_option =                TRUE,
		.property_alias =               "password",
		.prompt =                       N_("Password [none]"),
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_PROTOCOL,
		.is_cli_option =                TRUE,
		.property_alias =               "protocol",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       NM_META_TEXT_PROMPT_ADSL_PROTO,
		.def_hint =                     NM_META_TEXT_PROMPT_ADSL_PROTO_CHOICES,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_ADSL_PROTOCOL_PPPOA,
			                                          NM_SETTING_ADSL_PROTOCOL_PPPOE,
			                                          NM_SETTING_ADSL_PROTOCOL_IPOATM),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_ENCAPSULATION,
		.is_cli_option =                TRUE,
		.property_alias =               "encapsulation",
		.prompt =                       NM_META_TEXT_PROMPT_ADSL_ENCAP,
		.def_hint =                     NM_META_TEXT_PROMPT_ADSL_ENCAP_CHOICES,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_ADSL_ENCAPSULATION_VCMUX,
			                                          NM_SETTING_ADSL_ENCAPSULATION_LLC),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_VPI,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_ADSL_VCI,
		.property_type =                &_pt_gobject_int,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_BLUETOOTH
static const NMMetaPropertyInfo *const property_infos_BLUETOOTH[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BLUETOOTH_BDADDR,
		.is_cli_option =                TRUE,
		.property_alias =               "addr",
		.prompt =                       N_("Bluetooth device address"),
		.property_type =                &_pt_gobject_mac,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BLUETOOTH_TYPE,
		.is_cli_option =                TRUE,
		.property_alias =               "bt-type",
		.prompt =                       NM_META_TEXT_PROMPT_BT_TYPE,
		.def_hint =                     NM_META_TEXT_PROMPT_BT_TYPE_CHOICES,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_BLUETOOTH_TYPE_DUN,
			                                          NM_SETTING_BLUETOOTH_TYPE_PANU,
			                                          NM_SETTING_BLUETOOTH_TYPE_NAP),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_BOND
static const NMMetaPropertyInfo property_info_BOND_OPTIONS =
	PROPERTY_INFO_INIT (NM_SETTING_BOND_OPTIONS, DESCRIBE_DOC_NM_SETTING_BOND_OPTIONS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.describe_fcn =             _describe_fcn_bond_options,
			.get_fcn =                  _get_fcn_bond_options,
			.set_fcn =                  _set_fcn_bond_options,
			.remove_fcn =               _remove_fcn_bond_options,
			.values_fcn =               _values_fcn_bond_options,
		),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.nested =                   &nm_meta_property_typ_data_bond,
		),
	);

static const NMMetaPropertyInfo *const property_infos_BOND[] = {
	&property_info_BOND_OPTIONS,
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_BRIDGE
static const NMMetaPropertyInfo *const property_infos_BRIDGE[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_MAC_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "mac",
		.prompt =                       N_("MAC [none]"),
		.property_type =                &_pt_gobject_mac,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_STP,
		.is_cli_option =                TRUE,
		.property_alias =               "stp",
		.prompt =                       N_("Enable STP [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_PRIORITY,
		.is_cli_option =                TRUE,
		.property_alias =               "priority",
		.prompt =                       N_("STP priority [32768]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_FORWARD_DELAY,
		.is_cli_option =                TRUE,
		.property_alias =               "forward-delay",
		.prompt =                       N_("Forward delay [15]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_HELLO_TIME,
		.is_cli_option =                TRUE,
		.property_alias =               "hello-time",
		.prompt =                       N_("Hello time [2]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_MAX_AGE,
		.is_cli_option =                TRUE,
		.property_alias =               "max-age",
		.prompt =                       N_("Max age [20]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_AGEING_TIME,
		.is_cli_option =                TRUE,
		.property_alias =               "ageing-time",
		.prompt =                       N_("MAC address ageing time [300]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_GROUP_FORWARD_MASK,
		.is_cli_option =                TRUE,
		.property_alias =               "group-forward-mask",
		.prompt =                       N_("Group forward mask [0]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_MULTICAST_SNOOPING,
		.is_cli_option =                TRUE,
		.property_alias =               "multicast-snooping",
		.prompt =                       N_("Enable IGMP snooping [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_BRIDGE_PORT
static const NMMetaPropertyInfo *const property_infos_BRIDGE_PORT[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_PORT_PRIORITY,
		.is_cli_option =                TRUE,
		.property_alias =               "priority",
		.prompt =                       N_("Bridge port priority [32]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_PORT_PATH_COST,
		.is_cli_option =                TRUE,
		.property_alias =               "path-cost",
		.prompt =                       N_("Bridge port STP path cost [100]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_BRIDGE_PORT_HAIRPIN_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "hairpin",
		.prompt =                       N_("Hairpin [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_CDMA
static const NMMetaPropertyInfo *const property_infos_CDMA[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CDMA_NUMBER,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CDMA_USERNAME,
		.is_cli_option =                TRUE,
		.property_alias =               "user",
		.prompt =                       N_("Username [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CDMA_PASSWORD,
		.is_cli_option =                TRUE,
		.property_alias =               "password",
		.prompt =                       N_("Password [none]"),
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CDMA_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CDMA_MTU,
		.property_type =                &_pt_gobject_mtu,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mtu,
			.get_fcn =                  MTU_GET_FCN (NMSettingCdma, nm_setting_cdma_get_mtu),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_CONNECTION
static const NMMetaPropertyInfo *const property_infos_CONNECTION[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_ID,
		.is_cli_option =                TRUE,
		.property_alias =               "con-name",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_DONT_ASK,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_UUID,
		.property_type =                DEFINE_PROPERTY_TYPE ( .get_fcn = _get_fcn_gobject ),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_STABLE_ID,
		.property_type =                &_pt_gobject_string,
	),
[_NM_META_PROPERTY_TYPE_CONNECTION_TYPE] =
		PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_TYPE,
			.is_cli_option =                TRUE,
			.property_alias =               "type",
			.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
			.prompt =                       NM_META_TEXT_PROMPT_CON_TYPE,
			.property_type = DEFINE_PROPERTY_TYPE (
				.get_fcn =                  _get_fcn_gobject,
				.set_fcn =                  _set_fcn_connection_type,
				.complete_fcn =             _complete_fcn_connection_type,
			),
		),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_INTERFACE_NAME,
		.is_cli_option =                TRUE,
		.property_alias =               "ifname",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       NM_META_TEXT_PROMPT_IFNAME,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_gobject_ifname,
			.complete_fcn =             _complete_fcn_gobject_devices,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_AUTOCONNECT,
		.is_cli_option =                TRUE,
		.property_alias =               "autoconnect",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_DONT_ASK,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_AUTOCONNECT_PRIORITY,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_AUTOCONNECT_RETRIES,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = -1,
					.nick = "default",
				},
				{
					.value.i64 = 0,
					.nick = "forever",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_MULTI_CONNECT,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_connection_multi_connect_get_type,
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_AUTH_RETRIES,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_TIMESTAMP,
		.property_type =                &_pt_gobject_readonly,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_READ_ONLY,
		.property_type =                &_pt_gobject_readonly,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_PERMISSIONS,
		.describe_message =
		     N_("Enter a list of user permissions. This is a list of user names formatted as:\n"
		        "  [user:]<user name 1>, [user:]<user name 2>,...\n"
		        "The items can be separated by commas or spaces.\n\n"
		        "Example: alice bob charlie\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_connection_permissions,
			.set_fcn =                  _set_fcn_connection_permissions,
			.remove_fcn =               _remove_fcn_connection_permissions,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_ZONE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_MASTER,
		.is_cli_option =                TRUE,
		.property_alias =               "master",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_DONT_ASK,
		.prompt =                       NM_META_TEXT_PROMPT_MASTER,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_connection_master,
			.complete_fcn =             _complete_fcn_connection_master,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_SLAVE_TYPE,
		.is_cli_option =                TRUE,
		.property_alias =               "slave-type",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_DONT_ASK,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_BOND_SETTING_NAME,
			                                          NM_SETTING_BRIDGE_SETTING_NAME,
			                                          NM_SETTING_OVS_BRIDGE_SETTING_NAME,
			                                          NM_SETTING_OVS_PORT_SETTING_NAME,
			                                          NM_SETTING_TEAM_SETTING_NAME),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_AUTOCONNECT_SLAVES,
		.property_type =                &_pt_gobject_enum,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_SECONDARIES,
		.describe_message =
		    N_("Enter secondary connections that should be activated when this connection is\n"
		       "activated. Connections can be specified either by UUID or ID (name). nmcli\n"
		       "transparently translates names to UUIDs. Note that NetworkManager only supports\n"
		       "VPNs as secondary connections at the moment.\n"
		       "The items can be separated by commas or spaces.\n\n"
		       "Example: private-openvpn, fe6ba5d8-c2fc-4aae-b2e3-97efddd8d9a7\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_connection_secondaries,
			.remove_fcn =               _remove_fcn_connection_secondaries,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_GATEWAY_PING_TIMEOUT,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_METERED,
		.describe_message =
		    N_("Enter a value which indicates whether the connection is subject to a data\n"
		       "quota, usage costs or other limitations. Accepted options are:\n"
		       "'true','yes','on' to set the connection as metered\n"
		       "'false','no','off' to set the connection as not metered\n"
		       "'unknown' to let NetworkManager choose a value using some heuristics\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_connection_metered,
			.set_fcn =                  _set_fcn_connection_metered,
		),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("yes", "no", "unknown"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_LLDP,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_connection_lldp_get_type,
				.value_infos =          ENUM_VALUE_INFOS (
					{
						.value = NM_SETTING_CONNECTION_LLDP_ENABLE_RX,
						.nick = "enable",
					},
				),
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_MDNS,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_connection_mdns_get_type,
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_CONNECTION_LLMNR,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_connection_llmnr_get_type,
			),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_DCB
static const NMMetaPropertyInfo *const property_infos_DCB[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_FCOE_FLAGS,
		.property_type =                &_pt_dcb_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_FCOE_PRIORITY,
		DEFINE_DCB_PROPRITY_PROPERTY_TYPE
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_FCOE_MODE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_DCB_FCOE_MODE_FABRIC,
			                                           NM_SETTING_DCB_FCOE_MODE_VN2VN),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_ISCSI_FLAGS,
		.property_type =                &_pt_dcb_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_ISCSI_PRIORITY,
		DEFINE_DCB_PROPRITY_PROPERTY_TYPE
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_FIP_FLAGS,
		.property_type =                &_pt_dcb_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_APP_FIP_PRIORITY,
		DEFINE_DCB_PROPRITY_PROPERTY_TYPE
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_FLOW_CONTROL_FLAGS,
		.property_type =                &_pt_dcb_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_FLOW_CONTROL,
		.property_type =                &_pt_dcb_bool,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (dcb_bool,
			.get_fcn                    = nm_setting_dcb_get_priority_flow_control,
			.set_fcn                    = nm_setting_dcb_set_priority_flow_control,
			.with_flow_control_flags    = TRUE,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_GROUP_FLAGS,
		.property_type =                &_pt_dcb_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_GROUP_ID,
		.property_type =                &_pt_dcb,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (dcb,
			.get_fcn =                  nm_setting_dcb_get_priority_group_id,
			.set_fcn =                  nm_setting_dcb_set_priority_group_id,
			.max =                      7,
			.other =                    15,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_GROUP_BANDWIDTH,
		.property_type =                &_pt_dcb,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (dcb,
			.get_fcn =                  nm_setting_dcb_get_priority_group_bandwidth,
			.set_fcn =                  nm_setting_dcb_set_priority_group_bandwidth,
			.max =                      100,
			.other =                    0,
			.is_percent =               TRUE,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_BANDWIDTH,
		.property_type =                &_pt_dcb,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (dcb,
			.get_fcn =                  nm_setting_dcb_get_priority_bandwidth,
			.set_fcn =                  nm_setting_dcb_set_priority_bandwidth,
			.max =                      100,
			.other =                    0,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_STRICT_BANDWIDTH,
		.property_type =                &_pt_dcb_bool,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (dcb_bool,
			.get_fcn                    = nm_setting_dcb_get_priority_strict_bandwidth,
			.set_fcn                    = nm_setting_dcb_set_priority_strict_bandwidth,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_DCB_PRIORITY_TRAFFIC_CLASS,
		.property_type =                &_pt_dcb,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (dcb,
			.get_fcn =                  nm_setting_dcb_get_priority_traffic_class,
			.set_fcn =                  nm_setting_dcb_set_priority_traffic_class,
			.max =                      7,
			.other =                    0,
		),
	),
	NULL
};

#define PROPERTY_INFO_ETHTOOL(xname) \
	PROPERTY_INFO (NM_ETHTOOL_OPTNAME_##xname, NULL, \
		.property_type = &_pt_ethtool, \
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (ethtool, \
			.ethtool_id = NM_ETHTOOL_ID_##xname, \
		), \
	)

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_ETHTOOL
static const NMMetaPropertyInfo *const property_infos_ETHTOOL[] = {
	PROPERTY_INFO_ETHTOOL (FEATURE_ESP_HW_OFFLOAD),
	PROPERTY_INFO_ETHTOOL (FEATURE_ESP_TX_CSUM_HW_OFFLOAD),
	PROPERTY_INFO_ETHTOOL (FEATURE_FCOE_MTU),
	PROPERTY_INFO_ETHTOOL (FEATURE_GRO),
	PROPERTY_INFO_ETHTOOL (FEATURE_GSO),
	PROPERTY_INFO_ETHTOOL (FEATURE_HIGHDMA),
	PROPERTY_INFO_ETHTOOL (FEATURE_HW_TC_OFFLOAD),
	PROPERTY_INFO_ETHTOOL (FEATURE_L2_FWD_OFFLOAD),
	PROPERTY_INFO_ETHTOOL (FEATURE_LOOPBACK),
	PROPERTY_INFO_ETHTOOL (FEATURE_LRO),
	PROPERTY_INFO_ETHTOOL (FEATURE_NTUPLE),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX),
	PROPERTY_INFO_ETHTOOL (FEATURE_RXHASH),
	PROPERTY_INFO_ETHTOOL (FEATURE_RXVLAN),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_ALL),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_FCS),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_GRO_HW),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_UDP_TUNNEL_PORT_OFFLOAD),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_VLAN_FILTER),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_VLAN_STAG_FILTER),
	PROPERTY_INFO_ETHTOOL (FEATURE_RX_VLAN_STAG_HW_PARSE),
	PROPERTY_INFO_ETHTOOL (FEATURE_SG),
	PROPERTY_INFO_ETHTOOL (FEATURE_TLS_HW_RECORD),
	PROPERTY_INFO_ETHTOOL (FEATURE_TLS_HW_TX_OFFLOAD),
	PROPERTY_INFO_ETHTOOL (FEATURE_TSO),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX),
	PROPERTY_INFO_ETHTOOL (FEATURE_TXVLAN),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_CHECKSUM_FCOE_CRC),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_CHECKSUM_IPV4),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_CHECKSUM_IPV6),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_CHECKSUM_IP_GENERIC),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_CHECKSUM_SCTP),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_ESP_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_FCOE_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_GRE_CSUM_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_GRE_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_GSO_PARTIAL),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_GSO_ROBUST),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_IPXIP4_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_IPXIP6_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_NOCACHE_COPY),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_SCATTER_GATHER),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_SCATTER_GATHER_FRAGLIST),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_SCTP_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_TCP6_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_TCP_ECN_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_TCP_MANGLEID_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_TCP_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_UDP_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_UDP_TNL_CSUM_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_UDP_TNL_SEGMENTATION),
	PROPERTY_INFO_ETHTOOL (FEATURE_TX_VLAN_STAG_HW_INSERT),
	NULL,
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_GSM
static const NMMetaPropertyInfo *const property_infos_GSM[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_NUMBER,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_USERNAME,
		.is_cli_option =                TRUE,
		.property_alias =               "user",
		.prompt =                       N_("Username [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_PASSWORD,
		.is_cli_option =                TRUE,
		.property_alias =               "password",
		.prompt =                       N_("Password [none]"),
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_APN,
		.is_cli_option =                TRUE,
		.property_alias =               "apn",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("APN"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_NETWORK_ID,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_PIN,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_PIN_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_HOME_ONLY,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_DEVICE_ID,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_SIM_ID,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_SIM_OPERATOR_ID,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_gsm_sim_operator_id,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_GSM_MTU,
		.property_type =                &_pt_gobject_mtu,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mtu,
			.get_fcn =                  MTU_GET_FCN (NMSettingGsm, nm_setting_gsm_get_mtu),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_INFINIBAND
static const NMMetaPropertyInfo *const property_infos_INFINIBAND[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_INFINIBAND_MAC_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "mac",
		.prompt =                       N_("MAC [none]"),
		.property_type =                &_pt_gobject_mac,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mac,
			.mode =                     NM_META_PROPERTY_TYPE_MAC_MODE_INFINIBAND,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_INFINIBAND_MTU,
		.is_cli_option =                TRUE,
		.property_alias =               "mtu",
		.prompt =                       N_("MTU [auto]"),
		.property_type =                &_pt_gobject_mtu,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mtu,
			.get_fcn =                  MTU_GET_FCN (NMSettingInfiniband, nm_setting_infiniband_get_mtu),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_INFINIBAND_TRANSPORT_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "transport-mode",
		.prompt =                       NM_META_TEXT_PROMPT_IB_MODE,
		.def_hint =                     NM_META_TEXT_PROMPT_IB_MODE_CHOICES,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("datagram", "connected"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_INFINIBAND_P_KEY,
		.is_cli_option =                TRUE,
		.property_alias =               "p-key",
		.prompt =                       N_("P_KEY [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_infiniband_p_key,
			.set_fcn =                  _set_fcn_infiniband_p_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_INFINIBAND_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "parent",
		.prompt =                       N_("Parent interface [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_gobject_ifname,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_IP4_CONFIG
static const NMMetaPropertyInfo *const property_infos_IP4_CONFIG[] = {
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_METHOD, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_METHOD,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip_config_method,
		),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_IP4_CONFIG_METHOD_AUTO,
			                                          NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL,
			                                          NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
			                                          NM_SETTING_IP4_CONFIG_METHOD_SHARED,
			                                          NM_SETTING_IP4_CONFIG_METHOD_DISABLED),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DNS,
		.describe_message =
		    N_("Enter a list of IPv4 addresses of DNS servers.\n\n"
		       "Example: 8.8.8.8, 8.8.4.4\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip_config_dns,
			.remove_fcn =               _remove_fcn_ip_config_dns,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS_SEARCH, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DNS_SEARCH,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip_config_dns_search,
			.remove_fcn =               _remove_fcn_ip_config_dns_search,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS_OPTIONS, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DNS_OPTIONS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_nmc_with_default,
			.set_fcn =                  _set_fcn_ip_config_dns_options,
			.remove_fcn =               _remove_fcn_ip_config_dns_options,
		),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (get_with_default,
			.fcn =                      GET_FCN_WITH_DEFAULT (NMSettingIPConfig, _dns_options_is_default),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS_PRIORITY, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DNS_PRIORITY,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ADDRESSES, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_ADDRESSES,
		.is_cli_option =                TRUE,
		.property_alias =               "ip4",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_MULTI,
		.prompt =                       N_("IPv4 address (IP[/plen]) [none]"),
		.describe_message =
		    N_("Enter a list of IPv4 addresses formatted as:\n"
		       "  ip[/prefix], ip[/prefix],...\n"
		       "Missing prefix is regarded as prefix of 32.\n\n"
		       "Example: 192.168.1.5/24, 10.0.0.11/24\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_ip_config_addresses,
			.set_fcn =                  _set_fcn_ip4_config_addresses,
			.remove_fcn =               _remove_fcn_ip_config_addresses,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_GATEWAY, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_GATEWAY,
		.is_cli_option =                TRUE,
		.property_alias =               "gw4",
		.prompt =                       N_("IPv4 gateway [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip4_config_gateway,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ROUTES, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_ROUTES,
		.describe_message =
		    N_("Enter a list of IPv4 routes formatted as:\n"
		       "  ip[/prefix] [next-hop] [metric],...\n\n"
		       "Missing prefix is regarded as a prefix of 32.\n"
		       "Missing next-hop is regarded as 0.0.0.0.\n"
		       "Missing metric means default (NM/kernel will set a default value).\n\n"
		       "Examples: 192.168.2.0/24 192.168.2.1 3, 10.1.0.0/16 10.0.0.254\n"
		       "          10.1.2.0/24\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_ip_config_routes,
			.set_fcn =                  _set_fcn_ip4_config_routes,
			.remove_fcn =               _remove_fcn_ipv4_config_routes,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ROUTE_METRIC, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_ROUTE_METRIC,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ROUTE_TABLE, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_ROUTE_TABLE,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "unspec",
				},
				{
					.value.i64 = 254,
					.nick = "main",
				},
			),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_IGNORE_AUTO_ROUTES, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_IGNORE_AUTO_ROUTES,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_IGNORE_AUTO_DNS,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP4_CONFIG_DHCP_CLIENT_ID,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DHCP_TIMEOUT, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DHCP_TIMEOUT,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "default",
				},
				{
					.value.i64 = G_MAXINT32,
					.nick = "infinity",
				},
			),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DHCP_SEND_HOSTNAME, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DHCP_SEND_HOSTNAME,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DHCP_HOSTNAME, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DHCP_HOSTNAME,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP4_CONFIG_DHCP_FQDN,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_NEVER_DEFAULT, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_NEVER_DEFAULT,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_MAY_FAIL, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_MAY_FAIL,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DAD_TIMEOUT, DESCRIBE_DOC_NM_SETTING_IP4_CONFIG_DAD_TIMEOUT,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = -1,
					.nick = "default",
				},
				{
					.value.i64 = 0,
					.nick = "off",
				},
			),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_IP6_CONFIG
static const NMMetaPropertyInfo *const property_infos_IP6_CONFIG[] = {
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_METHOD, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_METHOD,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip_config_method,
		),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_IP6_CONFIG_METHOD_IGNORE,
			                                          NM_SETTING_IP6_CONFIG_METHOD_AUTO,
			                                          NM_SETTING_IP6_CONFIG_METHOD_DHCP,
			                                          NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL,
			                                          NM_SETTING_IP6_CONFIG_METHOD_MANUAL,
			                                          NM_SETTING_IP6_CONFIG_METHOD_SHARED),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DNS,
		.describe_message =
		    N_("Enter a list of IPv6 addresses of DNS servers.  If the IPv6 "
		       "configuration method is 'auto' these DNS servers are appended "
		       "to those (if any) returned by automatic configuration.  DNS "
		       "servers cannot be used with the 'shared' or 'link-local' IPv6 "
		       "configuration methods, as there is no upstream network. In "
		       "all other IPv6 configuration methods, these DNS "
		       "servers are used as the only DNS servers for this connection.\n\n"
		       "Example: 2607:f0d0:1002:51::4, 2607:f0d0:1002:51::1\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip_config_dns,
			.remove_fcn =               _remove_fcn_ip_config_dns,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS_SEARCH, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DNS_SEARCH,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip_config_dns_search,
			.remove_fcn =               _remove_fcn_ip_config_dns_search,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS_OPTIONS, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DNS_OPTIONS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_nmc_with_default,
			.set_fcn =                  _set_fcn_ip_config_dns_options,
			.remove_fcn =               _remove_fcn_ip_config_dns_options,
		),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (get_with_default,
			.fcn =                      GET_FCN_WITH_DEFAULT (NMSettingIPConfig, _dns_options_is_default),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DNS_PRIORITY, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DNS_PRIORITY,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ADDRESSES, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_ADDRESSES,
		.is_cli_option =                TRUE,
		.property_alias =               "ip6",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_MULTI,
		.prompt =                       N_("IPv6 address (IP[/plen]) [none]"),
		.describe_message =
		    N_("Enter a list of IPv6 addresses formatted as:\n"
		       "  ip[/prefix], ip[/prefix],...\n"
		       "Missing prefix is regarded as prefix of 128.\n\n"
		       "Example: 2607:f0d0:1002:51::4/64, 1050:0:0:0:5:600:300c:326b\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_ip_config_addresses,
			.set_fcn =                  _set_fcn_ip6_config_addresses,
			.remove_fcn =               _remove_fcn_ip_config_addresses,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_GATEWAY, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_GATEWAY,
		.is_cli_option =                TRUE,
		.property_alias =               "gw6",
		.prompt =                       N_("IPv6 gateway [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_ip6_config_gateway,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ROUTES, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_ROUTES,
		.describe_message =
		    N_("Enter a list of IPv6 routes formatted as:\n"
		       "  ip[/prefix] [next-hop] [metric],...\n\n"
		       "Missing prefix is regarded as a prefix of 128.\n"
		       "Missing next-hop is regarded as \"::\".\n"
		       "Missing metric means default (NM/kernel will set a default value).\n\n"
		       "Examples: 2001:db8:beef:2::/64 2001:db8:beef::2, 2001:db8:beef:3::/64 2001:db8:beef::3 2\n"
		       "          abbe::/64 55\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_ip_config_routes,
			.set_fcn =                  _set_fcn_ip6_config_routes,
			.remove_fcn =               _remove_fcn_ipv6_config_routes,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ROUTE_METRIC, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_ROUTE_METRIC,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_ROUTE_TABLE, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_ROUTE_TABLE,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "unspec",
				},
				{
					.value.i64 = 254,
					.nick = "main",
				},
			),
		),
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_IGNORE_AUTO_ROUTES, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_IGNORE_AUTO_ROUTES,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_IGNORE_AUTO_DNS,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_NEVER_DEFAULT, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_NEVER_DEFAULT,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_MAY_FAIL, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_MAY_FAIL,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP6_CONFIG_IP6_PRIVACY,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
			.value_infos_get =          ENUM_VALUE_INFOS (
				{
					.value = NM_SETTING_IP6_CONFIG_PRIVACY_PREFER_PUBLIC_ADDR,
					.nick = "enabled, prefer public IP",
				},
				{
					.value = NM_SETTING_IP6_CONFIG_PRIVACY_PREFER_TEMP_ADDR,
					.nick = "enabled, prefer temporary IP",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_ip6_config_addr_gen_mode_get_type,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO (NM_SETTING_IP6_CONFIG_DHCP_DUID, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DHCP_DUID,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DHCP_SEND_HOSTNAME, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DHCP_SEND_HOSTNAME,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO (NM_SETTING_IP_CONFIG_DHCP_HOSTNAME, DESCRIBE_DOC_NM_SETTING_IP6_CONFIG_DHCP_HOSTNAME,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP6_CONFIG_TOKEN,
		.property_type =                &_pt_gobject_string,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_IP_TUNNEL
static const NMMetaPropertyInfo *const property_infos_IP_TUNNEL[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "mode",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       NM_META_TEXT_PROMPT_IP_TUNNEL_MODE,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_ip_tunnel_mode_get_type,
				.min =                  NM_IP_TUNNEL_MODE_UNKNOWN + 1,
				.max =                  G_MAXINT,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "dev",
		.prompt =                       N_("Parent device [none]"),
		.property_type =                &_pt_gobject_devices,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_LOCAL,
		.is_cli_option =                TRUE,
		.property_alias =               "local",
		.prompt =                       N_("Local endpoint [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_REMOTE,
		.is_cli_option =                TRUE,
		.property_alias =               "remote",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("Remote"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_TTL,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_TOS,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_PATH_MTU_DISCOVERY,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_INPUT_KEY,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_OUTPUT_KEY,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_ENCAPSULATION_LIMIT,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_FLOW_LABEL,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_MTU,
		.property_type =                &_pt_gobject_mtu,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_IP_TUNNEL_FLAGS,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_ip_tunnel_flags_get_type,
			),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_MACSEC
static const NMMetaPropertyInfo *const property_infos_MACSEC[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "dev",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("MACsec parent device or connection UUID"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "mode",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       NM_META_TEXT_PROMPT_MACSEC_MODE,
		.def_hint =                     NM_META_TEXT_PROMPT_MACSEC_MODE_CHOICES,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_macsec_mode_get_type,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_ENCRYPT,
		.is_cli_option =                TRUE,
		.property_alias =               "encrypt",
		.prompt =                       N_("Enable encryption [yes]"),
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_MKA_CAK,
		.is_cli_option =                TRUE,
		.property_alias =               "cak",
		.prompt =                       N_("MKA CAK"),
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_MKA_CAK_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_MKA_CKN,
		.is_cli_option =                TRUE,
		.property_alias =               "ckn",
		.prompt =                       N_("MKA_CKN"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_PORT,
		.is_cli_option =                TRUE,
		.property_alias =               "port",
		.prompt =                       N_("SCI port [1]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_VALIDATION,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_macsec_validation_get_type,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACSEC_SEND_SCI,
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_MACVLAN
static const NMMetaPropertyInfo *const property_infos_MACVLAN[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACVLAN_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "dev",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("MACVLAN parent device or connection UUID"),
		.property_type =                &_pt_gobject_devices,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACVLAN_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "mode",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       NM_META_TEXT_PROMPT_MACVLAN_MODE,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
			.get_gtype =                nm_setting_macvlan_mode_get_type,
			.min =                      NM_SETTING_MACVLAN_MODE_UNKNOWN + 1,
			.max =                      G_MAXINT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACVLAN_PROMISCUOUS,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MACVLAN_TAP,
		.is_cli_option =                TRUE,
		.property_alias =               "tap",
		.prompt =                       N_("Tap [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_MATCH
static const NMMetaPropertyInfo *const property_infos_MATCH[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_MATCH_INTERFACE_NAME,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_match_interface_name,
			.set_fcn =                  _set_fcn_match_interface_name,
			.remove_fcn =               _remove_fcn_match_interface_name,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_OLPC_MESH
static const NMMetaPropertyInfo *const property_infos_OLPC_MESH[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OLPC_MESH_SSID,
		.is_cli_option =                TRUE,
		.property_alias =               "ssid",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("SSID"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_olpc_mesh_ssid,
			.set_fcn =                  _set_fcn_gobject_ssid,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OLPC_MESH_CHANNEL,
		.is_cli_option =                TRUE,
		.property_alias =               "channel",
		.prompt =                       N_("OLPC Mesh channel [1]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_olpc_mesh_channel,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OLPC_MESH_DHCP_ANYCAST_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "dhcp-anycast",
		.prompt =                       N_("DHCP anycast MAC address [none]"),
		.property_type =                &_pt_gobject_mac,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_OVS_BRIDGE
static const NMMetaPropertyInfo *const property_infos_OVS_BRIDGE[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_BRIDGE_FAIL_MODE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("secure", "standalone"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_BRIDGE_MCAST_SNOOPING_ENABLE,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_BRIDGE_RSTP_ENABLE,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_BRIDGE_STP_ENABLE,
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_OVS_INTERFACE
static const NMMetaPropertyInfo *const property_infos_OVS_INTERFACE[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_INTERFACE_TYPE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("internal", "patch"),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_OVS_PATCH
static const NMMetaPropertyInfo *const property_infos_OVS_PATCH[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PATCH_PEER,
		.property_type =                &_pt_gobject_string,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_OVS_PORT
static const NMMetaPropertyInfo *const property_infos_OVS_PORT[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PORT_VLAN_MODE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("access", "native-tagged", "native-untagged", "trunk"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PORT_TAG,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PORT_LACP,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("active", "off", "passive"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PORT_BOND_MODE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("active-backup", "balance-slb", "balance-tcp"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PORT_BOND_UPDELAY,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_OVS_PORT_BOND_DOWNDELAY,
		.property_type =                &_pt_gobject_int,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_PPP
static const NMMetaPropertyInfo *const property_infos_PPP[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_NOAUTH,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REFUSE_EAP,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REFUSE_PAP,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REFUSE_CHAP,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REFUSE_MSCHAP,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REFUSE_MSCHAPV2,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_NOBSDCOMP,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_NODEFLATE,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_NO_VJ_COMP,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REQUIRE_MPPE,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_REQUIRE_MPPE_128,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_MPPE_STATEFUL,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_CRTSCTS,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_BAUD,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_MRU,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_MTU,
		.property_type =                &_pt_gobject_mtu,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mtu,
			.get_fcn =                  MTU_GET_FCN (NMSettingPpp, nm_setting_ppp_get_mtu),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_LCP_ECHO_FAILURE,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPP_LCP_ECHO_INTERVAL,
		.property_type =                &_pt_gobject_int,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_PPPOE
static const NMMetaPropertyInfo *const property_infos_PPPOE[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPPOE_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "parent",
		.prompt =                       N_("PPPoE parent device"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPPOE_SERVICE,
		.is_cli_option =                TRUE,
		.property_alias =               "service",
		.prompt =                       N_("Service [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPPOE_USERNAME,
		.is_cli_option =                TRUE,
		.property_alias =               "username",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("PPPoE username"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPPOE_PASSWORD,
		.is_cli_option =                TRUE,
		.property_alias =               "password",
		.prompt =                       N_("Password [none]"),
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PPPOE_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_PROXY
static const NMMetaPropertyInfo *const property_infos_PROXY[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PROXY_METHOD,
		.is_cli_option =                TRUE,
		.property_alias =               "method",
		.prompt =                       NM_META_TEXT_PROMPT_PROXY_METHOD,
		.def_hint =                     NM_META_TEXT_PROMPT_PROXY_METHOD_CHOICES,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_proxy_method_get_type,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PROXY_BROWSER_ONLY,
		.is_cli_option =                TRUE,
		.property_alias =               "browser-only",
		.prompt =                       N_("Browser only [no]"),
		.property_type =                &_pt_gobject_bool
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PROXY_PAC_URL,
		.is_cli_option =                TRUE,
		.property_alias =               "pac-url",
		.prompt =                       N_("PAC URL"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_PROXY_PAC_SCRIPT,
		.is_cli_option =                TRUE,
		.property_alias =               "pac-script",
		.prompt =                       N_("PAC script"),
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_string,
			.validate_fcn =             _validate_fcn_proxy_pac_script,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_SERIAL
static const NMMetaPropertyInfo *const property_infos_SERIAL[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SERIAL_BAUD,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SERIAL_BITS,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SERIAL_PARITY,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.value_infos =          ENUM_VALUE_INFOS (
					{
						.value = NM_SETTING_SERIAL_PARITY_EVEN,
						.nick = "E",
					},
					{
						.value = NM_SETTING_SERIAL_PARITY_EVEN,
						.nick = "e",
					},
					{
						.value = NM_SETTING_SERIAL_PARITY_ODD,
						.nick = "O",
					},
					{
						.value = NM_SETTING_SERIAL_PARITY_ODD,
						.nick = "o",
					},
					{
						.value = NM_SETTING_SERIAL_PARITY_NONE,
						.nick = "N",
					},
					{
						.value = NM_SETTING_SERIAL_PARITY_NONE,
						.nick = "n",
					},
				),
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SERIAL_STOPBITS,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SERIAL_SEND_DELAY,
		.property_type =                &_pt_gobject_int,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_SRIOV
static const NMMetaPropertyInfo *const property_infos_SRIOV[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SRIOV_TOTAL_VFS,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SRIOV_VFS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_sriov_vfs,
			.set_fcn =                  _set_fcn_sriov_vfs,
			.remove_fcn =               _remove_fcn_sriov_vfs,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_SRIOV_AUTOPROBE_DRIVERS,
		.property_type =                &_pt_gobject_enum,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_TC_CONFIG
static const NMMetaPropertyInfo *const property_infos_TC_CONFIG[] = {
	PROPERTY_INFO (NM_SETTING_TC_CONFIG_QDISCS, DESCRIBE_DOC_NM_SETTING_TC_CONFIG_QDISCS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_tc_config_qdiscs,
			.set_fcn =                  _set_fcn_tc_config_qdiscs,
			.remove_fcn =               _remove_fcn_tc_config_qdiscs,
		),
	),
	PROPERTY_INFO (NM_SETTING_TC_CONFIG_TFILTERS, DESCRIBE_DOC_NM_SETTING_TC_CONFIG_TFILTERS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_tc_config_tfilters,
			.set_fcn =                  _set_fcn_tc_config_tfilters,
			.remove_fcn =               _remove_fcn_tc_config_tfilters,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_TEAM
static const NMMetaPropertyInfo *const property_infos_TEAM[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_CONFIG,
		.is_cli_option =                TRUE,
		.property_alias =               "config",
		.prompt =                       N_("Team JSON configuration [none]"),
		.describe_message =             TEAM_DESCRIBE_MESSAGE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_string,
			.validate_fcn =             _validate_fcn_team_config,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_NOTIFY_PEERS_COUNT,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "disabled",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_NOTIFY_PEERS_INTERVAL,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_MCAST_REJOIN_COUNT,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "disabled",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_MCAST_REJOIN_INTERVAL,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_TEAM_RUNNER_BROADCAST,
			                                          NM_SETTING_TEAM_RUNNER_ROUNDROBIN,
			                                          NM_SETTING_TEAM_RUNNER_RANDOM,
			                                          NM_SETTING_TEAM_RUNNER_ACTIVEBACKUP,
			                                          NM_SETTING_TEAM_RUNNER_LOADBALANCE,
			                                          NM_SETTING_TEAM_RUNNER_LACP),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_HWADDR_POLICY,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_TEAM_RUNNER_HWADDR_POLICY_SAME_ALL,
			                                          NM_SETTING_TEAM_RUNNER_HWADDR_POLICY_BY_ACTIVE,
			                                          NM_SETTING_TEAM_RUNNER_HWADDR_POLICY_ONLY_ACTIVE),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_TX_HASH,
		.property_type =  DEFINE_PROPERTY_TYPE (
			.get_fcn =                   _get_fcn_gobject,
			.set_fcn =                   _set_fcn_team_runner_tx_hash,
			.remove_fcn =                _remove_fcn_team_runner_tx_hash,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_TX_BALANCER,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("basic"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_TX_BALANCER_INTERVAL,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = NM_SETTING_TEAM_RUNNER_TX_BALANCER_INTERVAL_DEFAULT,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_ACTIVE,
		.property_type =                & _pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_FAST_RATE,
		.property_type =                & _pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_SYS_PRIO,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = NM_SETTING_TEAM_RUNNER_SYS_PRIO_DEFAULT,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_MIN_PORTS,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY_LACP_PRIO,
			                                          NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY_LACP_PRIO_STABLE,
			                                          NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY_BANDWIDTH,
			                                          NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY_COUNT,
			                                          NM_SETTING_TEAM_RUNNER_AGG_SELECT_POLICY_PORT_CONFIG),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_LINK_WATCHERS,
		.describe_message =             TEAM_LINK_WATCHERS_DESCRIBE_MESSAGE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_team_link_watchers,
			.set_fcn =                  _set_fcn_team_link_watchers,
			.remove_fcn =               _remove_fcn_team_link_watchers,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_TEAM_PORT
static const NMMetaPropertyInfo *const property_infos_TEAM_PORT[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_CONFIG,
		.is_cli_option =                TRUE,
		.property_alias =               "config",
		.prompt =                       N_("Team JSON configuration [none]"),
		.describe_message =             TEAM_DESCRIBE_MESSAGE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_string,
			.validate_fcn =             _validate_fcn_team_config,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_QUEUE_ID,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = NM_SETTING_TEAM_PORT_QUEUE_ID_DEFAULT,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_PRIO,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_STICKY,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_LACP_PRIO,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = NM_SETTING_TEAM_PORT_LACP_PRIO_DEFAULT,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_LACP_KEY,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = 0,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TEAM_PORT_LINK_WATCHERS,
		.describe_message =             TEAM_LINK_WATCHERS_DESCRIBE_MESSAGE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_team_port_link_watchers,
			.set_fcn =                  _set_fcn_team_port_link_watchers,
			.remove_fcn =               _remove_fcn_team_port_link_watchers,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_TUN
static const NMMetaPropertyInfo *const property_infos_TUN[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TUN_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "mode",
		.prompt =                       NM_META_TEXT_PROMPT_TUN_MODE,
		.def_hint =                     NM_META_TEXT_PROMPT_TUN_MODE_CHOICES,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
			.get_gtype =                nm_setting_tun_mode_get_type,
			.min =                      NM_SETTING_TUN_MODE_UNKNOWN + 1,
			.max =                      G_MAXINT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TUN_OWNER,
		.is_cli_option =                TRUE,
		.property_alias =               "owner",
		.prompt =                       N_("User ID [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TUN_GROUP,
		.is_cli_option =                TRUE,
		.property_alias =               "group",
		.prompt =                       N_("Group ID [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TUN_PI,
		.is_cli_option =                TRUE,
		.property_alias =               "pi",
		.prompt =                       N_("Enable PI [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TUN_VNET_HDR,
		.is_cli_option =                TRUE,
		.property_alias =               "vnet-hdr",
		.prompt =                       N_("Enable VNET header [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_TUN_MULTI_QUEUE,
		.is_cli_option =                TRUE,
		.property_alias =               "multi-queue",
		.prompt =                       N_("Enable multi queue [no]"),
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_VLAN
static const NMMetaPropertyInfo *const property_infos_VLAN[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VLAN_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "dev",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("VLAN parent device or connection UUID"),
		.property_type =                &_pt_gobject_devices,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VLAN_ID,
		.is_cli_option =                TRUE,
		.property_alias =               "id",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("VLAN ID (<0-4094>)"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VLAN_FLAGS,
		.is_cli_option =                TRUE,
		.property_alias =               "flags",
		.prompt =                       N_("VLAN flags (<0-7>) [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_vlan_flags,
			.set_fcn =                  _set_fcn_gobject_flags,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VLAN_INGRESS_PRIORITY_MAP,
		.is_cli_option =                TRUE,
		.property_alias =               "ingress",
		.prompt =                       N_("Ingress priority maps [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_vlan_xgress_priority_map,
			.set_fcn =                  _set_fcn_vlan_xgress_priority_map,
			.remove_fcn =               _remove_fcn_vlan_xgress_priority_map,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VLAN_EGRESS_PRIORITY_MAP,
		.is_cli_option =                TRUE,
		.property_alias =               "egress",
		.prompt =                       N_("Egress priority maps [none]"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_vlan_xgress_priority_map,
			.set_fcn =                  _set_fcn_vlan_xgress_priority_map,
			.remove_fcn =               _remove_fcn_vlan_xgress_priority_map,
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_VPN
static const NMMetaPropertyInfo *const property_infos_VPN[] = {
[_NM_META_PROPERTY_TYPE_VPN_SERVICE_TYPE] =
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VPN_SERVICE_TYPE,
		.is_cli_option =                TRUE,
		.property_alias =               "vpn-type",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       NM_META_TEXT_PROMPT_VPN_TYPE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_vpn_service_type,
			.complete_fcn =             _complete_fcn_vpn_service_type,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VPN_USER_NAME,
		.is_cli_option =                TRUE,
		.property_alias =               "user",
		.prompt =                       N_("Username [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VPN_DATA,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_vpn_data,
			.set_fcn =                  _set_fcn_vpn_data,
			.remove_fcn =               _remove_fcn_vpn_data,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VPN_SECRETS,
		.is_secret =                    TRUE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_vpn_secrets,
			.set_fcn =                  _set_fcn_vpn_secrets,
			.remove_fcn =               _remove_fcn_vpn_secrets,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VPN_PERSISTENT,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VPN_TIMEOUT,
		.property_type =                &_pt_gobject_int,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_VXLAN
static const NMMetaPropertyInfo *const property_infos_VXLAN[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_PARENT,
		.is_cli_option =                TRUE,
		.property_alias =               "dev",
		.prompt =                       N_("Parent device [none]"),
		.property_type =                &_pt_gobject_devices,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_ID,
		.is_cli_option =                TRUE,
		.property_alias =               "id",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("VXLAN ID"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_LOCAL,
		.is_cli_option =                TRUE,
		.property_alias =               "local",
		.prompt =                       N_("Local address [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_REMOTE,
		.is_cli_option =                TRUE,
		.property_alias =               "remote",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("Remote"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_SOURCE_PORT_MIN,
		.is_cli_option =                TRUE,
		.property_alias =               "source-port-min",
		.prompt =                       N_("Minimum source port [0]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_SOURCE_PORT_MAX,
		.is_cli_option =                TRUE,
		.property_alias =               "source-port-max",
		.prompt =                       N_("Maximum source port [0]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_DESTINATION_PORT,
		.is_cli_option =                TRUE,
		.property_alias =               "destination-port",
		.prompt =                       N_("Destination port [8472]"),
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_TOS,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_TTL,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_AGEING,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_LIMIT,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_LEARNING,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_PROXY,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_RSC,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_L2_MISS,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_VXLAN_L3_MISS,
		.property_type =                &_pt_gobject_bool,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WIFI_P2P
static const NMMetaPropertyInfo *const property_infos_WIFI_P2P[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIFI_P2P_PEER,
		.is_cli_option =                TRUE,
		.property_alias =               "peer",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("Peer"),
		.property_type =                &_pt_gobject_mac,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIFI_P2P_WPS_METHOD,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wireless_security_wps_method_get_type,
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIFI_P2P_WFD_IES,
		.property_type =                &_pt_gobject_bytes,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WIMAX
static const NMMetaPropertyInfo *const property_infos_WIMAX[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIMAX_MAC_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "mac",
		.prompt =                       N_("MAC [none]"),
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIMAX_NETWORK_NAME,
		.is_cli_option =                TRUE,
		.property_alias =               "nsp",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("WiMAX NSP name"),
		.property_type =                &_pt_gobject_mac,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WIRED
static const NMMetaPropertyInfo *const property_infos_WIRED[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_PORT,
		/* Do not allow setting 'port' for now. It is not implemented in
		 * NM core, nor in ifcfg-rh plugin. Enable this when it gets done.
		 * wired_valid_ports[] = { "tp", "aui", "bnc", "mii", NULL };
		 */
		.property_type =                &_pt_gobject_readonly,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_SPEED,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_DUPLEX,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("half", "full"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_AUTO_NEGOTIATE,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_MAC_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "mac",
		.prompt =                       N_("MAC [none]"),
		.property_type =                &_pt_gobject_mac,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_CLONED_MAC_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "cloned-mac",
		.prompt =                       N_("Cloned MAC [none]"),
		.property_type =                &_pt_gobject_mac,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mac,
			.mode =                     NM_META_PROPERTY_TYPE_MAC_MODE_CLONED,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_GENERATE_MAC_ADDRESS_MASK,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_MAC_ADDRESS_BLACKLIST,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_wired_mac_address_blacklist,
			.remove_fcn =               _remove_fcn_wired_mac_address_blacklist,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_MTU,
		.is_cli_option =                TRUE,
		.property_alias =               "mtu",
		.prompt =                       N_("MTU [auto]"),
		.property_type =                &_pt_gobject_mtu,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mtu,
			.get_fcn =                  MTU_GET_FCN (NMSettingWired, nm_setting_wired_get_mtu),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_S390_SUBCHANNELS,
		.describe_message =
		    N_("Enter a list of subchannels (comma or space separated).\n\n"
		       "Example: 0.0.0e20 0.0.0e21 0.0.0e22\n"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_wired_s390_subchannels,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_S390_NETTYPE,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("qeth", "lcs", "ctc"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_S390_OPTIONS,
		.property_type = DEFINE_PROPERTY_TYPE (
			.describe_fcn =             _describe_fcn_wired_s390_options,
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_wired_s390_options,
			.remove_fcn =               _remove_fcn_wired_s390_options,
			.values_fcn =               _values_fcn__wired_s390_options,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_WAKE_ON_LAN,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wired_wake_on_lan_get_type,
				.value_infos =          ENUM_VALUE_INFOS (
					{
						.value = NM_SETTING_WIRED_WAKE_ON_LAN_NONE,
						.nick = "none",
					},
					{
						.value = NM_SETTING_WIRED_WAKE_ON_LAN_NONE,
						.nick = "disable",
					},
					{
						.value = NM_SETTING_WIRED_WAKE_ON_LAN_NONE,
						.nick = "disabled",
					},
				),
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRED_WAKE_ON_LAN_PASSWORD,
		.property_type =                &_pt_gobject_mac,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WIREGUARD
static const NMMetaPropertyInfo *const property_infos_WIREGUARD[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIREGUARD_PRIVATE_KEY,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIREGUARD_PRIVATE_KEY_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIREGUARD_LISTEN_PORT,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIREGUARD_FWMARK,
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int, \
			.base =                     16,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIREGUARD_PEER_ROUTES,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIREGUARD_MTU,
		.property_type =                &_pt_gobject_mtu,
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WIRELESS
static const NMMetaPropertyInfo *const property_infos_WIRELESS[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SSID,
		.is_cli_option =                TRUE,
		.property_alias =               "ssid",
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.prompt =                       N_("SSID"),
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_wireless_ssid,
			.set_fcn =                  _set_fcn_gobject_ssid,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_MODE,
		.is_cli_option =                TRUE,
		.property_alias =               "mode",
		.prompt =                       NM_META_TEXT_PROMPT_WIFI_MODE,
		.def_hint =                     NM_META_TEXT_PROMPT_WIFI_MODE_CHOICES,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV (NM_SETTING_WIRELESS_MODE_INFRA,
			                                          NM_SETTING_WIRELESS_MODE_ADHOC,
			                                          NM_SETTING_WIRELESS_MODE_AP),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_BAND,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("a", "bg"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_CHANNEL,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_wireless_channel,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_BSSID,
		.property_type =                &_pt_gobject_mac,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_RATE,
		/* Do not allow setting 'rate'. It is not implemented in NM core. */
		.property_type =                &_pt_gobject_readonly,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_TX_POWER,
		/* Do not allow setting 'tx-power'. It is not implemented in NM core. */
		.property_type =                &_pt_gobject_readonly,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_MAC_ADDRESS,
		.property_type =                &_pt_gobject_mac,
		.is_cli_option =                TRUE,
		.property_alias =               "mac",
		.prompt =                       N_("MAC [none]"),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_CLONED_MAC_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "cloned-mac",
		.prompt =                       N_("Cloned MAC [none]"),
		.property_type =                &_pt_gobject_mac,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mac,
			.mode =                     NM_META_PROPERTY_TYPE_MAC_MODE_CLONED,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_GENERATE_MAC_ADDRESS_MASK,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_MAC_ADDRESS_BLACKLIST,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_gobject,
			.set_fcn =                  _set_fcn_wireless_mac_address_blacklist,
			.remove_fcn =               _remove_fcn_wireless_mac_address_blacklist,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_MAC_ADDRESS_RANDOMIZATION,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_mac_randomization_get_type,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_MTU,
		.is_cli_option =                TRUE,
		.property_alias =               "mtu",
		.prompt =                       N_("MTU [auto]"),
		.property_type =                &_pt_gobject_mtu,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mtu,
			.get_fcn =                  MTU_GET_FCN (NMSettingWireless, nm_setting_wireless_get_mtu),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SEEN_BSSIDS,
		.property_type =                &_pt_gobject_readonly,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_HIDDEN,
		.property_type =                &_pt_gobject_bool,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_POWERSAVE,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wireless_powersave_get_type,
			),
			.typ_flags =                NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_WAKE_ON_WLAN,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wireless_wake_on_wlan_get_type,
				.value_infos =          ENUM_VALUE_INFOS (
					{
						.value = NM_SETTING_WIRELESS_WAKE_ON_WLAN_NONE,
						.nick = "disabled",
					},
				),
			),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WIRELESS_SECURITY
static const NMMetaPropertyInfo *const property_infos_WIRELESS_SECURITY[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("none", "ieee8021x", "wpa-none", "wpa-psk", "wpa-eap", "sae"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_TX_KEYIDX,
		.property_type =                &_pt_gobject_int,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_AUTH_ALG,
		.property_type =                &_pt_gobject_string,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			.values_static =            NM_MAKE_STRV ("open", "shared", "leap"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_PROTO,
		.property_type =                &_pt_multilist,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (multilist,
				.get_num_fcn =          MULTILIST_GET_NUM_FCN         (NMSettingWirelessSecurity, nm_setting_wireless_security_get_num_protos),
				.add_fcn =              MULTILIST_ADD_FCN             (NMSettingWirelessSecurity, nm_setting_wireless_security_add_proto),
				.remove_by_idx_fcn =    MULTILIST_REMOVE_BY_IDX_FCN   (NMSettingWirelessSecurity, nm_setting_wireless_security_remove_proto),
				.remove_by_value_fcn =  MULTILIST_REMOVE_BY_VALUE_FCN (NMSettingWirelessSecurity, nm_setting_wireless_security_remove_proto_by_value),
			),
			.values_static =            NM_MAKE_STRV ("wpa", "rsn"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_PAIRWISE,
		.property_type =                &_pt_multilist,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (multilist,
				.get_num_fcn =          MULTILIST_GET_NUM_FCN         (NMSettingWirelessSecurity, nm_setting_wireless_security_get_num_pairwise),
				.add_fcn =              MULTILIST_ADD_FCN             (NMSettingWirelessSecurity, nm_setting_wireless_security_add_pairwise),
				.remove_by_idx_fcn =    MULTILIST_REMOVE_BY_IDX_FCN   (NMSettingWirelessSecurity, nm_setting_wireless_security_remove_pairwise),
				.remove_by_value_fcn =  MULTILIST_REMOVE_BY_VALUE_FCN (NMSettingWirelessSecurity, nm_setting_wireless_security_remove_pairwise_by_value),
			),
			.values_static =            NM_MAKE_STRV ("tkip", "ccmp"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_GROUP,
		.property_type =                &_pt_multilist,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (multilist,
				.get_num_fcn =          MULTILIST_GET_NUM_FCN         (NMSettingWirelessSecurity, nm_setting_wireless_security_get_num_groups),
				.add_fcn =              MULTILIST_ADD_FCN             (NMSettingWirelessSecurity, nm_setting_wireless_security_add_group),
				.remove_by_idx_fcn =    MULTILIST_REMOVE_BY_IDX_FCN   (NMSettingWirelessSecurity, nm_setting_wireless_security_remove_group),
				.remove_by_value_fcn =  MULTILIST_REMOVE_BY_VALUE_FCN (NMSettingWirelessSecurity, nm_setting_wireless_security_remove_group_by_value),
			),
			.values_static =            NM_MAKE_STRV ("wep40", "wep104", "tkip", "ccmp"),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_PMF,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wireless_security_pmf_get_type,
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_LEAP_USERNAME,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_KEY0,
		.is_secret =                    TRUE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_wireless_security_wep_key,
			.set_fcn =                  _set_fcn_wireless_wep_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_KEY1,
		.is_secret =                    TRUE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_wireless_security_wep_key,
			.set_fcn =                  _set_fcn_wireless_wep_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_KEY2,
		.is_secret =                    TRUE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_wireless_security_wep_key,
			.set_fcn =                  _set_fcn_wireless_wep_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_KEY3,
		.is_secret =                    TRUE,
		.property_type = DEFINE_PROPERTY_TYPE (
			.get_fcn =                  _get_fcn_wireless_security_wep_key,
			.set_fcn =                  _set_fcn_wireless_wep_key,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_KEY_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE,
		.describe_message =
		    N_("Enter the type of WEP keys. The accepted values are: "
		       "0 or unknown, 1 or key, and 2 or passphrase.\n"),
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.pre_set_notify =       _gobject_enum_pre_set_notify_fcn_wireless_security_wep_key_type,
			),
			.typ_flags =                  NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PARSABLE_TEXT
			                            | NM_META_PROPERTY_TYP_FLAG_ENUM_GET_PRETTY_TEXT,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_PSK,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_PSK_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_LEAP_PASSWORD,
		.is_secret =                    TRUE,
		.property_type =                &_pt_gobject_string,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_LEAP_PASSWORD_FLAGS,
		.property_type =                &_pt_gobject_secret_flags,
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_WPS_METHOD,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wireless_security_wps_method_get_type,
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WIRELESS_SECURITY_FILS,
		.property_type =                &_pt_gobject_enum,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA (
			PROPERTY_TYP_DATA_SUBTYPE (gobject_enum,
				.get_gtype =            nm_setting_wireless_security_fils_get_type,
			),
		),
	),
	NULL
};

#undef  _CURRENT_NM_META_SETTING_TYPE
#define _CURRENT_NM_META_SETTING_TYPE NM_META_SETTING_TYPE_WPAN
static const NMMetaPropertyInfo *const property_infos_WPAN[] = {
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WPAN_MAC_ADDRESS,
		.property_type =                &_pt_gobject_mac,
		.is_cli_option =                TRUE,
		.property_alias =               "mac",
		.prompt =                       N_("MAC [none]"),
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (mac,
			.mode =                     NM_META_PROPERTY_TYPE_MAC_MODE_WPAN,
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WPAN_SHORT_ADDRESS,
		.is_cli_option =                TRUE,
		.property_alias =               "short-addr",
		.prompt =                       N_("Short address (<0x0000-0xffff>)"),
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int, \
			.base =                     16,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = G_MAXUINT16,
					.nick = "unset",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WPAN_PAN_ID,
		.is_cli_option =                TRUE,
		.inf_flags =                    NM_META_PROPERTY_INF_FLAG_REQD,
		.property_alias =               "pan-id",
		.prompt =                       N_("PAN Identifier (<0x0000-0xffff>)"),
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int, \
			.base =                     16,
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = G_MAXUINT16,
					.nick = "unset",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WPAN_PAGE,
		.is_cli_option =                TRUE,
		.property_alias =               "page",
		.prompt =                       N_("Page (<default|0-31>)"),
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int, \
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = NM_SETTING_WPAN_PAGE_DEFAULT,
					.nick = "default",
				},
			),
		),
	),
	PROPERTY_INFO_WITH_DESC (NM_SETTING_WPAN_CHANNEL,
		.is_cli_option =                TRUE,
		.property_alias =               "channel",
		.prompt =                       N_("Channel (<default|0-26>)"),
		.property_type =                &_pt_gobject_int,
		.property_typ_data = DEFINE_PROPERTY_TYP_DATA_SUBTYPE (gobject_int, \
			.value_infos =              INT_VALUE_INFOS (
				{
					.value.i64 = NM_SETTING_WPAN_CHANNEL_DEFAULT,
					.nick = "default",
				},
			),
		),
	),
	NULL
};

/*****************************************************************************/

static void
_setting_init_fcn_adsl (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		/* Initialize a protocol */
		g_object_set (NM_SETTING_ADSL (setting),
		              NM_SETTING_ADSL_PROTOCOL, NM_SETTING_ADSL_PROTOCOL_PPPOE,
		              NULL);
	}
}

static void
_setting_init_fcn_cdma (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		/* Initialize 'number' so that 'cdma' is valid */
		g_object_set (NM_SETTING_CDMA (setting),
		              NM_SETTING_CDMA_NUMBER, "#777",
		              NULL);
	}
}

static void
_setting_init_fcn_gsm (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		/* Initialize 'apn' so that 'gsm' is valid */
		g_object_set (NM_SETTING_GSM (setting),
		              NM_SETTING_GSM_APN, "internet",
		              NULL);
	}
}

static void
_setting_init_fcn_infiniband (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		/* Initialize 'transport-mode' so that 'infiniband' is valid */
		g_object_set (NM_SETTING_INFINIBAND (setting),
		              NM_SETTING_INFINIBAND_TRANSPORT_MODE, "datagram",
		              NULL);
	}
}

static void
_setting_init_fcn_ip4_config (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		g_object_set (NM_SETTING_IP_CONFIG (setting),
		              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
		              NULL);
	}
}

static void
_setting_init_fcn_ip6_config (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		g_object_set (NM_SETTING_IP_CONFIG (setting),
		              NM_SETTING_IP_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
		              NULL);
	}
}

static void
_setting_init_fcn_olpc_mesh (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		g_object_set (NM_SETTING_OLPC_MESH (setting),
		              NM_SETTING_OLPC_MESH_CHANNEL, 1,
		              NULL);
	}
}

static void
_setting_init_fcn_proxy (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		g_object_set (NM_SETTING_PROXY (setting),
		              NM_SETTING_PROXY_METHOD, (int) NM_SETTING_PROXY_METHOD_NONE,
		              NULL);
	}
}

static void
_setting_init_fcn_tun (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		g_object_set (NM_SETTING_TUN (setting),
		              NM_SETTING_TUN_MODE, NM_SETTING_TUN_MODE_TUN,
		              NULL);
	}
}

static void
_setting_init_fcn_vlan (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		g_object_set (setting,
		              NM_SETTING_VLAN_ID, 1,
		              NULL);
	}
}

static void
_setting_init_fcn_wireless (ARGS_SETTING_INIT_FCN)
{
	if (init_type == NM_META_ACCESSOR_SETTING_INIT_TYPE_CLI) {
		/* For Wi-Fi set mode to "infrastructure". Even though mode == NULL
		 * is regarded as "infrastructure", explicit value makes no doubts.
		 */
		g_object_set (NM_SETTING_WIRELESS (setting),
		              NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_INFRA,
		              NULL);
	}
}

/*****************************************************************************/

#define SETTING_PRETTY_NAME_6LOWPAN             N_("6LOWPAN settings")
#define SETTING_PRETTY_NAME_802_1X              N_("802-1x settings")
#define SETTING_PRETTY_NAME_ADSL                N_("ADSL connection")
#define SETTING_PRETTY_NAME_BLUETOOTH           N_("bluetooth connection")
#define SETTING_PRETTY_NAME_BOND                N_("Bond device")
#define SETTING_PRETTY_NAME_BRIDGE              N_("Bridge device")
#define SETTING_PRETTY_NAME_BRIDGE_PORT         N_("Bridge port")
#define SETTING_PRETTY_NAME_CDMA                N_("CDMA mobile broadband connection")
#define SETTING_PRETTY_NAME_CONNECTION          N_("General settings")
#define SETTING_PRETTY_NAME_DCB                 N_("DCB settings")
#define SETTING_PRETTY_NAME_DUMMY               N_("Dummy settings")
#define SETTING_PRETTY_NAME_ETHTOOL             N_("Ethtool settings")
#define SETTING_PRETTY_NAME_GENERIC             N_("Generic settings")
#define SETTING_PRETTY_NAME_GSM                 N_("GSM mobile broadband connection")
#define SETTING_PRETTY_NAME_INFINIBAND          N_("InfiniBand connection")
#define SETTING_PRETTY_NAME_IP4_CONFIG          N_("IPv4 protocol")
#define SETTING_PRETTY_NAME_IP6_CONFIG          N_("IPv6 protocol")
#define SETTING_PRETTY_NAME_IP_TUNNEL           N_("IP-tunnel settings")
#define SETTING_PRETTY_NAME_MACSEC              N_("MACsec connection")
#define SETTING_PRETTY_NAME_MACVLAN             N_("macvlan connection")
#define SETTING_PRETTY_NAME_MATCH               N_("Match")
#define SETTING_PRETTY_NAME_OLPC_MESH           N_("OLPC Mesh connection")
#define SETTING_PRETTY_NAME_OVS_BRIDGE          N_("Open vSwitch bridge settings")
#define SETTING_PRETTY_NAME_OVS_INTERFACE       N_("Open vSwitch interface settings")
#define SETTING_PRETTY_NAME_OVS_PATCH           N_("Open vSwitch patch interface settings")
#define SETTING_PRETTY_NAME_OVS_PORT            N_("Open vSwitch port settings")
#define SETTING_PRETTY_NAME_PPP                 N_("PPP settings")
#define SETTING_PRETTY_NAME_PPPOE               N_("PPPoE")
#define SETTING_PRETTY_NAME_PROXY               N_("Proxy")
#define SETTING_PRETTY_NAME_SERIAL              N_("Serial settings")
#define SETTING_PRETTY_NAME_SRIOV               N_("SR-IOV settings")
#define SETTING_PRETTY_NAME_TC_CONFIG           N_("Traffic controls")
#define SETTING_PRETTY_NAME_TEAM                N_("Team device")
#define SETTING_PRETTY_NAME_TEAM_PORT           N_("Team port")
#define SETTING_PRETTY_NAME_TUN                 N_("Tun device")
#define SETTING_PRETTY_NAME_USER                N_("User settings")
#define SETTING_PRETTY_NAME_VLAN                N_("VLAN connection")
#define SETTING_PRETTY_NAME_VPN                 N_("VPN connection")
#define SETTING_PRETTY_NAME_VXLAN               N_("VXLAN connection")
#define SETTING_PRETTY_NAME_WIFI_P2P            N_("Wi-Fi P2P connection")
#define SETTING_PRETTY_NAME_WIMAX               N_("WiMAX connection")
#define SETTING_PRETTY_NAME_WIRED               N_("Wired Ethernet")
#define SETTING_PRETTY_NAME_WIREGUARD           N_("WireGuard VPN settings")
#define SETTING_PRETTY_NAME_WIRELESS            N_("Wi-Fi connection")
#define SETTING_PRETTY_NAME_WIRELESS_SECURITY   N_("Wi-Fi security settings")
#define SETTING_PRETTY_NAME_WPAN                N_("WPAN settings")

#define NM_META_SETTING_VALID_PARTS(...) \
	((const NMMetaSettingValidPartItem *const[]) { __VA_ARGS__  NULL })

#define NM_META_SETTING_VALID_PART_ITEM(type, mand) \
	(&((const NMMetaSettingValidPartItem) { \
		.setting_info =                     &nm_meta_setting_infos_editor[NM_META_SETTING_TYPE_##type], \
		.mandatory =                        mand, \
	}))

const NMMetaSettingInfoEditor nm_meta_setting_infos_editor[] = {
#define SETTING_INFO_EMPTY(type, ...) \
	[NM_META_SETTING_TYPE_##type] = { \
		.meta_type =                        &nm_meta_type_setting_info_editor, \
		.general =                          &nm_meta_setting_infos[NM_META_SETTING_TYPE_##type], \
		.pretty_name =                      SETTING_PRETTY_NAME_##type, \
		__VA_ARGS__ \
	}
#define SETTING_INFO(type, ...) \
	[NM_META_SETTING_TYPE_##type] = { \
		.meta_type =                        &nm_meta_type_setting_info_editor, \
		.general =                          &nm_meta_setting_infos[NM_META_SETTING_TYPE_##type], \
		.properties =                       property_infos_##type, \
		.properties_num =                   G_N_ELEMENTS (property_infos_##type) - 1, \
		.pretty_name =                      SETTING_PRETTY_NAME_##type, \
		__VA_ARGS__ \
	}
	SETTING_INFO (6LOWPAN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (6LOWPAN,               TRUE),
		),
	),
	SETTING_INFO (802_1X),
	SETTING_INFO (ADSL,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (ADSL,                  TRUE),
		),
		.setting_init_fcn =             _setting_init_fcn_adsl,
	),
	SETTING_INFO (BLUETOOTH,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (BLUETOOTH,             TRUE),
			NM_META_SETTING_VALID_PART_ITEM (BRIDGE,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (GSM,                   FALSE),
			NM_META_SETTING_VALID_PART_ITEM (CDMA,                  FALSE),
		),
	),
	SETTING_INFO (BOND,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (BOND,                  TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (BRIDGE,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (BRIDGE,                TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (BRIDGE_PORT),
	SETTING_INFO (CDMA,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (CDMA,                  TRUE),
			NM_META_SETTING_VALID_PART_ITEM (SERIAL,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (PPP,                   FALSE),
		),
		.setting_init_fcn =             _setting_init_fcn_cdma,
	),
	SETTING_INFO (CONNECTION),
	SETTING_INFO (DCB),
	SETTING_INFO (ETHTOOL),
	SETTING_INFO_EMPTY (DUMMY,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (DUMMY,                 TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO_EMPTY (GENERIC,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (GENERIC,               TRUE),
		),
	),
	SETTING_INFO (GSM,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (GSM,                   TRUE),
			NM_META_SETTING_VALID_PART_ITEM (SERIAL,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (PPP,                   FALSE),
		),
		.setting_init_fcn =             _setting_init_fcn_gsm,
	),
	SETTING_INFO (INFINIBAND,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (INFINIBAND,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (SRIOV,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
		.setting_init_fcn =             _setting_init_fcn_infiniband,
	),
	SETTING_INFO (IP4_CONFIG,
		.setting_init_fcn =             _setting_init_fcn_ip4_config,
	),
	SETTING_INFO (IP6_CONFIG,
		.setting_init_fcn =             _setting_init_fcn_ip6_config,
	),
	SETTING_INFO (IP_TUNNEL,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (IP_TUNNEL,             TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (MACSEC,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (MACSEC,                TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (802_1X,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (MACVLAN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (MACVLAN,               TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (MATCH),
	SETTING_INFO (OLPC_MESH,
		.alias =                            "olpc-mesh",
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (OLPC_MESH,             TRUE),
		),
		.setting_init_fcn =             _setting_init_fcn_olpc_mesh,
	),
	SETTING_INFO (OVS_BRIDGE,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (OVS_BRIDGE,            TRUE),
		),
	),
	SETTING_INFO (OVS_INTERFACE,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (OVS_INTERFACE,         TRUE),
			NM_META_SETTING_VALID_PART_ITEM (OVS_PATCH,             FALSE),
			NM_META_SETTING_VALID_PART_ITEM (IP4_CONFIG,            FALSE),
			NM_META_SETTING_VALID_PART_ITEM (IP6_CONFIG,            FALSE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (OVS_PATCH),
	SETTING_INFO (OVS_PORT,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (OVS_PORT,              TRUE),
		),
	),
	SETTING_INFO (PPPOE,
		/* PPPoE is a base connection type from historical reasons.
		 * See libnm-core/nm-setting.c:_nm_setting_is_base_type()
		 */
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (PPPOE,                 TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 TRUE),
			NM_META_SETTING_VALID_PART_ITEM (PPP,                   FALSE),
			NM_META_SETTING_VALID_PART_ITEM (802_1X,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (PPP),
	SETTING_INFO (PROXY,
		.setting_init_fcn =             _setting_init_fcn_proxy,
	),
	SETTING_INFO (SERIAL),
	SETTING_INFO (SRIOV),
	SETTING_INFO (TC_CONFIG),
	SETTING_INFO (TEAM,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (TEAM,                  TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (TEAM_PORT),
	SETTING_INFO (TUN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (TUN,                   TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
		.setting_init_fcn =             _setting_init_fcn_tun,
	),
	SETTING_INFO_EMPTY (USER),
	SETTING_INFO (VLAN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (VLAN,                  TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
		.setting_init_fcn =             _setting_init_fcn_vlan,
	),
	SETTING_INFO (VPN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (VPN,                   TRUE),
		),
	),
	SETTING_INFO (VXLAN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (VXLAN,                 TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (WIFI_P2P,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIFI_P2P,              TRUE),
		),
	),
	SETTING_INFO (WIMAX,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIMAX,                 TRUE),
		),
	),
	SETTING_INFO (WIRED,
		.alias =                            "ethernet",
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRED,                 TRUE),
			NM_META_SETTING_VALID_PART_ITEM (802_1X,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (DCB,                   FALSE),
			NM_META_SETTING_VALID_PART_ITEM (SRIOV,                 FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
	),
	SETTING_INFO (WIREGUARD,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIREGUARD,             TRUE),
		),
	),
	SETTING_INFO (WIRELESS,
		.alias =                            "wifi",
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRELESS,              TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WIRELESS_SECURITY,     FALSE),
			NM_META_SETTING_VALID_PART_ITEM (802_1X,                FALSE),
			NM_META_SETTING_VALID_PART_ITEM (ETHTOOL,               FALSE),
		),
		.setting_init_fcn =             _setting_init_fcn_wireless,
	),
	SETTING_INFO (WIRELESS_SECURITY,
		.alias =                            "wifi-sec",
	),
	SETTING_INFO (WPAN,
		.valid_parts = NM_META_SETTING_VALID_PARTS (
			NM_META_SETTING_VALID_PART_ITEM (CONNECTION,            TRUE),
			NM_META_SETTING_VALID_PART_ITEM (WPAN,                  TRUE),
		),
	),
};

/*****************************************************************************/

const NMMetaSettingValidPartItem *const nm_meta_setting_info_valid_parts_default[] = {
	NM_META_SETTING_VALID_PART_ITEM (CONNECTION, TRUE),
	NULL
};

/*****************************************************************************/

static const NMMetaSettingValidPartItem *const valid_settings_noslave[] = {
	NM_META_SETTING_VALID_PART_ITEM (MATCH,      FALSE),
	NM_META_SETTING_VALID_PART_ITEM (IP4_CONFIG, FALSE),
	NM_META_SETTING_VALID_PART_ITEM (IP6_CONFIG, FALSE),
	NM_META_SETTING_VALID_PART_ITEM (TC_CONFIG,  FALSE),
	NM_META_SETTING_VALID_PART_ITEM (PROXY,      FALSE),
	NULL,
};

static const NMMetaSettingValidPartItem *const valid_settings_slave_bridge[] = {
	NM_META_SETTING_VALID_PART_ITEM (BRIDGE_PORT, TRUE),
	NULL,
};

static const NMMetaSettingValidPartItem *const valid_settings_slave_ovs_bridge[] = {
	NM_META_SETTING_VALID_PART_ITEM (OVS_PORT, FALSE),
	NULL,
};

static const NMMetaSettingValidPartItem *const valid_settings_slave_ovs_port[] = {
	NM_META_SETTING_VALID_PART_ITEM (OVS_INTERFACE, FALSE),
	NULL,
};

static const NMMetaSettingValidPartItem *const valid_settings_slave_team[] = {
	NM_META_SETTING_VALID_PART_ITEM (TEAM_PORT, TRUE),
	NULL,
};

const NMMetaSettingValidPartItem *const*
nm_meta_setting_info_valid_parts_for_slave_type (const char *slave_type, const char **out_slave_name)
{
	if (!slave_type) {
		NM_SET_OUT (out_slave_name, NULL);
		return valid_settings_noslave;
	}
	if (nm_streq (slave_type, NM_SETTING_BOND_SETTING_NAME)) {
		NM_SET_OUT (out_slave_name, "bond-slave");
		return NM_PTRARRAY_EMPTY (const NMMetaSettingValidPartItem *);
	}
	if (nm_streq (slave_type, NM_SETTING_BRIDGE_SETTING_NAME)) {
		NM_SET_OUT (out_slave_name, "bridge-slave");
		return valid_settings_slave_bridge;
	}
	if (nm_streq (slave_type, NM_SETTING_OVS_BRIDGE_SETTING_NAME)) {
		NM_SET_OUT (out_slave_name, "ovs-slave");
		return valid_settings_slave_ovs_bridge;
	}
	if (nm_streq (slave_type, NM_SETTING_OVS_PORT_SETTING_NAME)) {
		NM_SET_OUT (out_slave_name, "ovs-slave");
		return valid_settings_slave_ovs_port;
	}
	if (nm_streq (slave_type, NM_SETTING_TEAM_SETTING_NAME)) {
		NM_SET_OUT (out_slave_name, "team-slave");
		return valid_settings_slave_team;
	}
	return NULL;
}

/*****************************************************************************/

static const char *
_meta_type_setting_info_editor_get_name (const NMMetaAbstractInfo *abstract_info, gboolean for_header)
{
	if (for_header)
		return N_("name");
	return ((const NMMetaSettingInfoEditor *) abstract_info)->general->setting_name;
}

static const char *
_meta_type_property_info_get_name (const NMMetaAbstractInfo *abstract_info, gboolean for_header)
{
	return ((const NMMetaPropertyInfo *) abstract_info)->property_name;
}

static gconstpointer
_meta_type_setting_info_editor_get_fcn (const NMMetaAbstractInfo *abstract_info,
                                        const NMMetaEnvironment *environment,
                                        gpointer environment_user_data,
                                        gpointer target,
                                        gpointer target_data,
                                        NMMetaAccessorGetType get_type,
                                        NMMetaAccessorGetFlags get_flags,
                                        NMMetaAccessorGetOutFlags *out_flags,
                                        gboolean *out_is_default,
                                        gpointer *out_to_free)
{
	const NMMetaSettingInfoEditor *info = (const NMMetaSettingInfoEditor *) abstract_info;

	nm_assert (!out_to_free || !*out_to_free);
	nm_assert (out_flags && !*out_flags);
	nm_assert (!target_data);

	if (!NM_IN_SET (get_type,
	                NM_META_ACCESSOR_GET_TYPE_PARSABLE,
	                NM_META_ACCESSOR_GET_TYPE_PRETTY))
		return NULL;

	nm_assert (out_to_free);

	return info->general->setting_name;
}

static gconstpointer
_meta_type_property_info_get_fcn (const NMMetaAbstractInfo *abstract_info,
                                  const NMMetaEnvironment *environment,
                                  gpointer environment_user_data,
                                  gpointer target,
                                  gpointer target_data,
                                  NMMetaAccessorGetType get_type,
                                  NMMetaAccessorGetFlags get_flags,
                                  NMMetaAccessorGetOutFlags *out_flags,
                                  gboolean *out_is_default,
                                  gpointer *out_to_free)
{
	const NMMetaPropertyInfo *info = (const NMMetaPropertyInfo *) abstract_info;

	nm_assert (!out_to_free || !*out_to_free);
	nm_assert (out_flags && !*out_flags);

	if (!NM_IN_SET (get_type,
	                NM_META_ACCESSOR_GET_TYPE_PARSABLE,
	                NM_META_ACCESSOR_GET_TYPE_PRETTY))
		return NULL;

	nm_assert (out_to_free);

	if (   info->is_secret
	    && !NM_FLAGS_HAS (get_flags, NM_META_ACCESSOR_GET_FLAGS_SHOW_SECRETS)) {
		NM_SET_OUT (out_is_default, TRUE);
		return _get_text_hidden (get_type);
	}

	return info->property_type->get_fcn (info,
	                                     environment,
	                                     environment_user_data,
	                                     target,
	                                     get_type,
	                                     get_flags,
	                                     out_flags,
	                                     out_is_default,
	                                     out_to_free);

}

static const NMMetaAbstractInfo *const*
_meta_type_setting_info_editor_get_nested (const NMMetaAbstractInfo *abstract_info,
                                           guint *out_len,
                                           gpointer *out_to_free)
{
	const NMMetaSettingInfoEditor *info;

	info = (const NMMetaSettingInfoEditor *) abstract_info;

	NM_SET_OUT (out_len, info->properties_num);
	return (const NMMetaAbstractInfo *const*) info->properties;
}

static const NMMetaAbstractInfo *const*
_meta_type_property_info_get_nested (const NMMetaAbstractInfo *abstract_info,
                                     guint *out_len,
                                     gpointer *out_to_free)
{
	NM_SET_OUT (out_len, 0);
	return NULL;
}

static const char *const*
_meta_type_property_info_complete_fcn (const NMMetaAbstractInfo *abstract_info,
                                       const NMMetaEnvironment *environment,
                                       gpointer environment_user_data,
                                       const NMMetaOperationContext *operation_context,
                                       const char *text,
                                       char ***out_to_free)
{
	const NMMetaPropertyInfo *info = (const NMMetaPropertyInfo *) abstract_info;

	nm_assert (out_to_free && !*out_to_free);

	if (info->property_type->complete_fcn) {
		return info->property_type->complete_fcn (info,
		                                          environment,
		                                          environment_user_data,
		                                          operation_context,
		                                          text,
		                                          out_to_free);
	}

	if (info->property_type->values_fcn) {
		return info->property_type->values_fcn (info,
		                                        out_to_free);
	}

	if (   info->property_typ_data
	    && info->property_typ_data->values_static)
		return info->property_typ_data->values_static;

	return NULL;
}

const NMMetaType nm_meta_type_setting_info_editor = {
	.type_name =         "setting_info_editor",
	.get_name =          _meta_type_setting_info_editor_get_name,
	.get_nested =        _meta_type_setting_info_editor_get_nested,
	.get_fcn =           _meta_type_setting_info_editor_get_fcn,
};

const NMMetaType nm_meta_type_property_info = {
	.type_name =        "property_info",
	.get_name =         _meta_type_property_info_get_name,
	.get_nested =       _meta_type_property_info_get_nested,
	.get_fcn =          _meta_type_property_info_get_fcn,
	.complete_fcn =     _meta_type_property_info_complete_fcn,
};

const NMMetaType nm_meta_type_nested_property_info = {
	.type_name =        "nested_property_info",
};
