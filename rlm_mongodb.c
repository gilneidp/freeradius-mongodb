/*
 * rlm_mongodb.c
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2013       Neutron Soutmun <neutron@rahunas.org>
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/token.h>

#include <string.h>
#include <sys/time.h>

#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#define PTHREAD_MUTEX_LOCK   pthread_mutex_lock
#define PTHREAD_MUTEX_UNLOCK pthread_mutex_unlock
#else
#define PTHREAD_MUTEX_LOCK(_x)
#define PTHREAD_MUTEX_UNLOCK(_x)
#endif

#include <mongo.h>

typedef struct rlm_mongodb_t MONGODB_INST;

typedef struct rlm_mongodb_t {
  mongo_sync_pool  *pool;
  char             *hostname;
  int const         port;
  char             *database;
  int const         numconns;
  char              col_users[256];
  char              col_groups[256];
  char              col_acct[256];
  int const         read_groups;
  int const         verify_session;
  int const         zap_stale_sessions;

#ifdef HAVE_PTHREAD_H
  pthread_mutex_t   mutex;
#else
  UNUSED int        mutex;
#endif
} rlm_mongodb_t;

static const CONF_PARSER module_config[] = {
  { "hostname", PW_TYPE_STRING_PTR,
    offsetof(MONGODB_INST, hostname), NULL, "127.0.0.1" },
  { "port", PW_TYPE_INTEGER,
    offsetof(MONGODB_INST, port), NULL, "27017" },
  { "database", PW_TYPE_STRING_PTR,
    offsetof(MONGODB_INST, database), NULL, "radius" },
  { "num_connections", PW_TYPE_INTEGER,
    offsetof(MONGODB_INST, numconns), NULL, "10" },
  { "read_groups", PW_TYPE_BOOLEAN,
    offsetof(MONGODB_INST, read_groups), NULL, "yes" },
  { "verify_session", PW_TYPE_BOOLEAN,
    offsetof(MONGODB_INST, verify_session), NULL, "yes" },
  { "zap_stale_sessions", PW_TYPE_BOOLEAN,
    offsetof(MONGODB_INST, zap_stale_sessions), NULL, "yes" },
  { NULL, -1, 0, NULL, NULL } /* end of the list */
};

static int mongodb_detach (void *instance)
{
  MONGODB_INST *inst = instance;

  if (inst->pool) {
    mongo_sync_pool_free (inst->pool);
  }

  if (inst->hostname) {
    free (inst->hostname);
    inst->hostname = NULL;
  }

  if (inst->database) {
    free (inst->database);
    inst->database = NULL;
  }

  free (inst);

  return 0;
}

static int mongodb_connection_setoptions (MONGODB_INST *inst)
{
  mongo_sync_pool_connection **conns = NULL;
  conns = rad_malloc (sizeof (conns) * inst->numconns);
  int ret = 0;

  if (!conns)
    return -1;

  int i;
  for (i = 0; i < inst->numconns; i++) {
    conns[i] = mongo_sync_pool_pick (inst->pool, TRUE);
    if (!conns[i]) {
      ret = -1;
      break;
    }

    mongo_sync_conn_set_auto_reconnect ((mongo_sync_connection *) conns[i], TRUE);
  }

  for (i = 0; ret == 0 && i < inst->numconns; i++) {
    if (mongo_sync_pool_return (inst->pool, conns[i]) != TRUE) {
      ret = -1;
      break;
    }
  }

  free (conns);
  return ret;
}

static int mongodb_instantiate (CONF_SECTION *conf, void **instance)
{
  MONGODB_INST *inst;
  inst = rad_malloc (sizeof(MONGODB_INST));
  if (!inst)
    return -1;
  memset(inst, 0, sizeof (MONGODB_INST));

  if (cf_section_parse (conf, inst, module_config) < 0) {
    mongodb_detach (inst);
    return -1;
  }

  if (!inst->database) {
    radlog (L_ERR, "rlm_mongodb: Invalid database name");
    mongodb_detach (inst);
    return -1;
  }

  if (inst->numconns <= 0 || inst->numconns > 1024) {
    radlog (L_ERR, "rlm_mongodb: Invalid connections number (> 1024)");
    mongodb_detach (inst);
    return -1;
  }

  inst->pool = mongo_sync_pool_new(inst->hostname, inst->port,
                                   inst->numconns, 0);
  if (!inst->pool) {
    radlog (L_ERR, "rlm_mongodb: Could not create connections pool");
    mongodb_detach (inst);
    return -1;
  }

  if (mongodb_connection_setoptions (inst) != 0) {
    radlog (L_ERR, "rlm_mongodb: Could not set connection options");
    mongodb_detach (inst);
    return -1;
  }


#ifdef HAVE_PTHREAD_H
  if (pthread_mutex_init (&inst->mutex, NULL) != 0) {
    radlog (L_ERR, "rlm_mongodb: Could not initialize mutex");
    mongodb_detach (inst);
    return -1;
  }

  DEBUG ("rlm_mongodb: Multi threaded support");
#endif

  snprintf (inst->col_users, sizeof (inst->col_users) - 1,
            "%s.%s", inst->database, "users");
  snprintf (inst->col_groups, sizeof (inst->col_groups) - 1,
            "%s.%s", inst->database, "groups");
  snprintf (inst->col_acct, sizeof (inst->col_acct) - 1,
            "%s.%s", inst->database, "acct");
  inst->col_users[sizeof (inst->col_users) - 1]   = '\0';
  inst->col_groups[sizeof (inst->col_groups) - 1] = '\0';
  inst->col_acct[sizeof (inst->col_acct) - 1] = '\0';

  *instance = inst;

  return RLM_MODULE_OK;
}

static mongo_sync_pool_connection *mongodb_get_conn (MONGODB_INST *inst)
{
  PTHREAD_MUTEX_LOCK (&inst->mutex);
  mongo_sync_pool_connection *conn = mongo_sync_pool_pick (inst->pool, TRUE);
  PTHREAD_MUTEX_UNLOCK (&inst->mutex);

  return conn;
}

static int mongodb_return_conn (MONGODB_INST *inst,
                                mongo_sync_pool_connection *conn)
{
  gboolean ret = FALSE;

  PTHREAD_MUTEX_LOCK (&inst->mutex);
  ret = mongo_sync_pool_return (inst->pool, conn);
  PTHREAD_MUTEX_UNLOCK (&inst->mutex);

  return ret == TRUE ? 0 : -1;
}

static int mongodb_cursor_getvpdata (bson_cursor *data, VALUE_PAIR **first_pair,
                                     const char *def_attr)
{
  int is_fail    = 0;
  bson *array = NULL;

  bson_cursor_get_array (data, &array);
  bson_cursor *c = bson_cursor_new (array);

  while (bson_cursor_next (c) && !is_fail) {
    const char *attribute = NULL;
    const char *op        = NULL;
    const char *value     = NULL;

    if (bson_cursor_type (c) != BSON_TYPE_DOCUMENT) {
      if (def_attr) {
        attribute = def_attr;

        switch (bson_cursor_type (c)) {
          case BSON_TYPE_STRING:
            bson_cursor_get_string (c, &value);
            break;
          default:
            radlog (L_ERR, "rlm_mongodb: Unsupport getting non string value from"
                    "non attribute-value pair for '%s'", def_attr);
            break;
        }

        if (value) {
          VALUE_PAIR *new_pair = pairmake (attribute, value, T_OP_CMP_EQ);
          if (new_pair) {
            pairadd (first_pair, new_pair);
          } else {
            radlog (L_ERR, "rlm_mongodb: Failed to create the pair: %s",
                    fr_strerror ());
          }
        }
      }

      continue;
    }

    bson *doc = NULL;
    bson_cursor_get_document (c, &doc);
    bson_cursor *attr = bson_cursor_new (doc);


    while (bson_cursor_next (attr)) {
      if (bson_cursor_type (attr) != BSON_TYPE_STRING)
        continue;

      if (strcmp (bson_cursor_key (attr), "attribute") == 0) {
        bson_cursor_get_string (attr, &attribute);
      } else if (strcmp (bson_cursor_key (attr), "op") == 0) {
        bson_cursor_get_string (attr, &op);
      } else if (strcmp (bson_cursor_key (attr), "value") == 0) {
        bson_cursor_get_string (attr, &value);
      }
    }

    if (!attribute || attribute[0] == '\0') {
      radlog (L_ERR, "rlm_mongodb: The 'attribute' field is empty or NULL, "
                     "skipping");
      goto next;
    }

    char buf[MAX_STRING_LEN];
    FR_TOKEN operator = T_EOL;
    if (op && op[0] != '\0') {
      operator = gettoken (&op, buf, sizeof (buf));

      if ((operator < T_OP_ADD) ||
          (operator > T_OP_CMP_EQ)) {
        radlog (L_ERR, "rlm_mongodb: Invalid operator '%s' for attribute %s",
                op, attribute);
        goto next;
      }
    } else {
      operator = T_OP_CMP_EQ;
      radlog (L_ERR, "rlm_mongodb: The 'op' field for attribute '%s' is NULL, "
              "or non-existent.", attribute);
      radlog (L_ERR, "rlm_mongodb: You MUST FIX THIS if you want the "
              "configuration to behave as you expect.");
    }

    VALUE_PAIR *new_pair = pairmake (attribute, value, operator);
    if (new_pair) {
      pairadd (first_pair, new_pair);
    } else {
      radlog (L_ERR, "rlm_mongodb: Failed to create the pair: %s",
              fr_strerror ());
      is_fail = 1;
    }

next:
    bson_cursor_free (attr);
    bson_free (doc);
  }

  bson_cursor_free (c);
  bson_free (array);

  return is_fail;
}

static int mongodb_getvpdata (MONGODB_INST *inst, REQUEST *request,
                              const char *collection, bson *query, bson *select,
                              VALUE_PAIR **first_pair, const char *def_attr)
{
  int docs_count = 0; 
  mongo_sync_pool_connection *conn = mongodb_get_conn (inst);
  mongo_sync_cursor *sc     = NULL;
  mongo_packet      *p      = NULL;

  if (!conn) {
    radlog_request (L_ERR, 0, request, "Maximum %d connections exceeded; "
                    "rejecting user", inst->numconns);
    return -1;
  }

  p = mongo_sync_cmd_query ((mongo_sync_connection *) conn, collection,
                            0, 0, 3, query, select);

  if (!p) {
    docs_count = 0;
    goto out;
  }

  sc = mongo_sync_cursor_new ((mongo_sync_connection *) conn, collection, p);

  if (!sc) {
    radlog_request (L_ERR, 0, request, "Error create new cursor");
    docs_count = -1;
    mongo_wire_packet_free (p);
    goto out;
  }

  int fail = 0;

  while (mongo_sync_cursor_next (sc) && !fail) {
    bson *result = mongo_sync_cursor_get_data (sc);

    if (!result)
      break;

    bson_cursor *slc = bson_cursor_new (select);

    while (bson_cursor_next (slc) && !fail) {
      int selected = 0;
      bson_cursor_get_int32 (slc, &selected);

      if (!selected)
        continue;

      bson_cursor *doc = bson_find (result, bson_cursor_key (slc));
      if (bson_cursor_type (doc) != BSON_TYPE_ARRAY) {
        bson_cursor_free (doc);
        continue;
      }

      fail = mongodb_cursor_getvpdata (doc, first_pair, def_attr);

      if (!fail)
        docs_count++;

      bson_cursor_free (doc);
    }

    bson_cursor_free (slc);
    bson_free (result);
  }

  mongo_sync_cursor_free (sc);

out:
  if (mongodb_return_conn (inst, conn) == -1) {
    radlog_request (L_ERR, 0, request,
                    "The connection was not returned to the pool; "
                    "rejecting user");
    return -1; 
  }

  return docs_count;
}

static int mongodb_user_check (MONGODB_INST *inst, REQUEST *request,
                               VALUE_PAIR **pair)
{
  bson       *query  = NULL;
  bson       *select = NULL;
  const char *username   = request->username->vp_strvalue;
  int         docs_count = 0;

  query = bson_build (BSON_TYPE_STRING, "username", username, -1,
                      BSON_TYPE_NONE);
  bson_finish (query);

  select = bson_build (BSON_TYPE_INT32, "check", 1, BSON_TYPE_NONE);
  bson_finish (select);

  docs_count = mongodb_getvpdata (inst, request, inst->col_users, query,
                                  select, pair, NULL);

  bson_free (query);
  bson_free (select);

  return docs_count;
}

static int mongodb_user_reply (MONGODB_INST *inst, REQUEST *request,
                               VALUE_PAIR **pair)
{
  bson       *query  = NULL;
  bson       *select = NULL;
  const char *username   = request->username->vp_strvalue;
  int         docs_count = 0;

  query = bson_build (BSON_TYPE_STRING, "username", username, -1,
                      BSON_TYPE_NONE);
  bson_finish (query);

  select = bson_build (BSON_TYPE_INT32, "reply", 1, BSON_TYPE_NONE);
  bson_finish (select);

  docs_count = mongodb_getvpdata (inst, request, inst->col_users, query,
                                  select, pair, NULL);

  bson_free (query);
  bson_free (select);

  return docs_count;
}

static int mongodb_group_check (MONGODB_INST *inst, REQUEST *request,
                                const char *groupname, VALUE_PAIR **pair)
{
  bson       *query  = NULL;
  bson       *select = NULL;
  int         docs_count = 0;

  if (!groupname || groupname[0] == '\0')
    return -1;

  query = bson_build (BSON_TYPE_STRING, "groupname", groupname, -1,
                      BSON_TYPE_NONE);
  bson_finish (query);

  select = bson_build (BSON_TYPE_INT32, "check", 1, BSON_TYPE_NONE);
  bson_finish (select);

  docs_count = mongodb_getvpdata (inst, request, inst->col_groups, query,
                                  select, pair, NULL);

  bson_free (query);
  bson_free (select);

  return docs_count;
}

static int mongodb_group_reply (MONGODB_INST *inst, REQUEST *request,
                                const char *groupname, VALUE_PAIR **pair)
{
  bson       *query  = NULL;
  bson       *select = NULL;
  int         docs_count = 0;

  if (!groupname || groupname[0] == '\0')
    return -1;

  query = bson_build (BSON_TYPE_STRING, "groupname", groupname, -1,
                      BSON_TYPE_NONE);
  bson_finish (query);

  select = bson_build (BSON_TYPE_INT32, "reply", 1, BSON_TYPE_NONE);
  bson_finish (select);

  docs_count = mongodb_getvpdata (inst, request, inst->col_groups, query,
                                  select, pair, NULL);

  bson_free (query);
  bson_free (select);

  return docs_count;
}

static int fallthrough (VALUE_PAIR *vp)
{
  VALUE_PAIR *ft = pairfind (vp, PW_FALL_THROUGH);

  return ft ? ft->vp_integer : 0;
}

static int mongodb_user_getgroups (MONGODB_INST *inst, REQUEST *request,
                                   VALUE_PAIR **pair)
{
  bson       *query  = NULL;
  bson       *select = NULL;
  const char *username   = request->username->vp_strvalue;
  int         docs_count = 0;

  query = bson_build (BSON_TYPE_STRING, "username", username, -1,
                      BSON_TYPE_NONE);
  bson_finish (query);

  select = bson_build (BSON_TYPE_INT32, "groups", 1, BSON_TYPE_NONE);
  bson_finish (select);

  docs_count = mongodb_getvpdata (inst, request, inst->col_users, query,
                                  select, pair, "Group");

  bson_free (query);
  bson_free (select);

  return docs_count;
}

static int mongodb_process_groups (MONGODB_INST *inst, REQUEST *request,
                                   int *dofallthrough)
{
  VALUE_PAIR *groups = NULL;
  int docs_count = mongodb_user_getgroups (inst, request, &groups);
  int found = 0;

  if (docs_count <= 0) {
    return docs_count;
  }

  VALUE_PAIR *it = NULL;

  for (it = groups; it != NULL && *dofallthrough; it = it->next) {
    VALUE_PAIR *check_items = NULL;
    VALUE_PAIR *reply_items = NULL;

    docs_count = mongodb_group_check (inst, request, it->vp_strvalue,
                                      &check_items);

    if (docs_count > 0) {
      if (paircompare (request, request->packet->vps, check_items,
            &request->reply->vps) == 0) {
        found = 1;
        RDEBUG2 ("User %s is in group %s", request->username->vp_strvalue,
                 it->vp_strvalue);

        docs_count = mongodb_group_reply (inst, request, it->vp_strvalue,
                                          &reply_items);

        *dofallthrough = fallthrough (reply_items);
        pairmove (&request->config_items, &check_items);
        pairmove (&request->reply->vps,   &reply_items);
      }
    } else if (docs_count < 0) {
      radlog_request (L_ERR, 0, request,
                      "Error retrieving check pairs for group %s",
                      it->vp_strvalue);
      found = -1;
      break;
    }

    pairfree (&check_items);
    pairfree (&reply_items);
  }

  pairfree (&groups);
  return found;
}

static int mongodb_authorize (void *instance, REQUEST *request)
{
  VALUE_PAIR *check_items = NULL;
  VALUE_PAIR *reply_items = NULL;
  int ret           = RLM_MODULE_NOTFOUND;
  int dofallthrough = 1;
  int docs_count    = 0;

  if (request->username == NULL)
    return RLM_MODULE_NOOP;

  MONGODB_INST *inst = instance;

  docs_count = mongodb_user_check (inst, request, &check_items);

  RDEBUG ("Found %d documents in user check collection", docs_count);
  if (docs_count > 0 && paircompare (request, request->packet->vps,
        check_items, &request->reply->vps) == 0) {
    pairmove (&request->config_items, &check_items);
    ret = RLM_MODULE_OK;

    docs_count = mongodb_user_reply (inst, request, &reply_items);

    RDEBUG ("Found %d documents in user reply collection", docs_count);
    if (docs_count > 0) {
      if (!inst->read_groups)
        dofallthrough = fallthrough (reply_items);

      pairmove (&request->reply->vps, &reply_items);
      ret = RLM_MODULE_OK;
    }
  } else if (docs_count == 0) {
    goto skipreply;
  } else {
    goto error;
  }

skipreply:
  pairfree (&check_items);
  pairfree (&reply_items);

  if (dofallthrough) {
    docs_count = mongodb_process_groups (inst, request, &dofallthrough);

    if (docs_count < 0) {
      radlog_request (L_ERR, 0, request, "Error processing groups; "
                      "rejecting user");
      goto error;
    }
  }

  goto out;

error:
  ret = RLM_MODULE_FAIL;

out:
  pairfree (&check_items);
  pairfree (&reply_items);

  return ret;
}

static int mongodb_accounting_update (MONGODB_INST *inst, REQUEST *request,
                                      bson *select, bson *update,
                                      gint32 flags)
{
  mongo_sync_pool_connection *conn = NULL;
  gboolean done   = FALSE;
  int      ret    = RLM_MODULE_FAIL;

  if (!update || !select) {
    radlog_request (L_ERR, 0, request,
                    "Invalid data for accounting update");
    return RLM_MODULE_FAIL;
  }

  conn = mongodb_get_conn (inst);
  if (!conn) {
    radlog_request (L_ERR, 0, request, "Maximum %d connections exceeded; "
                    "ignore accounting update", inst->numconns);
    return RLM_MODULE_FAIL;
  }

  done = mongo_sync_cmd_update ((mongo_sync_connection *) conn,
                                 inst->col_acct, flags, select, update);

  if (done == TRUE) {
    ret = RLM_MODULE_OK;
  } else {
    gchar *error = NULL;
    mongo_sync_cmd_get_last_error ((mongo_sync_connection *) conn,
                                   inst->database, &error);
    radlog_request (L_ERR, 0, request,
                    "MongoDB accounting update error; [%s]", error);
    mongo_sync_cmd_reset_error ((mongo_sync_connection *) conn,
                                inst->database);
    ret = RLM_MODULE_FAIL;
  }

  if (mongodb_return_conn (inst, conn) == -1) {
    radlog_request (L_ERR, 0, request,
                    "The connection was not returned to the pool");
    return RLM_MODULE_FAIL;
  }

  return ret;
}

static int64_t _get_request_timestamp (REQUEST *request)
{
  VALUE_PAIR *acct_delay_time = pairfind (request->packet->vps,
                                          PW_ACCT_DELAY_TIME);
  struct timeval tval;
  int64_t timestamp = 0;
  int     delay     = 0;

  gettimeofday (&tval, NULL);
  timestamp = tval.tv_sec * 1000 + tval.tv_usec / 1000; /* milliseconds */

  if (acct_delay_time) {
    delay = acct_delay_time->vp_integer * 1000; /* milliseconds */
  }

  timestamp -= delay;
  return timestamp;
}

static int mongodb_accounting_start (MONGODB_INST *inst, REQUEST *request)
{
  VALUE_PAIR *vp     = request->packet->vps;
  const char *attr   = NULL;
  bson       *select = NULL;
  bson       *update = NULL;
  bson       *updset = NULL;
  int         ret    = 0;
  char        value[MAX_STRING_LEN + 1];

  VALUE_PAIR *username = pairfind (request->packet->vps, PW_USER_NAME);
  VALUE_PAIR *sessid  = pairfind (request->packet->vps, PW_ACCT_SESSION_ID);
  VALUE_PAIR *nasip   = pairfind (request->packet->vps, PW_NAS_IP_ADDRESS);
  VALUE_PAIR *nasport = pairfind (request->packet->vps, PW_NAS_PORT);

  if (!username || !sessid || !nasip || !nasport) {
    radlog_request (L_ERR, 0, request, "packet has no User-Name and/or "
                    "no Acct-Session-ID");
    return RLM_MODULE_INVALID;
  }

  select = bson_new ();
  bson_append_string (select, username->name, username->vp_strvalue, -1);
  bson_append_string (select, sessid->name, sessid->vp_strvalue, -1);
  vp_prints_value (value, sizeof (value), nasip, 0);
  bson_append_string (select, nasip->name, value, -1);
  bson_append_int32 (select, nasport->name, nasport->vp_integer);
  bson_append_null (select, "Acct-Session-Stop-Time");
  bson_finish (select);

  update = bson_new ();

  VALUE_PAIR *it = vp;
  while (it) {
    attr  = it->name;

    switch (it->type) {
      case PW_TYPE_INTEGER:
      case PW_TYPE_BYTE:
      case PW_TYPE_SHORT:
        bson_append_int32 (update, attr, it->vp_integer);
        break;
      case PW_TYPE_DATE:
        bson_append_utc_datetime (update, attr, it->vp_date);
        break;
      default:
        vp_prints_value (value, sizeof (value), it, 0);
        bson_append_string (update, attr, value, -1);
        break;
    }

    it = it->next;
  }

  int64_t timestamp = _get_request_timestamp (request);
  bson_append_utc_datetime (update, "Acct-Session-Start-Time", timestamp);
  bson_finish (update);

  updset = bson_new ();
  bson_append_document (updset, "$set", update);
  bson_finish (updset);

  ret = mongodb_accounting_update (inst, request, select, updset,
                                   MONGO_WIRE_FLAG_UPDATE_UPSERT);

  bson_free (updset);
  bson_free (update);
  bson_free (select);

  return ret;
}

static int mongodb_accounting_prestop (MONGODB_INST *inst,
                                       REQUEST *request, bson *query,
                                       int64_t *starttime,
                                       int64_t *stoptime,
                                       int64_t *sesstime)
{
  int     found         = 0;
  int64_t doc_starttime = 0;
  mongo_sync_pool_connection *conn = mongodb_get_conn (inst);
  mongo_sync_cursor *sc     = NULL;
  mongo_packet      *p      = NULL;
  bson              *select = NULL;

  if (!conn) {
    radlog_request (L_ERR, 0, request, "Maximum %d connections exceeded; "
                    "rejecting user", inst->numconns);
    return -1;
  }

  select = bson_build (BSON_TYPE_INT32, "Acct-Session-Start-Time", 1,
                       BSON_TYPE_NONE);
  bson_finish (select);

  p = mongo_sync_cmd_query ((mongo_sync_connection *) conn, inst->col_acct,
                            0, 0, 1, query, select);

  if (!p) {
    found = 0;
    goto out;
  }

  sc = mongo_sync_cursor_new ((mongo_sync_connection *) conn,
                              inst->col_acct, p);

  if (!sc) {
    radlog_request (L_ERR, 0, request, "Error create new cursor");
    found = -1;
    mongo_wire_packet_free (p);
    goto out;
  }

  mongo_sync_cursor_next (sc);
  bson *result = mongo_sync_cursor_get_data (sc);

  if (!result) {
    radlog_request (L_ERR, 0, request, "Could not get result");
    found = -1;
    goto out_cursor;
  }

  bson_cursor *doc = bson_find (result, "Acct-Session-Start-Time");
  if (bson_cursor_type (doc) == BSON_TYPE_UTC_DATETIME) {
    bson_cursor_get_utc_datetime (doc, &doc_starttime);
    found = 1;
  }
  bson_cursor_free (doc);

out_cursor:
  bson_free (result);
  mongo_sync_cursor_free (sc);

out:
  if (*sesstime == 0) {
    *stoptime = _get_request_timestamp (request);
    *sesstime = (*stoptime - doc_starttime) / 1000; /* seconds */
    *starttime = doc_starttime;
  } else {
    if (doc_starttime > 0) {
      *starttime = doc_starttime;
      *stoptime  = doc_starttime + (*sesstime * 1000);
    } else {
      *stoptime = _get_request_timestamp (request);
      *starttime = *stoptime - (*sesstime * 1000);
    }
  }

  bson_free (select);

  if (mongodb_return_conn (inst, conn) == -1) {
    radlog_request (L_ERR, 0, request,
                    "The connection was not returned to the pool");
    return -1;
  }

  return found;
}

static int mongodb_accounting_stop (MONGODB_INST *inst, REQUEST *request)
{
  bson       *select = NULL;
  bson       *update = NULL;
  bson       *updset = NULL;
  int         ret    = 0;
  char        value[MAX_STRING_LEN + 1];

  VALUE_PAIR *username = pairfind (request->packet->vps, PW_USER_NAME);
  VALUE_PAIR *sessid  = pairfind (request->packet->vps, PW_ACCT_SESSION_ID);
  VALUE_PAIR *nasip   = pairfind (request->packet->vps, PW_NAS_IP_ADDRESS);
  VALUE_PAIR *nasport = pairfind (request->packet->vps, PW_NAS_PORT);
  VALUE_PAIR *sstime = pairfind (request->packet->vps, PW_ACCT_SESSION_TIME);
  VALUE_PAIR *tcause = pairfind (request->packet->vps, PW_ACCT_TERMINATE_CAUSE);

  if (!username || !sessid || !nasip || !nasport) {
    radlog_request (L_ERR, 0, request, "packet has no some of required data "
                    "User-Name, Acct-Session-Id, NAS-IP-Address, NAS-Port");
    return RLM_MODULE_INVALID;
  }

  select = bson_new ();
  update = bson_new ();
  updset = bson_new ();

  bson_append_string (select, username->name, username->vp_strvalue, -1);
  bson_append_string (select, sessid->name, sessid->vp_strvalue, -1);
  vp_prints_value (value, sizeof (value), nasip, 0);
  bson_append_string (select, nasip->name, value, -1);
  bson_append_int32 (select, nasport->name, nasport->vp_integer);
  bson_append_null (select, "Acct-Session-Stop-Time");
  bson_finish (select);

  int64_t starttime = 0;
  int64_t stoptime  = 0;
  int64_t sesstime  = 0;
  int     found     = 0;

  if (sstime) {
    sesstime = sstime->vp_date;
  }

  found = mongodb_accounting_prestop (inst, request, select, &starttime,
                                      &stoptime, &sesstime);

  if (found < 0 || (found == 0 && sesstime == 0)) {
    radlog_request (L_ERR, 0, request, "Could not update accounting (stop), "
                    "no start record and could not re-generate "
                    "or fail to get data");
    ret = RLM_MODULE_FAIL;
    goto out;
  } else if (found == 0) {
    /* no start record, just add one and update with calculated
       starttime = stoptime - sessiontime */
    ret = mongodb_accounting_start (inst, request);

    if (ret != RLM_MODULE_OK) {
      goto out;
    }
  }

  bson_append_utc_datetime (update, "Acct-Session-Start-Time", starttime);
  bson_append_utc_datetime (update, "Acct-Session-Stop-Time", stoptime);

  if (tcause) {
    bson_append_int32 (update, tcause->name, tcause->vp_integer);
  }

  bson_append_int32 (update, "Acct-Status-Type", PW_STATUS_STOP);
  bson_append_int64 (update, "Acct-Session-Time", sesstime);
  bson_finish (update);

  bson_append_document (updset, "$set", update);
  bson_finish (updset);

  ret = mongodb_accounting_update (inst, request, select, updset, 0);

out:
  bson_free (updset);
  bson_free (update);
  bson_free (select);

  return ret;
}

static int mongodb_accounting (void *instance, REQUEST *request) {
  MONGODB_INST *inst  = instance;
  int  acctstatustype = 0;
  int  ret            = RLM_MODULE_NOOP;
  char logstr[MAX_STRING_LEN];

  VALUE_PAIR *pair = pairfind (request->packet->vps, PW_ACCT_STATUS_TYPE);
  if (pair) {
    acctstatustype = pair->vp_integer;
  } else {
    radius_xlat (logstr, sizeof (logstr), "packet has no accounting status"
                 "type. [user '%{User-Name}', nas '%{NAS-IP-Address}']",
                 request, NULL);
    radlog_request (L_ERR, 0, request, "%s", logstr);
    return RLM_MODULE_INVALID;
  }

  switch (acctstatustype) {
    case PW_STATUS_START:
      ret = mongodb_accounting_start (inst, request);
      break;
    case PW_STATUS_STOP:
      ret = mongodb_accounting_stop (inst, request);
      break;
    case PW_STATUS_ALIVE:
      break;
    case PW_STATUS_ACCOUNTING_ON:
    case PW_STATUS_ACCOUNTING_OFF:
      break;
    default:
      RDEBUG ("Unsupported Acct-Status-Type = %d", acctstatustype);
      return RLM_MODULE_NOOP;
  }

  return ret;
}

static uint32_t mongodb_cursor_get_addr (bson_cursor *cs)
{
  const gchar *addr_s = NULL;

  if (!cs)
    return 0;

  bson_cursor_get_string (cs, &addr_s);
  return inet_addr (addr_s);
}

static int mongodb_cursor_get_port (bson_cursor *cs)
{
  int port = 0;

  if (!cs)
    return 0;

  bson_cursor_get_int32 (cs, &port);
  return port;
}

static int mongodb_session_check (REQUEST *request, MONGODB_INST *inst,
                                  const bson *session)
{
  bson_cursor *cs_username    = bson_find (session, "User-Name");
  bson_cursor *cs_sessid      = bson_find (session, "Acct-Session-Id");
  bson_cursor *cs_nas_addr    = bson_find (session, "NAS-IP-Address");
  bson_cursor *cs_nas_port    = bson_find (session, "NAS-Port");
  bson_cursor *cs_framed_addr = bson_find (session, "Framed-IP-Address");
  int          ret            = 0;

  if (!cs_username || !cs_sessid) {
    RDEBUG ("Could not zap stale entry. No username or session id present in"
            " entry.");
    ret = 1;
    goto out;
  }

  const gchar *username = NULL;
  const gchar *sessid   = NULL;

  if (bson_cursor_get_string (cs_username, &username) == FALSE ||
      bson_cursor_get_string (cs_sessid, &sessid) == FALSE) {
    RDEBUG ("Could not zap stale entry. Could not get username or session id.");
    ret = 1;
    goto out;
  }

  uint32_t nas_addr    = mongodb_cursor_get_addr (cs_nas_addr);
  int      nas_port    = mongodb_cursor_get_port (cs_nas_port);
  uint32_t framed_addr = mongodb_cursor_get_addr (cs_framed_addr);

  int check = rad_check_ts (nas_addr, nas_port, username, sessid);

  switch (check) {
    case 0:
      if (inst->zap_stale_sessions) {
        char         proto        = 0;
        int64_t      sess_time    = 0;
        bson_cursor *cs_sess_time = bson_find (session, "Acct-Session-Time");

        if (cs_sess_time)
          bson_cursor_get_int64 (cs_sess_time, &sess_time);

        session_zap (request, nas_addr, nas_port, username, sessid, framed_addr,
                     proto, sess_time);

        bson_cursor_free (cs_sess_time);
      }
      break;

    case 1:
      {
        VALUE_PAIR  *vp          = NULL;
        uint32_t     req_ip      = 0;
        char        *req_callnum = NULL;
        const gchar *callnum     = NULL;
        bson_cursor *cs_callnum  = bson_find (session, "Calling-Station-Id");

        ++request->simul_count;
        vp = pairfind (request->packet->vps, PW_FRAMED_IP_ADDRESS);
        if (vp)
          req_ip = vp->vp_ipaddr;

        vp = pairfind (request->packet->vps, PW_CALLING_STATION_ID);
        if (vp)
          req_callnum = vp->vp_strvalue;

        if (cs_callnum)
          bson_cursor_get_string (cs_callnum, &callnum);

        if (framed_addr && req_ip && framed_addr == req_ip) {
          request->simul_mpp = 2;
        } else if (callnum && req_callnum &&
                   strncmp (callnum, req_callnum, 16) != 0) {
          request->simul_mpp = 2;
        }

        bson_cursor_free (cs_callnum);
      }
      break;

    default:
      radlog_request (L_ERR, 0, request, "Failed to check the terminal server "
                      "for user '%s'.", username);
      ret = 1;
  }

out:
  bson_cursor_free (cs_username);
  bson_cursor_free (cs_sessid);
  bson_cursor_free (cs_nas_addr);
  bson_cursor_free (cs_nas_port);
  bson_cursor_free (cs_framed_addr);

  return ret;
}

static int mongodb_session_verify (mongo_sync_connection *conn,
                                   MONGODB_INST *inst, REQUEST *request)
{
  int                ret    = RLM_MODULE_OK;
  bson              *query  = NULL;
  mongo_packet      *p      = NULL;
  mongo_sync_cursor *sc     = NULL;

  query = bson_new ();
  bson_append_string (query, request->username->name,
                      request->username->vp_strvalue, -1);
  bson_append_null (query, "Acct-Session-Stop-Time");
  bson_finish (query);

  p = mongo_sync_cmd_query (conn, inst->col_acct, 0, 0, INT_MAX, query, NULL);

  if (!p) {
    ret = RLM_MODULE_FAIL;
    goto out;
  }

  sc = mongo_sync_cursor_new (conn, inst->col_acct, p);
  if (!sc) {
    radlog_request (L_ERR, 0, request, "Error create new cursor");
    ret = RLM_MODULE_FAIL;
    mongo_wire_packet_free (p);
    goto out;
  }

  int fail = 0;

  request->simul_count = 0;

  while (mongo_sync_cursor_next (sc) && !fail) {
    bson *result = mongo_sync_cursor_get_data (sc);

    if (result) {
      fail = mongodb_session_check (request, inst, result);

      if (fail)
        ret = RLM_MODULE_FAIL;
    }

    bson_free (result);
  }

  mongo_sync_cursor_free (sc);

out:
  bson_free (query);

  return ret;
}

static int mongodb_checksimul (void *instance, REQUEST *request)
{
  MONGODB_INST *inst = instance;
  int ret = RLM_MODULE_NOOP;

  if ((request->username == NULL) || (request->username->length == 0)) {
    radlog_request (L_ERR, 0, request, "Zero length username not permitted");
    return RLM_MODULE_INVALID;
  }

  mongo_sync_pool_connection *conn = mongodb_get_conn (inst);
  bson    *query   = NULL;
  gdouble  count   = 0;

  if (!conn) {
    radlog_request (L_ERR, 0, request, "Maximum %d connections exceeded; "
                    "could not check simultaneous-use", inst->numconns);
    return RLM_MODULE_FAIL;
  }

  query = bson_new ();
  bson_append_string (query, request->username->name,
                      request->username->vp_strvalue, -1);
  bson_append_null (query, "Acct-Session-Stop-Time");
  bson_finish (query);

  count = mongo_sync_cmd_count ((mongo_sync_connection *) conn, inst->database,
                                "acct", query);
  bson_free (query);

  if (count < 0) {
    radlog_request (L_ERR, 0, request, "Could not get data; "
                    "no simultaneous-use check");
    ret = RLM_MODULE_FAIL;
    goto out;
  }

  RDEBUG ("Simultaneous count/max: %0.0f/%d", count, request->simul_max);
  request->simul_count = (int) count;

  if (request->simul_count >= request->simul_max && inst->verify_session) {
    ret = mongodb_session_verify ((mongo_sync_connection *)conn, inst, request);
  } else {
    ret = RLM_MODULE_OK;
  }

out:
  if (mongodb_return_conn (inst, conn) == -1) {
    radlog_request (L_ERR, 0, request,
                    "The connection was not returned to the pool;");
    return RLM_MODULE_FAIL;
  }

  return ret;
}

module_t rlm_mongodb = {
  RLM_MODULE_INIT,
  "mongodb",
  RLM_TYPE_THREAD_SAFE, /* type */
  mongodb_instantiate, /* instantiation */
  mongodb_detach, /* detach */
  {
    NULL, /* authentication */
    mongodb_authorize, /* authorization */
    NULL, /* preaccounting */
    mongodb_accounting, /* accounting */
    mongodb_checksimul, /* checksimul */
    NULL, /* pre-proxy */
    NULL, /* post-proxy */
    NULL  /* post-auth */
  }
};
