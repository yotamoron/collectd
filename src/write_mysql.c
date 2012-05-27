/**
 * collectd - src/write_mysql.c
 * Copyright (C) 2011-2012  Cyril Feraudet
 * Copyright (C) 2012       Florian Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Cyril Feraudet <cyril at feraudet.com>
 *   Florian Forster <octo at collectd.org>
 **/

#include "collectd.h"
#include "plugin.h"
#include "common.h"

#include "utils_cache.h"
#include "utils_parse_option.h"
#include "utils_avltree.c"

#ifdef HAVE_MYSQL_H
#include <mysql.h>
#elif defined(HAVE_MYSQL_MYSQL_H)
#include <mysql/mysql.h>
#endif

#include <pthread.h>

typedef struct dataset_s dataset_t;
struct dataset_s
{
  char name[DATA_MAX_NAME_LEN];
  int id;
  int type_id;
};

const char *wm_data_statement = "INSERT INTO data "
  "(identifier_id, timestamp, value) VALUES (?, ?, ?)";
struct wm_data_binding_s
{
  int identifier_id;
  MYSQL_TIME timestamp;
  double value;
  my_bool value_is_null;
#define WM_DATA_BINDING_FIELDS_NUM 3
};
typedef struct wm_data_binding_s wm_data_binding_t;

const char *wm_identifier_statement_select = "SELECT id FROM identifier "
  "WHERE host = ? AND plugin = ? AND plugin_instance = ? "
  "AND type = ? AND type_instance = ? AND data_source_name = ?";
const char *wm_identifier_statement_insert = "INSERT INTO identifier "
  "(host, plugin, plugin_instance, type, type_instance, "
  "data_source_name, data_source_type) "
  "VALUES (?, ?, ?, ?, ?, ?, ?)";
struct wm_identifier_binding_s
{
  char host[DATA_MAX_NAME_LEN];
  char plugin[DATA_MAX_NAME_LEN];
  char plugin_instance[DATA_MAX_NAME_LEN];
  char type[DATA_MAX_NAME_LEN];
  char type_instance[DATA_MAX_NAME_LEN];
  char data_source_name[DATA_MAX_NAME_LEN];
  char data_source_type[DATA_MAX_NAME_LEN];
#define WM_IDENTIFIER_BINDING_FIELDS_NUM 7
};
typedef struct wm_identifier_binding_s wm_identifier_binding_t;

typedef int wm_identifier_cache_entry_t;

struct wm_callback_s
{
  char *host;
  int   port;
  char *user;
  char *passwd;
  char *database;

  MYSQL *conn;
  pthread_mutex_t conn_lock;

  pthread_mutex_t data_lock;
  MYSQL_STMT *data_stmt;
  wm_data_binding_t data_values;

  pthread_mutex_t identifier_lock;
  c_avl_tree_t *identifier_cache;

  MYSQL_STMT *identifier_stmt_select;

  MYSQL_STMT *identifier_stmt_insert;
};
typedef struct wm_callback_s wm_callback_t;

#define HOST_ITEM   0
#define PLUGIN_ITEM 1
#define TYPE_ITEM   2

static void wm_callback_init (wm_callback_t *cb) /* {{{ */
{
  memset (cb, 0, sizeof (*cb));

  cb->host = NULL;
  cb->user = NULL;
  cb->passwd = NULL;
  cb->database = NULL;
  cb->conn = NULL;
  cb->data_stmt = NULL;

  pthread_mutex_init (&cb->conn_lock, /* attr = */ NULL);

  pthread_mutex_init (&cb->data_lock, /* attr = */ NULL);
} /* }}} void wm_callback_init */

static int wm_callback_disconnect (wm_callback_t *cb) /* {{{ */
{
  if (cb->conn == NULL)
    return (0);

  if (cb->data_stmt != NULL)
  {
    mysql_stmt_close (cb->data_stmt);
    cb->data_stmt = NULL;
  }

  if (cb->identifier_stmt_select != NULL)
  {
    mysql_stmt_close (cb->identifier_stmt_select);
    cb->identifier_stmt_select = NULL;
  }

  if (cb->identifier_stmt_insert != NULL)
  {
    mysql_stmt_close (cb->identifier_stmt_insert);
    cb->identifier_stmt_insert = NULL;
  }

  mysql_close (cb->conn);
  cb->conn = NULL;

  return (0);
} /* }}} int wm_callback_disconnect */

static int wm_callback_connect (wm_callback_t *cb) /* {{{ */
{
  int status;
  my_bool flag;

  if (cb->conn != NULL)
    return (0);

  assert (cb->data_stmt == NULL);

  cb->conn = mysql_init (/* old conn = */ NULL);
  if (cb->conn == NULL)
  {
    ERROR ("write_mysql plugin: mysql_init() failed.");
    return (-1);
  }

  flag = (my_bool) 1;
  mysql_options (cb->conn, MYSQL_OPT_RECONNECT, (void *) &flag);
  /* TODO: Make this configurable */
  mysql_options (cb->conn, MYSQL_OPT_COMPRESS, /* unused */ NULL);

  if (mysql_real_connect (cb->conn, cb->host, cb->user, cb->passwd,
        /* cb->database */ NULL,
        cb->port,
        /* unix sock = */ NULL, /* flags = */ 0) == NULL)
  {
    ERROR ("write_mysql plugin: mysql_real_connect (%s, %i) failed: %s",
        cb->host, cb->port, mysql_error (cb->conn));
    DEBUG ("cb->passwd = %s", cb->passwd);
    wm_callback_disconnect (cb);
    return (-1);
  }

#if 1
  status = mysql_select_db (cb->conn, cb->database);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_select_db (%s) failed: %s",
        cb->database, mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }
#endif

  /* Prepare the data INSERT statement */
  cb->data_stmt = mysql_stmt_init (cb->conn);
  if (cb->data_stmt == NULL)
  {
    ERROR ("write_mysql plugin: mysql_stmt_init() failed: %s",
        mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }

  status = mysql_stmt_prepare (cb->data_stmt,
      wm_data_statement, (unsigned long) strlen (wm_data_statement));
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_prepare(\"%s\") failed: %s",
        wm_data_statement, mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }
  DEBUG ("write_mysql plugin: Statement prepared: \"%s\"",
      wm_data_statement);

  /* Prepare the identifier SELECT statement */
  cb->identifier_stmt_select = mysql_stmt_init (cb->conn);
  if (cb->identifier_stmt_select == NULL)
  {
    ERROR ("write_mysql plugin: mysql_stmt_init() failed: %s",
        mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }

  status = mysql_stmt_prepare (cb->identifier_stmt_select,
      wm_identifier_statement_select,
      (unsigned long) strlen (wm_identifier_statement_select));
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_prepare(\"%s\") failed: %s",
        wm_identifier_statement_select, mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }
  DEBUG ("write_mysql plugin: Statement prepared: \"%s\"",
      wm_identifier_statement_select);

  /* Prepare the identifier INSERT statement */
  cb->identifier_stmt_insert = mysql_stmt_init (cb->conn);
  if (cb->identifier_stmt_insert == NULL)
  {
    ERROR ("write_mysql plugin: mysql_stmt_init() failed: %s",
        mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }

  status = mysql_stmt_prepare (cb->identifier_stmt_insert,
      wm_identifier_statement_insert,
      (unsigned long) strlen (wm_identifier_statement_insert));
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_prepare(\"%s\") failed: %s",
        wm_identifier_statement_insert, mysql_error (cb->conn));
    wm_callback_disconnect (cb);
    return (-1);
  }
  DEBUG ("write_mysql plugin: Statement prepared: \"%s\"",
      wm_identifier_statement_insert);

  return (0);
} /* }}} int wm_callback_connect */

static void wm_callback_free (void *data) /* {{{ */
{
  wm_callback_t *cb = data;

  if (cb == NULL)
    return;

  wm_callback_disconnect (cb);

  sfree (cb->host);
  sfree (cb->user);
  sfree (cb->passwd);
  sfree (cb->database);
} /* }}} void wm_callback_free */

static int wm_identifier_cache_lookup (wm_callback_t *cb, /* {{{ */
    const char *identifier)
{
  intptr_t tmp = 0;
  int status;

  status = c_avl_get (cb->identifier_cache,
      (const void *) identifier, (void *) &tmp);

  if (status != 0)
    return (-1);
  return ((int) tmp);
} /* }}} int wm_identifier_cache_lookup */

static int wm_identifier_cache_insert (wm_callback_t *cb, /* {{{ */
    const char *identifier, int id)
{
  intptr_t tmp = (intptr_t) id;
  int status;

  char *key = strdup (identifier);
  if (key == NULL)
    return (ENOMEM);

  status = c_avl_insert (cb->identifier_cache,
      (void *) key, (void *) tmp);
  if (status != 0)
    sfree (key);

  return (status);
} /* }}} int wm_identifier_cache_insert */

static int wm_identifier_database_insert (wm_callback_t *cb, /* {{{ */
    const value_list_t *vl, const data_set_t *ds, int index)
{
  MYSQL_BIND binding[7];
  char ds_type[DATA_MAX_NAME_LEN];
  int status;
  int id;

  status = (int) mysql_stmt_reset (cb->identifier_stmt_insert);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_reset failed: %s",
        mysql_stmt_error (cb->identifier_stmt_insert));
    return (-1);
  }

  sstrncpy (ds_type, DS_TYPE_TO_STRING (ds->ds[index].type),
      sizeof (ds_type));

  memset (binding, 0, sizeof (binding));
  binding[0].buffer_type = MYSQL_TYPE_STRING;
  binding[0].buffer_length = (unsigned long) strlen (vl->host);
  binding[0].buffer = (void *) &vl->host[0];
  binding[1].buffer_type = MYSQL_TYPE_STRING;
  binding[1].buffer_length = (unsigned long) strlen (vl->plugin);
  binding[1].buffer = (void *) &vl->plugin[0];
  binding[2].buffer_type = MYSQL_TYPE_STRING;
  binding[2].buffer_length = (unsigned long) strlen (vl->plugin_instance);
  binding[2].buffer = (void *) &vl->plugin_instance[0];
  binding[3].buffer_type = MYSQL_TYPE_STRING;
  binding[3].buffer_length = (unsigned long) strlen (vl->type);
  binding[3].buffer = (void *) &vl->type[0];
  binding[4].buffer_type = MYSQL_TYPE_STRING;
  binding[4].buffer_length = (unsigned long) strlen (vl->type_instance);
  binding[4].buffer = (void *) &vl->type_instance[0];
  binding[5].buffer_type = MYSQL_TYPE_STRING;
  binding[5].buffer_length = (unsigned long) strlen (ds->ds[index].name);
  binding[5].buffer = (void *) &ds->ds[index].name[0];
  binding[6].buffer_type = MYSQL_TYPE_STRING;
  binding[6].buffer_length = (unsigned long) strlen (ds_type);
  binding[6].buffer = (void *) &ds_type[0];

  status = (int) mysql_stmt_bind_param (cb->identifier_stmt_insert, binding);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_bind_param failed: %s",
        mysql_stmt_error (cb->identifier_stmt_insert));
    return (-1);
  }

  status = mysql_stmt_execute (cb->identifier_stmt_insert);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_execute failed: %s",
        mysql_stmt_error (cb->identifier_stmt_insert));
    return (-1);
  }

  status = (int) mysql_stmt_affected_rows (cb->identifier_stmt_insert);
  if (status != 1)
  {
    ERROR ("write_mysql plugin: mysql_stmt_affected_rows returned %i, "
        "expected 1.", status);
    return (-1);
  }

  id = (int) mysql_stmt_insert_id (cb->identifier_stmt_insert);
  DEBUG ("write_mysql plugin: New identifier has ID %i.", id);

  return (id);
} /* }}} int wm_identifier_database_insert */

static int wm_identifier_database_lookup (wm_callback_t *cb, /* {{{ */
    const value_list_t *vl, const data_set_t *ds, int index)
{
  MYSQL_BIND param_bind[6];
  MYSQL_BIND return_bind;
  int id = -1;
  my_bool id_is_null = 0;
  unsigned long affected_rows;
  int status;

  memset (param_bind, 0, sizeof (param_bind));
  param_bind[0].buffer_type = MYSQL_TYPE_STRING;
  param_bind[0].buffer_length = (unsigned long) strlen (vl->host);
  param_bind[0].buffer = (void *) &vl->host[0];
  param_bind[1].buffer_type = MYSQL_TYPE_STRING;
  param_bind[1].buffer_length = (unsigned long) strlen (vl->plugin);
  param_bind[1].buffer = (void *) &vl->plugin[0];
  param_bind[2].buffer_type = MYSQL_TYPE_STRING;
  param_bind[2].buffer_length = (unsigned long) strlen (vl->plugin_instance);
  param_bind[2].buffer = (void *) &vl->plugin_instance[0];
  param_bind[3].buffer_type = MYSQL_TYPE_STRING;
  param_bind[3].buffer_length = (unsigned long) strlen (vl->type);
  param_bind[3].buffer = (void *) &vl->type[0];
  param_bind[4].buffer_type = MYSQL_TYPE_STRING;
  param_bind[4].buffer_length = (unsigned long) strlen (vl->type_instance);
  param_bind[4].buffer = (void *) &vl->type_instance[0];
  param_bind[5].buffer_type = MYSQL_TYPE_STRING;
  param_bind[5].buffer_length = (unsigned long) strlen (ds->ds[index].name);
  param_bind[5].buffer = (void *) &ds->ds[index].name[0];

  status = (int) mysql_stmt_bind_param (cb->identifier_stmt_select,
      param_bind);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_bind_param failed: %s",
        mysql_stmt_error (cb->identifier_stmt_select));
    return (-1);
  }

  /* Bind output buffers */
  memset (&return_bind, 0, sizeof (return_bind));
  return_bind.buffer_type = MYSQL_TYPE_LONG;
  return_bind.buffer = (void *) &id;
  return_bind.buffer_length = sizeof (id);
  return_bind.is_null = &id_is_null;

  status = (int) mysql_stmt_bind_result (cb->identifier_stmt_select,
      &return_bind);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_bind_result failed: %s",
        mysql_stmt_error (cb->identifier_stmt_select));
    return (-1);
  }

  /* Execute statement */
  DEBUG ("write_mysql plugin: Executing identifier_stmt_select.");
  status = mysql_stmt_execute (cb->identifier_stmt_select);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_execute failed: %s",
        mysql_stmt_error (cb->identifier_stmt_select));
    return (-1);
  }

  /* Fetch all results, otherwise mysql_stmt_num_rows() is not available. */
  status = mysql_stmt_store_result (cb->identifier_stmt_select);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_store_result failed: %s",
        mysql_stmt_error (cb->identifier_stmt_select));
    mysql_stmt_free_result (cb->identifier_stmt_select);
    return (-1);
  }

  affected_rows = (unsigned long) mysql_stmt_num_rows (
      cb->identifier_stmt_select);
  DEBUG ("write_mysql plugin: identifier_stmt_select returned %lu row(s).",
      affected_rows);
  if (affected_rows == 0)
  {
    mysql_stmt_free_result (cb->identifier_stmt_select);
    return (wm_identifier_database_insert (cb, vl, ds, index));
  }
  else if (affected_rows > 1)
  {
    ERROR ("write_mysql plugin: Looking up an identifier id returned %lu "
        "results.", affected_rows);
    mysql_stmt_free_result (cb->identifier_stmt_select);
    return (-1);
  }

  /* Copy value to the provided buffer "id". */
  status = mysql_stmt_fetch (cb->identifier_stmt_select);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: mysql_stmt_fetch failed: %s",
        mysql_stmt_error (cb->identifier_stmt_select));
    mysql_stmt_free_result (cb->identifier_stmt_select);
    return (-1);
  }
  DEBUG ("write_mysql plugin: id = %i; id_is_null = %i;",
      id, (int) id_is_null);

  mysql_stmt_free_result (cb->identifier_stmt_select);

  if (id_is_null)
  {
    /* XXX: If this ever fires at all, it will fire for *every* write
     * to this ID. */
    ERROR ("write_mysql plugin: NULL identifier id returned.");
    return (-1);
  }

  return (id);
} /* }}} int wm_identifier_database_lookup */

static int wm_identifier_to_id (wm_callback_t *cb, /* {{{ */
    const value_list_t *vl, const data_set_t *ds, int index)
{
  char tmp[6 * DATA_MAX_NAME_LEN];
  char identifier[7 * DATA_MAX_NAME_LEN];
  int status;

  status = FORMAT_VL (tmp, sizeof (tmp), vl);
  if (status != 0)
    return (-1);

  (void) ssnprintf (identifier, sizeof (identifier),
      "%s/%s", tmp, ds->ds[index].name);

  pthread_mutex_lock (&cb->identifier_lock);

  status = wm_identifier_cache_lookup (cb, identifier);
  if (status >= 0) /* likely */
  {
    pthread_mutex_unlock (&cb->identifier_lock);
    return (status);
  }
  else
  {
    DEBUG ("write_mysql plugin: Identifier \"%s\" not found in the cache.",
        identifier);
  }

  status = wm_identifier_database_lookup (cb, vl, ds, index);
  if (status >= 0)
    wm_identifier_cache_insert (cb, identifier, status);

  pthread_mutex_unlock (&cb->identifier_lock);

  return (status);
} /* }}} int wm_identifier_to_id */

static int wm_cdtime_t_to_mysql_time (cdtime_t in, MYSQL_TIME *out) /* {{{ */
{
  time_t t;
  struct tm stm;

  memset (out, 0, sizeof (*out));
  memset (&stm, 0, sizeof (stm));

  t = CDTIME_T_TO_TIME_T (in);
  if (localtime_r (&t, &stm) == NULL)
  {
    ERROR ("write_mysql plugin: localtime_r(%.3f) failed.",
        CDTIME_T_TO_DOUBLE (in));
    return (-1);
  }

  out->year   = stm.tm_year + 1900;
  out->month  = stm.tm_mon + 1;
  out->day    = stm.tm_mday;
  out->hour   = stm.tm_hour;
  out->minute = stm.tm_min;
  out->second = stm.tm_sec;

  return (0);
} /* }}} int wm_cdtime_t_to_mysql_time */

static int wm_write_locked (wm_callback_t *cb, /* {{{ */
    const data_set_t *ds, const value_list_t *vl,
    gauge_t *rates)
{
  MYSQL_TIME vl_time;
  MYSQL_BIND param_bind[3];
  int status;
  int i;

  status = wm_callback_connect (cb);
  if (status != 0)
  {
    ERROR ("write_mysql plugin: Unable to connect.");
    return (status);
  }

  status = wm_cdtime_t_to_mysql_time (vl->time, &vl_time);
  if (status != 0)
  {
    char errbuf[1024];
    ERROR ("write_mysql plugin: wm_cdtime_t_to_mysql_time failed: %s.",
        sstrerror (errno, errbuf, sizeof (errbuf)));
    return (status);
  }

  for (i = 0; i < ds->ds_num; i++)
  {
    int dataset_id;

    dataset_id = wm_identifier_to_id (cb, vl, ds, i);
    if (dataset_id < 0)
      continue;

    memset (param_bind, 0, sizeof (param_bind));
    param_bind[0].buffer_type = MYSQL_TYPE_LONG;
    param_bind[0].buffer_length = 0;
    param_bind[0].buffer = (void *) &dataset_id;
    param_bind[1].buffer_type = MYSQL_TYPE_DATETIME;
    param_bind[1].buffer_length = 0;
    param_bind[1].buffer = (void *) &vl_time;
    param_bind[2].buffer_type = MYSQL_TYPE_DOUBLE;
    param_bind[2].buffer_length = 0;
    param_bind[2].buffer = rates + i;

    status = (int) mysql_stmt_bind_param (cb->data_stmt, param_bind);
    if (status != 0)
    {
      ERROR ("write_mysql plugin: mysql_stmt_bind_param failed: %s",
          mysql_stmt_error (cb->data_stmt));
      return (-1);
    }

    status = mysql_stmt_execute (cb->data_stmt);
    if (status != 0)
    {
      ERROR ("write_mysql plugin: mysql_stmt_execute failed: %s",
          mysql_stmt_error (cb->data_stmt));
      return (-1);
    }
  } /* for (ds->ds) */

  return (0);
} /* }}} wm_write_locked */

static int wm_write (const data_set_t *ds, const value_list_t *vl, /* {{{ */
    user_data_t *user_data)
{
  wm_callback_t *cb = user_data->data;
  gauge_t *rates;
  int status;

  assert (cb != NULL);

  rates = uc_get_rate (ds, vl);
  if (rates == NULL)
    return (-1);

  pthread_mutex_lock (&cb->conn_lock);
  status = wm_write_locked (cb, ds, vl, rates);
  pthread_mutex_unlock (&cb->conn_lock);

  sfree (rates);

  return (status);
} /* }}} int wm_write */

static int wm_config_instance (const oconfig_item_t *ci) /* {{{ */
{
  wm_callback_t *cb;
  user_data_t user_data;
  char callback_name[DATA_MAX_NAME_LEN];
  int i;

  cb = malloc (sizeof (*cb));
  if (cb == NULL)
  {
    ERROR ("write_mysql plugin: malloc(3) failed.");
    return (ENOMEM);
  }
  wm_callback_init (cb);

  for (i = 0; i < ci->children_num; i++)
  {
    const oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Host", child->key) == 0)
      cf_util_get_string (child, &cb->host);
    else if (strcasecmp ("Port", child->key) == 0)
    {
      int tmp = cf_util_get_port_number (child);
      if (tmp > 0)
        cb->port = tmp;
    }
    else if (strcasecmp ("User", child->key) == 0)
      cf_util_get_string (child, &cb->user);
    else if (strcasecmp ("Password", child->key) == 0)
      cf_util_get_string (child, &cb->passwd);
    else if (strcasecmp ("Database", child->key) == 0)
      cf_util_get_string (child, &cb->database);
    else
      ERROR ("write_mysql plugin: The config option \"%s\" "
          "is not allowed in \"Instance\" blocks.", child->key);
  }

  /* TODO: Sanity checking */

  cb->identifier_cache = c_avl_create ((void *) strcmp);
  assert (cb->identifier_cache != NULL);

  ssnprintf (callback_name, sizeof (callback_name), "write_mysql/%s/%i",
      cb->host, cb->port);

  memset (&user_data, 0, sizeof (user_data));
  user_data.data = cb;
  user_data.free_func = wm_callback_free;
  plugin_register_write (callback_name, wm_write, &user_data);

  return (0);
} /* }}} int wm_config_instance */

static int wm_config (oconfig_item_t *ci) /* {{{ */
{
  int i;

  for (i = 0; i < ci->children_num; i++)
  {
    const oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Instance", child->key) == 0)
      wm_config_instance (child);
    else
      ERROR ("write_mysql plugin: The config option \"%s\" "
          "is not allowed here.", child->key);
  }

  return (0);
} /* }}} int wm_config */

void module_register (void)
{
  plugin_register_complex_config ("write_mysql", wm_config);
}

/* vim: set sw=2 sts=2 et fdm=marker : */
