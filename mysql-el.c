/* mysql-el: Support for accessing MySQL databases via Emacs dynamic module.
   API modeled after the built-in sqlite.c interface.

   Connection objects use struct mysql_conn (wrapped in user-ptr) which
   tracks async state so that the same handle works for both sync and
   async operations.  */

#include <emacs-module.h>
#include <mysql.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* MySQL nonblocking API (requires MySQL 8.0.16+).  */
#include <mysql/plugin_auth_common.h>

/* MySQL 8.0+ removed my_bool; define it for compatibility.  */
#ifndef my_bool
# define my_bool bool
#endif

int plugin_is_GPL_compatible;

/* ================================================================== */
/*  Connection wrapper                                                */
/* ================================================================== */

enum mysql_phase {
  PHASE_IDLE,         /* ready for new operations */
  PHASE_CONNECTING,   /* async connect in progress */
  PHASE_QUERY,        /* async query in progress */
  PHASE_STORE,        /* async store-result in progress */
};

struct mysql_conn {
  MYSQL *conn;
  enum mysql_phase phase;
  MYSQL_RES *pending_res;   /* store-result completed, awaiting conversion */
  /* Async connect: libmysqlclient holds pointers to these strings
     until mysql_real_connect_nonblocking completes, so we must keep
     them alive.  Freed in poll-complete, poll-error, close, or finalizer. */
  char *connect_host;
  char *connect_user;
  char *connect_password;
  char *connect_database;
};

/* Extract struct mysql_conn* from a user-ptr arg.
   Returns NULL and signals error if invalid.  */
static struct mysql_conn *
get_conn (emacs_env *env, emacs_value arg);

/* Check that connection is idle (not in the middle of async op).
   Signals error and returns false if busy.  */
static bool
check_idle (emacs_env *env, struct mysql_conn *mc);

/* ================================================================== */
/*  Helper: signal errors                                             */
/* ================================================================== */

static void
signal_error (emacs_env *env, const char *msg)
{
  emacs_value Qerror = env->intern (env, "error");
  emacs_value data = env->make_string (env, msg, strlen (msg));
  env->non_local_exit_signal (env, Qerror, data);
}

/* Signal `mysql-error' with structured data: (ERRNO SQLSTATE ERRMSG). */
static void
signal_mysql_error (emacs_env *env, MYSQL *conn)
{
  unsigned int eno = mysql_errno (conn);
  const char *state = mysql_sqlstate (conn);
  const char *err = mysql_error (conn);

  emacs_value Qmysql_error = env->intern (env, "mysql-error");
  emacs_value Qlist = env->intern (env, "list");

  emacs_value errno_val = env->make_integer (env, (intmax_t) eno);
  emacs_value state_val = env->make_string (env, state, strlen (state));
  emacs_value msg_val   = env->make_string (env, err, strlen (err));

  emacs_value list_args[] = { errno_val, state_val, msg_val };
  emacs_value data = env->funcall (env, Qlist, 3, list_args);

  env->non_local_exit_signal (env, Qmysql_error, data);
}

/* Signal `mysql-error' for a prepared statement. */
static void
signal_mysql_stmt_error (emacs_env *env, MYSQL_STMT *stmt)
{
  unsigned int eno = mysql_stmt_errno (stmt);
  const char *state = mysql_stmt_sqlstate (stmt);
  const char *err = mysql_stmt_error (stmt);

  emacs_value Qmysql_error = env->intern (env, "mysql-error");
  emacs_value Qlist = env->intern (env, "list");

  emacs_value errno_val = env->make_integer (env, (intmax_t) eno);
  emacs_value state_val = env->make_string (env, state, strlen (state));
  emacs_value msg_val   = env->make_string (env, err, strlen (err));

  emacs_value list_args[] = { errno_val, state_val, msg_val };
  emacs_value data = env->funcall (env, Qlist, 3, list_args);

  env->non_local_exit_signal (env, Qmysql_error, data);
}

/* ================================================================== */
/*  Helper: Emacs Lisp utilities                                      */
/* ================================================================== */

static void
bind_function (emacs_env *env, const char *name, emacs_value Sfun)
{
  emacs_value Qfset = env->intern (env, "fset");
  emacs_value Qsym = env->intern (env, name);
  emacs_value args[] = { Qsym, Sfun };
  env->funcall (env, Qfset, 2, args);
}

static void
provide (emacs_env *env, const char *feature)
{
  emacs_value Qfeat = env->intern (env, feature);
  emacs_value Qprovide = env->intern (env, "provide");
  emacs_value args[] = { Qfeat };
  env->funcall (env, Qprovide, 1, args);
}

static char *
extract_string (emacs_env *env, emacs_value val)
{
  ptrdiff_t len = 0;
  env->copy_string_contents (env, val, NULL, &len);
  char *buf = malloc (len);
  if (!buf)
    {
      signal_error (env, "malloc failed");
      return NULL;
    }
  env->copy_string_contents (env, val, buf, &len);
  return buf;
}

static bool
is_nil (emacs_env *env, emacs_value val)
{
  return !env->is_not_nil (env, val);
}

/* ================================================================== */
/*  Connection object: finalizer, get_conn, check_idle                */
/* ================================================================== */

static void
free_connect_strings (struct mysql_conn *mc)
{
  free (mc->connect_host);     mc->connect_host = NULL;
  free (mc->connect_user);     mc->connect_user = NULL;
  free (mc->connect_password); mc->connect_password = NULL;
  free (mc->connect_database); mc->connect_database = NULL;
}

static void
mysql_conn_finalizer (void *ptr)
{
  if (!ptr) return;
  struct mysql_conn *mc = (struct mysql_conn *) ptr;
  if (mc->pending_res)
    mysql_free_result (mc->pending_res);
  free_connect_strings (mc);
  if (mc->conn)
    mysql_close (mc->conn);
  free (mc);
}

static struct mysql_conn *
get_conn (emacs_env *env, emacs_value arg)
{
  struct mysql_conn *mc = env->get_user_ptr (env, arg);
  if (!mc || !mc->conn)
    {
      signal_error (env, "Invalid or closed database object");
      return NULL;
    }
  return mc;
}

static bool
check_idle (emacs_env *env, struct mysql_conn *mc)
{
  if (mc->phase != PHASE_IDLE)
    {
      signal_error (env, "Connection busy (async operation in progress)");
      return false;
    }
  return true;
}

/* ================================================================== */
/*  mysql_set: cursor/set object for lazy row-by-row iteration        */
/* ================================================================== */

struct mysql_set {
  MYSQL *conn;
  MYSQL_RES *res;
  MYSQL_STMT *stmt;
  MYSQL_RES *meta;
  MYSQL_BIND *res_binds;
  char **col_bufs;
  unsigned long *col_lens;
  my_bool *col_nulls;
  unsigned int ncols;
  bool eof;
  bool is_prepared;
};

static void
mysql_set_finalizer (void *ptr)
{
  if (!ptr) return;
  struct mysql_set *set = (struct mysql_set *) ptr;

  if (set->is_prepared)
    {
      if (set->res_binds)
        {
          for (unsigned int c = 0; c < set->ncols; c++)
            free (set->col_bufs[c]);
          free (set->col_bufs);
          free (set->col_lens);
          free (set->col_nulls);
          free (set->res_binds);
        }
      if (set->meta)
        mysql_free_result (set->meta);
      if (set->stmt)
        mysql_stmt_close (set->stmt);
    }
  else
    {
      if (set->res)
        mysql_free_result (set->res);
    }
  free (set);
}

/* ================================================================== */
/*  Internal: row/column helpers                                      */
/* ================================================================== */

static emacs_value
row_to_list (emacs_env *env, MYSQL_RES *res, MYSQL_ROW row)
{
  unsigned int ncols = mysql_num_fields (res);
  unsigned long *lengths = mysql_fetch_lengths (res);
  MYSQL_FIELD *fields = mysql_fetch_fields (res);

  emacs_value Qcons = env->intern (env, "cons");
  emacs_value list = env->intern (env, "nil");

  for (int i = (int)ncols - 1; i >= 0; i--)
    {
      emacs_value v;
      if (row[i] == NULL)
        {
          v = env->intern (env, "nil");
        }
      else
        {
          switch (fields[i].type)
            {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_YEAR:
              {
                long long val = strtoll (row[i], NULL, 10);
                v = env->make_integer (env, val);
              }
              break;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
              {
                double val = strtod (row[i], NULL);
                v = env->make_float (env, val);
              }
              break;
            default:
              v = env->make_string (env, row[i], lengths[i]);
              break;
            }
        }
      emacs_value cons_args[] = { v, list };
      list = env->funcall (env, Qcons, 2, cons_args);
    }

  return list;
}

static emacs_value
column_names (emacs_env *env, MYSQL_RES *res)
{
  unsigned int ncols = mysql_num_fields (res);
  MYSQL_FIELD *fields = mysql_fetch_fields (res);

  emacs_value Qcons = env->intern (env, "cons");
  emacs_value list = env->intern (env, "nil");

  for (int i = (int)ncols - 1; i >= 0; i--)
    {
      emacs_value name = env->make_string (env, fields[i].name,
                                           strlen (fields[i].name));
      emacs_value cons_args[] = { name, list };
      list = env->funcall (env, Qcons, 2, cons_args);
    }
  return list;
}

/* ================================================================== */
/*  Internal: build result plist from MYSQL_RES (shared by sync/async)*/
/* ================================================================== */

/* Build (:type select :columns (...) :rows (...) :warning-count N)
   or    (:type dml :affected-rows N :warning-count N).
   If res is non-NULL, it is a SELECT result and will be freed.
   If res is NULL, it is a DML and affected-rows is read from conn.  */
static emacs_value
build_result_plist (emacs_env *env, struct mysql_conn *mc, MYSQL_RES *res)
{
  unsigned int wc = mysql_warning_count (mc->conn);
  emacs_value Qlist = env->intern (env, "list");

  if (res)
    {
      /* SELECT result */
      emacs_value cols = column_names (env, res);

      emacs_value Qcons = env->intern (env, "cons");
      emacs_value Qnreverse = env->intern (env, "nreverse");
      emacs_value rows = env->intern (env, "nil");
      MYSQL_ROW row;
      while ((row = mysql_fetch_row (res)))
        {
          emacs_value r = row_to_list (env, res, row);
          emacs_value ra[] = { r, rows };
          rows = env->funcall (env, Qcons, 2, ra);
        }
      rows = env->funcall (env, Qnreverse, 1, &rows);

      mysql_free_result (res);

      emacs_value args[] = {
        env->intern (env, ":type"),    env->intern (env, "select"),
        env->intern (env, ":columns"), cols,
        env->intern (env, ":rows"),    rows,
        env->intern (env, ":warning-count"), env->make_integer (env, wc)
      };
      return env->funcall (env, Qlist, 8, args);
    }
  else
    {
      /* DML result */
      my_ulonglong affected = mysql_affected_rows (mc->conn);
      emacs_value args[] = {
        env->intern (env, ":type"),          env->intern (env, "dml"),
        env->intern (env, ":affected-rows"), env->make_integer (env, (intmax_t) affected),
        env->intern (env, ":warning-count"), env->make_integer (env, wc)
      };
      return env->funcall (env, Qlist, 6, args);
    }
}

/* ================================================================== */
/*  Internal: bind parameters (for prepared statements)               */
/* ================================================================== */

struct bind_buf {
  MYSQL_BIND *binds;
  char **str_bufs;
  unsigned long *lengths;
  int count;
};

static void
free_bind_buf (struct bind_buf *b)
{
  if (!b) return;
  for (int i = 0; i < b->count; i++)
    free (b->str_bufs[i]);
  free (b->str_bufs);
  free (b->lengths);
  free (b->binds);
}

static bool
build_bind_params (emacs_env *env, emacs_value values, struct bind_buf *out)
{
  emacs_value Qlength = env->intern (env, "length");
  emacs_value vlen = env->funcall (env, Qlength, 1, &values);
  int count = (int) env->extract_integer (env, vlen);

  out->count = count;
  out->binds = calloc (count, sizeof (MYSQL_BIND));
  out->str_bufs = calloc (count, sizeof (char *));
  out->lengths = calloc (count, sizeof (unsigned long));
  if (!out->binds || !out->str_bufs || !out->lengths)
    {
      signal_error (env, "malloc failed");
      free_bind_buf (out);
      return false;
    }

  emacs_value Qnth = env->intern (env, "nth");
  emacs_value Qintegerp = env->intern (env, "integerp");
  emacs_value Qfloatp = env->intern (env, "floatp");
  emacs_value Qstringp = env->intern (env, "stringp");

  for (int i = 0; i < count; i++)
    {
      emacs_value idx = env->make_integer (env, i);
      emacs_value nth_args[] = { idx, values };
      emacs_value val = env->funcall (env, Qnth, 2, nth_args);

      if (is_nil (env, val))
        {
          out->binds[i].buffer_type = MYSQL_TYPE_NULL;
        }
      else if (env->is_not_nil (env, env->funcall (env, Qintegerp, 1, &val)))
        {
          long long *p = malloc (sizeof (long long));
          if (!p) { signal_error (env, "malloc failed"); free_bind_buf (out); return false; }
          *p = (long long) env->extract_integer (env, val);
          out->binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
          out->binds[i].buffer = p;
          out->str_bufs[i] = (char *) p;
        }
      else if (env->is_not_nil (env, env->funcall (env, Qfloatp, 1, &val)))
        {
          double *p = malloc (sizeof (double));
          if (!p) { signal_error (env, "malloc failed"); free_bind_buf (out); return false; }
          *p = env->extract_float (env, val);
          out->binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
          out->binds[i].buffer = p;
          out->str_bufs[i] = (char *) p;
        }
      else if (env->is_not_nil (env, env->funcall (env, Qstringp, 1, &val)))
        {
          char *s = extract_string (env, val);
          if (!s) { free_bind_buf (out); return false; }
          out->lengths[i] = strlen (s);
          out->binds[i].buffer_type = MYSQL_TYPE_STRING;
          out->binds[i].buffer = s;
          out->binds[i].buffer_length = out->lengths[i];
          out->binds[i].length = &out->lengths[i];
          out->str_bufs[i] = s;
        }
      else
        {
          signal_error (env, "Unsupported value type for bind parameter");
          free_bind_buf (out);
          return false;
        }
    }
  return true;
}

/* ================================================================== */
/*  mysql-open: sync & async connect                                  */
/* ================================================================== */

static emacs_value
Fmysql_open (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
             void *data)
{
  /* (mysql-open HOST USER PASSWORD &optional DATABASE PORT ASYNC) */
  char *host = extract_string (env, args[0]);
  if (!host) return env->intern (env, "nil");
  char *user = extract_string (env, args[1]);
  if (!user) { free (host); return env->intern (env, "nil"); }
  char *password = extract_string (env, args[2]);
  if (!password) { free (host); free (user); return env->intern (env, "nil"); }

  char *database = NULL;
  unsigned int port = 0;
  bool async = false;

  if (nargs > 3 && env->is_not_nil (env, args[3]))
    database = extract_string (env, args[3]);
  if (nargs > 4 && env->is_not_nil (env, args[4]))
    port = (unsigned int) env->extract_integer (env, args[4]);
  if (nargs > 5 && env->is_not_nil (env, args[5]))
    async = true;

  MYSQL *conn = mysql_init (NULL);
  if (!conn)
    {
      signal_error (env, "mysql_init failed");
      free (host); free (user); free (password); free (database);
      return env->intern (env, "nil");
    }

  struct mysql_conn *mc = calloc (1, sizeof *mc);
  if (!mc)
    {
      signal_error (env, "malloc failed");
      mysql_close (conn);
      free (host); free (user); free (password); free (database);
      return env->intern (env, "nil");
    }
  mc->conn = conn;

  if (!async)
    {
      /* Synchronous connect */
      if (!mysql_real_connect (conn, host, user, password, database, port,
                               NULL, CLIENT_MULTI_STATEMENTS))
        {
          signal_mysql_error (env, conn);
          mysql_close (conn);
          free (mc);
          free (host); free (user); free (password); free (database);
          return env->intern (env, "nil");
        }
      mysql_set_character_set (conn, "utf8mb4");
      mc->phase = PHASE_IDLE;
      free (host); free (user); free (password); free (database);
    }
  else
    {
      /* Asynchronous connect — libmysqlclient may hold pointers to
         host/user/password/database until the handshake completes,
         so we must keep them alive in the struct.  */
      mc->connect_host     = host;
      mc->connect_user     = user;
      mc->connect_password = password;
      mc->connect_database = database;

      enum net_async_status st =
        mysql_real_connect_nonblocking (conn, host, user, password,
                                        database, port, NULL,
                                        CLIENT_MULTI_STATEMENTS);
      if (st == NET_ASYNC_ERROR)
        {
          signal_mysql_error (env, conn);
          mysql_close (conn);
          free_connect_strings (mc);
          free (mc);
          return env->intern (env, "nil");
        }
      if (st == NET_ASYNC_COMPLETE)
        {
          mysql_set_character_set (conn, "utf8mb4");
          mc->phase = PHASE_IDLE;
          free_connect_strings (mc);
        }
      else
        mc->phase = PHASE_CONNECTING;
    }

  return env->make_user_ptr (env, mysql_conn_finalizer, mc);
}

/* ================================================================== */
/*  mysql-open-poll                                                   */
/* ================================================================== */

static emacs_value
Fmysql_open_poll (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                  void *data)
{
  struct mysql_conn *mc = env->get_user_ptr (env, args[0]);
  if (!mc || !mc->conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }
  if (mc->phase == PHASE_IDLE)
    return env->intern (env, "complete");
  if (mc->phase != PHASE_CONNECTING)
    {
      signal_error (env, "No async connect in progress");
      return env->intern (env, "nil");
    }

  enum net_async_status st =
    mysql_real_connect_nonblocking (mc->conn, NULL, NULL, NULL,
                                    NULL, 0, NULL, 0);
  switch (st)
    {
    case NET_ASYNC_NOT_READY:
      return env->intern (env, "not-ready");
    case NET_ASYNC_COMPLETE:
      mysql_set_character_set (mc->conn, "utf8mb4");
      mc->phase = PHASE_IDLE;
      free_connect_strings (mc);
      return env->intern (env, "complete");
    default:
      signal_mysql_error (env, mc->conn);
      mc->phase = PHASE_IDLE;
      free_connect_strings (mc);
      return env->intern (env, "nil");
    }
}

/* ================================================================== */
/*  mysql-close                                                       */
/* ================================================================== */

static emacs_value
Fmysql_close (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
              void *data)
{
  struct mysql_conn *mc = env->get_user_ptr (env, args[0]);
  if (!mc || !mc->conn)
    {
      signal_error (env, "Invalid database object");
      return env->intern (env, "nil");
    }
  if (mc->pending_res)
    {
      mysql_free_result (mc->pending_res);
      mc->pending_res = NULL;
    }
  free_connect_strings (mc);
  mysql_close (mc->conn);
  mc->conn = NULL;
  mc->phase = PHASE_IDLE;
  env->set_user_ptr (env, args[0], NULL);
  env->set_user_finalizer (env, args[0], NULL);
  free (mc);
  return env->intern (env, "t");
}

/* ================================================================== */
/*  mysql-query: sync & async unified query                           */
/* ================================================================== */

static emacs_value
Fmysql_query (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
              void *data)
{
  /* (mysql-query DB SQL &optional ASYNC)
     ASYNC nil: synchronous, returns result plist.
     ASYNC t:   asynchronous, returns 'not-ready or result plist. */

  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");

  char *sql = extract_string (env, args[1]);
  if (!sql) return env->intern (env, "nil");

  bool async = (nargs > 2 && env->is_not_nil (env, args[2]));

  if (!async)
    {
      /* Synchronous path */
      if (mysql_real_query (mc->conn, sql, strlen (sql)))
        {
          signal_mysql_error (env, mc->conn);
          free (sql);
          return env->intern (env, "nil");
        }
      free (sql);

      MYSQL_RES *res = mysql_store_result (mc->conn);
      if (!res && mysql_field_count (mc->conn) != 0)
        {
          signal_mysql_error (env, mc->conn);
          return env->intern (env, "nil");
        }
      return build_result_plist (env, mc, res);
    }
  else
    {
      /* Asynchronous path: start query */
      enum net_async_status st =
        mysql_real_query_nonblocking (mc->conn, sql, strlen (sql));
      free (sql);

      switch (st)
        {
        case NET_ASYNC_COMPLETE:
          if (mysql_errno (mc->conn))
            {
              signal_mysql_error (env, mc->conn);
              return env->intern (env, "nil");
            }
          /* Query done, advance to store phase immediately */
          mc->phase = PHASE_STORE;
          /* Try to store immediately too */
          {
            MYSQL_RES *res = NULL;
            enum net_async_status st2 =
              mysql_store_result_nonblocking (mc->conn, &res);
            if (st2 == NET_ASYNC_COMPLETE)
              {
                if (!res && mysql_field_count (mc->conn) != 0)
                  {
                    signal_mysql_error (env, mc->conn);
                    mc->phase = PHASE_IDLE;
                    return env->intern (env, "nil");
                  }
                mc->phase = PHASE_IDLE;
                return build_result_plist (env, mc, res);
              }
            else if (st2 == NET_ASYNC_NOT_READY)
              {
                mc->pending_res = NULL;
                /* Stay in PHASE_STORE */
                return env->intern (env, "not-ready");
              }
            else
              {
                signal_mysql_error (env, mc->conn);
                mc->phase = PHASE_IDLE;
                return env->intern (env, "nil");
              }
          }
        case NET_ASYNC_NOT_READY:
          mc->phase = PHASE_QUERY;
          return env->intern (env, "not-ready");
        default:
          signal_mysql_error (env, mc->conn);
          return env->intern (env, "nil");
        }
    }
}

/* ================================================================== */
/*  mysql-query-poll                                                  */
/* ================================================================== */

static emacs_value
Fmysql_query_poll (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                   void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");

  switch (mc->phase)
    {
    case PHASE_QUERY:
      {
        enum net_async_status st =
          mysql_real_query_nonblocking (mc->conn, NULL, 0);

        if (st == NET_ASYNC_NOT_READY)
          return env->intern (env, "not-ready");

        if (st != NET_ASYNC_COMPLETE)
          {
            signal_mysql_error (env, mc->conn);
            mc->phase = PHASE_IDLE;
            return env->intern (env, "nil");
          }

        if (mysql_errno (mc->conn))
          {
            signal_mysql_error (env, mc->conn);
            mc->phase = PHASE_IDLE;
            return env->intern (env, "nil");
          }

        /* Query complete, fall through to store phase */
        mc->phase = PHASE_STORE;
      }
      /* FALLTHROUGH */

    case PHASE_STORE:
      {
        MYSQL_RES *res = NULL;
        enum net_async_status st =
          mysql_store_result_nonblocking (mc->conn, &res);

        if (st == NET_ASYNC_NOT_READY)
          return env->intern (env, "not-ready");

        if (st != NET_ASYNC_COMPLETE)
          {
            signal_mysql_error (env, mc->conn);
            mc->phase = PHASE_IDLE;
            return env->intern (env, "nil");
          }

        /* Store complete */
        if (!res && mysql_field_count (mc->conn) != 0)
          {
            signal_mysql_error (env, mc->conn);
            mc->phase = PHASE_IDLE;
            return env->intern (env, "nil");
          }

        mc->phase = PHASE_IDLE;
        return build_result_plist (env, mc, res);
      }

    default:
      signal_error (env, "No async query in progress");
      return env->intern (env, "nil");
    }
}

/* ================================================================== */
/*  mysql-execute (convenience sync alias, with prepared stmt)        */
/* ================================================================== */

static emacs_value
Fmysql_execute (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");
  MYSQL *conn = mc->conn;

  char *query = extract_string (env, args[1]);
  if (!query) return env->intern (env, "nil");

  /* Prepared-statement path when VALUES is supplied. */
  if (nargs > 2 && env->is_not_nil (env, args[2]))
    {
      MYSQL_STMT *stmt = mysql_stmt_init (conn);
      if (!stmt)
        {
          signal_mysql_error (env, conn);
          free (query);
          return env->intern (env, "nil");
        }

      if (mysql_stmt_prepare (stmt, query, strlen (query)))
        {
          signal_mysql_stmt_error (env, stmt);
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      struct bind_buf bb = {0};
      if (!build_bind_params (env, args[2], &bb))
        {
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      if (mysql_stmt_bind_param (stmt, bb.binds))
        {
          signal_mysql_stmt_error (env, stmt);
          free_bind_buf (&bb);
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      if (mysql_stmt_execute (stmt))
        {
          signal_mysql_stmt_error (env, stmt);
          free_bind_buf (&bb);
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      emacs_value retval;
      MYSQL_RES *meta = mysql_stmt_result_metadata (stmt);
      if (meta)
        {
          my_bool update_max = 1;
          mysql_stmt_attr_set (stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max);
          mysql_stmt_store_result (stmt);

          unsigned int ncols = mysql_num_fields (meta);
          MYSQL_BIND *res_binds = calloc (ncols, sizeof (MYSQL_BIND));
          char **col_bufs = calloc (ncols, sizeof (char *));
          unsigned long *col_lens = calloc (ncols, sizeof (unsigned long));
          my_bool *col_nulls = calloc (ncols, sizeof (my_bool));
          MYSQL_FIELD *fields = mysql_fetch_fields (meta);

          for (unsigned int c = 0; c < ncols; c++)
            {
              unsigned long buflen = fields[c].max_length;
              if (buflen < 256) buflen = 256;
              col_bufs[c] = calloc (1, buflen + 1);
              res_binds[c].buffer_type = MYSQL_TYPE_STRING;
              res_binds[c].buffer = col_bufs[c];
              res_binds[c].buffer_length = buflen + 1;
              res_binds[c].length = &col_lens[c];
              res_binds[c].is_null = &col_nulls[c];
            }
          mysql_stmt_bind_result (stmt, res_binds);

          emacs_value Qcons = env->intern (env, "cons");
          emacs_value Qnreverse = env->intern (env, "nreverse");
          emacs_value rows = env->intern (env, "nil");

          while (!mysql_stmt_fetch (stmt))
            {
              emacs_value row_list = env->intern (env, "nil");
              for (int c = (int)ncols - 1; c >= 0; c--)
                {
                  emacs_value v;
                  if (col_nulls[c])
                    v = env->intern (env, "nil");
                  else
                    v = env->make_string (env, col_bufs[c], col_lens[c]);
                  emacs_value ca[] = { v, row_list };
                  row_list = env->funcall (env, Qcons, 2, ca);
                }
              emacs_value ra[] = { row_list, rows };
              rows = env->funcall (env, Qcons, 2, ra);
            }

          retval = env->funcall (env, Qnreverse, 1, &rows);

          for (unsigned int c = 0; c < ncols; c++)
            free (col_bufs[c]);
          free (col_bufs); free (col_lens);
          free (col_nulls); free (res_binds);
          mysql_free_result (meta);
        }
      else
        {
          my_ulonglong affected = mysql_stmt_affected_rows (stmt);
          retval = env->make_integer (env, (intmax_t) affected);
        }

      free_bind_buf (&bb);
      mysql_stmt_close (stmt);
      free (query);
      return retval;
    }

  /* Simple (non-prepared) path. */
  if (mysql_real_query (conn, query, strlen (query)))
    {
      signal_mysql_error (env, conn);
      free (query);
      return env->intern (env, "nil");
    }
  free (query);

  MYSQL_RES *res = mysql_store_result (conn);
  if (res)
    {
      emacs_value Qcons = env->intern (env, "cons");
      emacs_value Qnreverse = env->intern (env, "nreverse");
      emacs_value rows = env->intern (env, "nil");
      MYSQL_ROW row;
      while ((row = mysql_fetch_row (res)))
        {
          emacs_value r = row_to_list (env, res, row);
          emacs_value args2[] = { r, rows };
          rows = env->funcall (env, Qcons, 2, args2);
        }
      mysql_free_result (res);
      return env->funcall (env, Qnreverse, 1, &rows);
    }
  else
    {
      if (mysql_field_count (conn) == 0)
        {
          my_ulonglong affected = mysql_affected_rows (conn);
          return env->make_integer (env, (intmax_t) affected);
        }
      else
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "nil");
        }
    }
}

/* ================================================================== */
/*  mysql-select (convenience sync alias, with prepared stmt + set)   */
/* ================================================================== */

static emacs_value
Fmysql_select (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
               void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");
  MYSQL *conn = mc->conn;

  char *query = extract_string (env, args[1]);
  if (!query) return env->intern (env, "nil");

  bool set_mode = (nargs > 3 && env->is_not_nil (env, args[3])
                   && env->eq (env, args[3], env->intern (env, "set")));
  bool full_mode = (nargs > 3 && env->is_not_nil (env, args[3])
                    && env->eq (env, args[3], env->intern (env, "full")));

  /* Prepared-statement path. */
  if (nargs > 2 && env->is_not_nil (env, args[2]))
    {
      MYSQL_STMT *stmt = mysql_stmt_init (conn);
      if (!stmt)
        {
          signal_mysql_error (env, conn);
          free (query);
          return env->intern (env, "nil");
        }

      if (mysql_stmt_prepare (stmt, query, strlen (query)))
        {
          signal_mysql_stmt_error (env, stmt);
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      struct bind_buf bb = {0};
      if (!build_bind_params (env, args[2], &bb))
        {
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      if (mysql_stmt_bind_param (stmt, bb.binds))
        {
          signal_mysql_stmt_error (env, stmt);
          free_bind_buf (&bb);
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      if (mysql_stmt_execute (stmt))
        {
          signal_mysql_stmt_error (env, stmt);
          free_bind_buf (&bb);
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      free_bind_buf (&bb);

      MYSQL_RES *meta = mysql_stmt_result_metadata (stmt);
      if (!meta)
        {
          signal_error (env, "Query did not return a result set");
          mysql_stmt_close (stmt);
          free (query);
          return env->intern (env, "nil");
        }

      my_bool update_max = 1;
      mysql_stmt_attr_set (stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max);
      mysql_stmt_store_result (stmt);
      unsigned int ncols = mysql_num_fields (meta);
      MYSQL_BIND *res_binds = calloc (ncols, sizeof (MYSQL_BIND));
      char **col_bufs = calloc (ncols, sizeof (char *));
      unsigned long *col_lens = calloc (ncols, sizeof (unsigned long));
      my_bool *col_nulls = calloc (ncols, sizeof (my_bool));
      MYSQL_FIELD *fields = mysql_fetch_fields (meta);

      for (unsigned int c = 0; c < ncols; c++)
        {
          unsigned long buflen = fields[c].max_length;
          if (buflen < 256) buflen = 256;
          col_bufs[c] = calloc (1, buflen + 1);
          res_binds[c].buffer_type = MYSQL_TYPE_STRING;
          res_binds[c].buffer = col_bufs[c];
          res_binds[c].buffer_length = buflen + 1;
          res_binds[c].length = &col_lens[c];
          res_binds[c].is_null = &col_nulls[c];
        }
      mysql_stmt_bind_result (stmt, res_binds);

      if (set_mode)
        {
          struct mysql_set *set = calloc (1, sizeof *set);
          if (!set)
            {
              signal_error (env, "malloc failed");
              for (unsigned int c = 0; c < ncols; c++) free (col_bufs[c]);
              free (col_bufs); free (col_lens); free (col_nulls); free (res_binds);
              mysql_free_result (meta); mysql_stmt_close (stmt); free (query);
              return env->intern (env, "nil");
            }
          set->conn = conn; set->stmt = stmt; set->meta = meta;
          set->res = NULL; set->res_binds = res_binds;
          set->col_bufs = col_bufs; set->col_lens = col_lens;
          set->col_nulls = col_nulls; set->ncols = ncols;
          set->eof = false; set->is_prepared = true;
          free (query);
          return env->make_user_ptr (env, mysql_set_finalizer, set);
        }

      emacs_value Qcons = env->intern (env, "cons");
      emacs_value Qnreverse = env->intern (env, "nreverse");
      emacs_value rows = env->intern (env, "nil");

      while (!mysql_stmt_fetch (stmt))
        {
          emacs_value row_list = env->intern (env, "nil");
          for (int c = (int)ncols - 1; c >= 0; c--)
            {
              emacs_value v;
              if (col_nulls[c])
                v = env->intern (env, "nil");
              else
                v = env->make_string (env, col_bufs[c], col_lens[c]);
              emacs_value ca[] = { v, row_list };
              row_list = env->funcall (env, Qcons, 2, ca);
            }
          emacs_value ra[] = { row_list, rows };
          rows = env->funcall (env, Qcons, 2, ra);
        }

      emacs_value data_list = env->funcall (env, Qnreverse, 1, &rows);

      emacs_value retval;
      if (full_mode)
        {
          emacs_value col_list = env->intern (env, "nil");
          for (int c = (int)ncols - 1; c >= 0; c--)
            {
              emacs_value nm = env->make_string (env, fields[c].name,
                                                 strlen (fields[c].name));
              emacs_value na[] = { nm, col_list };
              col_list = env->funcall (env, Qcons, 2, na);
            }
          emacs_value fa[] = { col_list, data_list };
          retval = env->funcall (env, Qcons, 2, fa);
        }
      else
        retval = data_list;

      for (unsigned int c = 0; c < ncols; c++) free (col_bufs[c]);
      free (col_bufs); free (col_lens); free (col_nulls); free (res_binds);
      mysql_free_result (meta); mysql_stmt_close (stmt); free (query);
      return retval;
    }

  /* Simple path (no bind parameters). */
  if (mysql_real_query (conn, query, strlen (query)))
    {
      signal_mysql_error (env, conn);
      free (query);
      return env->intern (env, "nil");
    }
  free (query);

  if (set_mode)
    {
      MYSQL_RES *res = mysql_use_result (conn);
      if (!res)
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "nil");
        }
      struct mysql_set *set = calloc (1, sizeof *set);
      if (!set)
        {
          signal_error (env, "malloc failed");
          mysql_free_result (res);
          return env->intern (env, "nil");
        }
      set->conn = conn; set->res = res; set->stmt = NULL;
      set->meta = NULL; set->res_binds = NULL; set->col_bufs = NULL;
      set->col_lens = NULL; set->col_nulls = NULL;
      set->ncols = mysql_num_fields (res);
      set->eof = false; set->is_prepared = false;
      return env->make_user_ptr (env, mysql_set_finalizer, set);
    }

  MYSQL_RES *res = mysql_store_result (conn);
  if (!res)
    {
      signal_mysql_error (env, conn);
      return env->intern (env, "nil");
    }

  emacs_value Qcons = env->intern (env, "cons");
  emacs_value Qnreverse = env->intern (env, "nreverse");
  emacs_value rows = env->intern (env, "nil");
  MYSQL_ROW row;
  while ((row = mysql_fetch_row (res)))
    {
      emacs_value r = row_to_list (env, res, row);
      emacs_value ra[] = { r, rows };
      rows = env->funcall (env, Qcons, 2, ra);
    }
  emacs_value data_list = env->funcall (env, Qnreverse, 1, &rows);

  emacs_value retval;
  if (full_mode)
    {
      emacs_value col_list = column_names (env, res);
      emacs_value fa[] = { col_list, data_list };
      retval = env->funcall (env, Qcons, 2, fa);
    }
  else
    retval = data_list;

  mysql_free_result (res);
  return retval;
}

/* ================================================================== */
/*  Set object functions: mysql-next / more-p / columns / finalize    */
/* ================================================================== */

static emacs_value
Fmysql_next (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set) { signal_error (env, "Invalid or closed set object"); return env->intern (env, "nil"); }
  if (set->eof) return env->intern (env, "nil");

  if (set->is_prepared)
    {
      int ret = mysql_stmt_fetch (set->stmt);
      if (ret == MYSQL_NO_DATA) { set->eof = true; return env->intern (env, "nil"); }
      if (ret != 0 && ret != MYSQL_DATA_TRUNCATED)
        {
          signal_mysql_stmt_error (env, set->stmt);
          set->eof = true;
          return env->intern (env, "nil");
        }
      emacs_value Qcons = env->intern (env, "cons");
      emacs_value row = env->intern (env, "nil");
      for (int c = (int)set->ncols - 1; c >= 0; c--)
        {
          emacs_value v;
          if (set->col_nulls[c]) v = env->intern (env, "nil");
          else v = env->make_string (env, set->col_bufs[c], set->col_lens[c]);
          emacs_value ca[] = { v, row };
          row = env->funcall (env, Qcons, 2, ca);
        }
      return row;
    }
  else
    {
      MYSQL_ROW mysql_row = mysql_fetch_row (set->res);
      if (!mysql_row) { set->eof = true; return env->intern (env, "nil"); }
      return row_to_list (env, set->res, mysql_row);
    }
}

static emacs_value
Fmysql_more_p (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set) { signal_error (env, "Invalid or closed set object"); return env->intern (env, "nil"); }
  return set->eof ? env->intern (env, "nil") : env->intern (env, "t");
}

static emacs_value
Fmysql_columns (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set) { signal_error (env, "Invalid or closed set object"); return env->intern (env, "nil"); }

  MYSQL_FIELD *fields;
  unsigned int ncols;
  if (set->is_prepared)
    {
      if (!set->meta) { signal_error (env, "Set has no metadata"); return env->intern (env, "nil"); }
      fields = mysql_fetch_fields (set->meta);
      ncols = set->ncols;
    }
  else
    {
      if (!set->res) { signal_error (env, "Set has no result"); return env->intern (env, "nil"); }
      fields = mysql_fetch_fields (set->res);
      ncols = set->ncols;
    }

  emacs_value Qcons = env->intern (env, "cons");
  emacs_value list = env->intern (env, "nil");
  for (int i = (int)ncols - 1; i >= 0; i--)
    {
      emacs_value name = env->make_string (env, fields[i].name, strlen (fields[i].name));
      emacs_value cons_args[] = { name, list };
      list = env->funcall (env, Qcons, 2, cons_args);
    }
  return list;
}

static emacs_value
Fmysql_finalize (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set) { signal_error (env, "Invalid or closed set object"); return env->intern (env, "nil"); }
  mysql_set_finalizer (set);
  env->set_user_ptr (env, args[0], NULL);
  env->set_user_finalizer (env, args[0], NULL);
  return env->intern (env, "t");
}

/* ================================================================== */
/*  mysql-execute-batch                                               */
/* ================================================================== */

static emacs_value
Fmysql_execute_batch (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");
  MYSQL *conn = mc->conn;

  char *stmts = extract_string (env, args[1]);
  if (!stmts) return env->intern (env, "nil");

  if (mysql_real_query (conn, stmts, strlen (stmts)))
    {
      signal_mysql_error (env, conn);
      free (stmts);
      return env->intern (env, "nil");
    }
  free (stmts);

  for (;;)
    {
      MYSQL_RES *res = mysql_store_result (conn);
      if (res)
        mysql_free_result (res);
      else if (mysql_field_count (conn) != 0)
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "nil");
        }
      int status = mysql_next_result (conn);
      if (status == -1) break;
      if (status > 0)
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "nil");
        }
    }

  return env->intern (env, "t");
}

/* ================================================================== */
/*  mysql-transaction / mysql-commit / mysql-rollback                 */
/* ================================================================== */

static emacs_value
mysql_simple_exec (emacs_env *env, MYSQL *conn, const char *sql)
{
  if (mysql_real_query (conn, sql, strlen (sql)))
    {
      signal_mysql_error (env, conn);
      return env->intern (env, "nil");
    }
  return env->intern (env, "t");
}

static emacs_value
Fmysql_transaction (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");
  return mysql_simple_exec (env, mc->conn, "START TRANSACTION");
}

static emacs_value
Fmysql_commit (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");
  return mysql_simple_exec (env, mc->conn, "COMMIT");
}

static emacs_value
Fmysql_rollback (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");
  return mysql_simple_exec (env, mc->conn, "ROLLBACK");
}

/* ================================================================== */
/*  Utility functions                                                 */
/* ================================================================== */

static emacs_value
Fmysql_version (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  const char *ver = mysql_get_client_info ();
  return env->make_string (env, ver, strlen (ver));
}

static emacs_value
Fmysqlp (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_conn *mc = env->get_user_ptr (env, args[0]);
  if (env->non_local_exit_check (env) != emacs_funcall_exit_return)
    {
      env->non_local_exit_clear (env);
      return env->intern (env, "nil");
    }
  return (mc && mc->conn) ? env->intern (env, "t") : env->intern (env, "nil");
}

static emacs_value
Fmysql_available_p (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  return env->intern (env, "t");
}

static emacs_value
Fmysql_escape_string (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  struct mysql_conn *mc = get_conn (env, args[0]);
  if (!mc) return env->intern (env, "nil");
  if (!check_idle (env, mc)) return env->intern (env, "nil");

  char *str = extract_string (env, args[1]);
  if (!str) return env->intern (env, "nil");

  size_t len = strlen (str);
  char *escaped = malloc (len * 2 + 1);
  if (!escaped)
    {
      signal_error (env, "malloc failed");
      free (str);
      return env->intern (env, "nil");
    }

  unsigned long new_len = mysql_real_escape_string (mc->conn, escaped, str, len);
  emacs_value result = env->make_string (env, escaped, new_len);
  free (escaped);
  free (str);
  return result;
}

/* ================================================================== */
/*  Module initialisation                                             */
/* ================================================================== */

int
emacs_module_init (struct emacs_runtime *ert)
{
  emacs_env *env = ert->get_environment (ert);

  /* --- Core: connect / close --- */

  bind_function (env, "mysql-open",
                 env->make_function (env, 3, 6, Fmysql_open,
    "Open a MySQL connection.\n"
    "(mysql-open HOST USER PASSWORD &optional DATABASE PORT ASYNC)\n"
    "ASYNC non-nil: non-blocking connect, poll with `mysql-open-poll'.\n"
    "Return a database handle object.",
                                     NULL));

  bind_function (env, "mysql-open-poll",
                 env->make_function (env, 1, 1, Fmysql_open_poll,
    "Poll an async connect started by `mysql-open'.\n"
    "(mysql-open-poll DB)\n"
    "Return 'complete or 'not-ready.",
                                     NULL));

  bind_function (env, "mysql-close",
                 env->make_function (env, 1, 1, Fmysql_close,
    "Close the MySQL database DB.",
                                     NULL));

  /* --- Core: unified query --- */

  bind_function (env, "mysql-query",
                 env->make_function (env, 2, 3, Fmysql_query,
    "Execute SQL on DB.\n"
    "(mysql-query DB SQL &optional ASYNC)\n"
    "ASYNC nil: synchronous, returns result plist.\n"
    "ASYNC non-nil: non-blocking, returns 'not-ready; poll with `mysql-query-poll'.\n"
    "Result plist: (:type select :columns COLS :rows ROWS :warning-count N)\n"
    "          or: (:type dml :affected-rows N :warning-count N)",
                                     NULL));

  bind_function (env, "mysql-query-poll",
                 env->make_function (env, 1, 1, Fmysql_query_poll,
    "Poll an async query started by `mysql-query'.\n"
    "(mysql-query-poll DB)\n"
    "Return 'not-ready or result plist.",
                                     NULL));

  /* --- Convenience sync aliases (with prepared stmt support) --- */

  bind_function (env, "mysql-execute",
                 env->make_function (env, 2, 3, Fmysql_execute,
    "Execute SQL QUERY on DB (sync).\n"
    "(mysql-execute DB QUERY &optional VALUES)\n"
    "VALUES: optional list of bind parameters.\n"
    "Return affected rows (integer) or list of row-lists.",
                                     NULL));

  bind_function (env, "mysql-select",
                 env->make_function (env, 2, 4, Fmysql_select,
    "Select data from DB (sync).\n"
    "(mysql-select DB QUERY &optional VALUES RETURN-TYPE)\n"
    "RETURN-TYPE: nil (rows), 'full (columns+rows), 'set (cursor).",
                                     NULL));

  /* --- Set object --- */

  bind_function (env, "mysql-next",
                 env->make_function (env, 1, 1, Fmysql_next,
    "Return the next row from SET as a list.\n"
    "Return nil when all rows have been fetched.",
                                     NULL));

  bind_function (env, "mysql-more-p",
                 env->make_function (env, 1, 1, Fmysql_more_p,
    "Return t if there are more results in SET.",
                                     NULL));

  bind_function (env, "mysql-columns",
                 env->make_function (env, 1, 1, Fmysql_columns,
    "Return the column names of SET as a list of strings.",
                                     NULL));

  bind_function (env, "mysql-finalize",
                 env->make_function (env, 1, 1, Fmysql_finalize,
    "Free the resources held by SET.",
                                     NULL));

  /* --- Batch / Transaction --- */

  bind_function (env, "mysql-execute-batch",
                 env->make_function (env, 2, 2, Fmysql_execute_batch,
    "Execute multiple semicolon-separated SQL STATEMENTS in DB.",
                                     NULL));

  bind_function (env, "mysql-transaction",
                 env->make_function (env, 1, 1, Fmysql_transaction,
    "Start a transaction in DB.", NULL));

  bind_function (env, "mysql-commit",
                 env->make_function (env, 1, 1, Fmysql_commit,
    "Commit a transaction in DB.", NULL));

  bind_function (env, "mysql-rollback",
                 env->make_function (env, 1, 1, Fmysql_rollback,
    "Roll back a transaction in DB.", NULL));

  /* --- Utility --- */

  bind_function (env, "mysql-version",
                 env->make_function (env, 0, 0, Fmysql_version,
    "Return the version string of the MySQL client library.", NULL));

  bind_function (env, "mysqlp",
                 env->make_function (env, 1, 1, Fmysqlp,
    "Return t if OBJECT is a MySQL connection object.", NULL));

  bind_function (env, "mysql-available-p",
                 env->make_function (env, 0, 0, Fmysql_available_p,
    "Return t if MySQL support is available.", NULL));

  bind_function (env, "mysql-escape-string",
                 env->make_function (env, 2, 2, Fmysql_escape_string,
    "Escape STRING for safe use in SQL on DB.\n"
    "(mysql-escape-string DB STRING)", NULL));

  /* --- Error symbol --- */

  {
    emacs_value Qput = env->intern (env, "put");
    emacs_value Qmysql_error = env->intern (env, "mysql-error");

    emacs_value Qerror_conditions = env->intern (env, "error-conditions");
    emacs_value Qerror = env->intern (env, "error");
    emacs_value Qlist = env->intern (env, "list");
    emacs_value cond_args[] = { Qmysql_error, Qerror };
    emacs_value conds = env->funcall (env, Qlist, 2, cond_args);
    emacs_value put_args1[] = { Qmysql_error, Qerror_conditions, conds };
    env->funcall (env, Qput, 3, put_args1);

    emacs_value Qerror_message = env->intern (env, "error-message");
    emacs_value msg = env->make_string (env, "MySQL error", 11);
    emacs_value put_args2[] = { Qmysql_error, Qerror_message, msg };
    env->funcall (env, Qput, 3, put_args2);
  }

  provide (env, "mysql-el");
  return 0;
}
