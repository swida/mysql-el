/* mysql-el: Support for accessing MySQL databases via Emacs dynamic module.
   API modeled after the built-in sqlite.c interface. */

#include <emacs-module.h>
#include <mysql.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* MySQL 8.0+ removed my_bool; define it for compatibility.  */
#ifndef my_bool
# define my_bool bool
#endif

int plugin_is_GPL_compatible;

/* ------------------------------------------------------------------ */
/*  Helper: signal an Emacs Lisp error                                */
/* ------------------------------------------------------------------ */

static void
signal_error (emacs_env *env, const char *msg)
{
  emacs_value Qerror = env->intern (env, "error");
  emacs_value data = env->make_string (env, msg, strlen (msg));
  env->non_local_exit_signal (env, Qerror, data);
}

/* Signal a `mysql-error' with structured data: (ERRNO SQLSTATE ERRMSG).
   Elisp callers receive (mysql-error ERRNO SQLSTATE ERRMSG) where:
     ERRNO    — integer, the MySQL error code (mysql_errno)
     SQLSTATE — string, the 5-character SQLSTATE (mysql_sqlstate)
     ERRMSG   — string, the human-readable message (mysql_error)
   This mirrors the Emacs built-in sqlite.c approach so that the Elisp
   layer can format/display the error however it likes without needing
   to re-query the connection handle (which may have been reset).  */
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

/* Signal a `mysql-error' for a prepared statement.
   Uses mysql_stmt_errno/sqlstate/error for the details.  */
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

/* ------------------------------------------------------------------ */
/*  Helper: Emacs Lisp utilities                                      */
/* ------------------------------------------------------------------ */

/* Bind NAME to FUN.  */
static void
bind_function (emacs_env *env, const char *name, emacs_value Sfun)
{
  emacs_value Qfset = env->intern (env, "fset");
  emacs_value Qsym = env->intern (env, name);
  emacs_value args[] = { Qsym, Sfun };
  env->funcall (env, Qfset, 2, args);
}

/* Provide FEATURE to Emacs.  */
static void
provide (emacs_env *env, const char *feature)
{
  emacs_value Qfeat = env->intern (env, feature);
  emacs_value Qprovide = env->intern (env, "provide");
  emacs_value args[] = { Qfeat };
  env->funcall (env, Qprovide, 1, args);
}

/* Extract a C string from an Emacs Lisp string value.
   Caller must free() the result.  */
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

/* Return true if VAL is the Emacs nil value.  */
static bool
is_nil (emacs_env *env, emacs_value val)
{
  return !env->is_not_nil (env, val);
}

/* ------------------------------------------------------------------ */
/*  User-pointer type tag for MYSQL* connections                      */
/* ------------------------------------------------------------------ */

/* Pointers stored inside user-ptr carry a finalizer that automatically
   calls mysql_close when the GC collects the object.  */

static void
mysql_connection_finalizer (void *ptr)
{
  if (ptr)
    mysql_close ((MYSQL *) ptr);
}

/* ------------------------------------------------------------------ */
/*  mysql_set: a cursor/set object for lazy row-by-row iteration      */
/* ------------------------------------------------------------------ */

/* A set object wraps either a MYSQL_RES (non-prepared path, obtained
   via mysql_use_result for streaming) or a MYSQL_STMT (prepared path).
   The caller iterates with mysql-next and checks mysql-more-p.  */

struct mysql_set {
  MYSQL *conn;            /* parent connection (not owned) */
  MYSQL_RES *res;         /* non-prepared result (owned), or NULL */
  MYSQL_STMT *stmt;       /* prepared statement (owned), or NULL */
  MYSQL_RES *meta;        /* prepared-stmt metadata (owned), or NULL */
  MYSQL_BIND *res_binds;  /* result bind array for prepared path */
  char **col_bufs;        /* column buffers for prepared path */
  unsigned long *col_lens;
  my_bool *col_nulls;
  unsigned int ncols;
  bool eof;               /* true after last row has been fetched */
  bool is_prepared;       /* true if using prepared statement path */
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

/* ------------------------------------------------------------------ */
/*  mysql-open                                                        */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_open (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
             void *data)
{
  /* (mysql-open HOST USER PASSWORD &optional DATABASE PORT TIMEOUT) */
  char *host = extract_string (env, args[0]);
  if (!host) return env->intern (env, "nil");
  char *user = extract_string (env, args[1]);
  if (!user) { free (host); return env->intern (env, "nil"); }
  char *password = extract_string (env, args[2]);
  if (!password) { free (host); free (user); return env->intern (env, "nil"); }

  char *database = NULL;
  unsigned int port = 0;
  unsigned int timeout = 0;

  if (nargs > 3 && env->is_not_nil (env, args[3]))
    database = extract_string (env, args[3]);
  if (nargs > 4 && env->is_not_nil (env, args[4]))
    port = (unsigned int) env->extract_integer (env, args[4]);
  if (nargs > 5 && env->is_not_nil (env, args[5]))
    timeout = (unsigned int) env->extract_integer (env, args[5]);

  MYSQL *conn = mysql_init (NULL);
  if (!conn)
    {
      signal_error (env, "mysql_init failed");
      free (host); free (user); free (password); free (database);
      return env->intern (env, "nil");
    }

  /* Set read/write timeouts before connecting.
     This prevents Emacs from hanging indefinitely when the server
     is unresponsive (e.g. stopped in gdb, network partition).  */
  if (timeout > 0)
    {
      mysql_options (conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
      mysql_options (conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
      mysql_options (conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    }

  if (!mysql_real_connect (conn, host, user, password, database, port,
                           NULL, CLIENT_MULTI_STATEMENTS))
    {
      signal_mysql_error (env, conn);
      mysql_close (conn);
      free (host); free (user); free (password); free (database);
      return env->intern (env, "nil");
    }

  /* Force UTF-8 so that strings round-trip correctly.  */
  mysql_set_character_set (conn, "utf8mb4");

  free (host); free (user); free (password); free (database);
  return env->make_user_ptr (env, mysql_connection_finalizer, conn);
}

/* ------------------------------------------------------------------ */
/*  mysql-close                                                       */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_close (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
              void *data)
{
  /* (mysql-close DB) */
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid database object");
      return env->intern (env, "nil");
    }
  mysql_close (conn);
  env->set_user_ptr (env, args[0], NULL);
  env->set_user_finalizer (env, args[0], NULL);
  return env->intern (env, "t");
}

/* ------------------------------------------------------------------ */
/*  Internal: bind parameters and execute a prepared statement        */
/* ------------------------------------------------------------------ */

/* Build a MYSQL_BIND array from an Emacs vector of values.
   Returns a heap-allocated array (caller must free it and the
   string buffers inside).  On error, signals and returns NULL.  */

struct bind_buf {
  MYSQL_BIND *binds;
  char **str_bufs;        /* owned string copies */
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
  /* Get the length of the vector.  */
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
          out->str_bufs[i] = (char *) p;  /* for later free */
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

/* ------------------------------------------------------------------ */
/*  Internal: convert one result row to an Emacs list                 */
/* ------------------------------------------------------------------ */

static emacs_value
row_to_list (emacs_env *env, MYSQL_RES *res, MYSQL_ROW row)
{
  unsigned int ncols = mysql_num_fields (res);
  unsigned long *lengths = mysql_fetch_lengths (res);
  MYSQL_FIELD *fields = mysql_fetch_fields (res);

  /* Build a list in reverse, then call nreverse.  */
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
          /* Try to give integers and floats their native types.  */
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

/* ------------------------------------------------------------------ */
/*  Internal: get column names from a result set                      */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  mysql-execute                                                     */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_execute (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                void *data)
{
  /* (mysql-execute DB QUERY &optional VALUES)
     Execute QUERY on DB.  If QUERY produces rows, return them as a
     list of lists.  Otherwise return the number of affected rows.
     VALUES, if non-nil, should be a vector of bind parameters.  */

  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  char *query = extract_string (env, args[1]);
  if (!query) return env->intern (env, "nil");

  /* Prepared-statement path when VALUES is supplied.  */
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
          /* Statement returned rows – fetch via store_result.
             Enable max_length update so we can size buffers.  */
          my_bool update_max = 1;
          mysql_stmt_attr_set (stmt, STMT_ATTR_UPDATE_MAX_LENGTH,
                               &update_max);
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
              if (buflen < 256)
                buflen = 256;  /* reasonable minimum */
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
          free (col_bufs);
          free (col_lens);
          free (col_nulls);
          free (res_binds);
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

  /* Simple (non-prepared) path.  */
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

/* ------------------------------------------------------------------ */
/*  mysql-select                                                      */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_select (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
               void *data)
{
  /* (mysql-select DB QUERY &optional VALUES RETURN-TYPE)
     RETURN-TYPE: nil  -> list of row-lists
                  full -> (column-names row1 row2 ...)  */

  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  char *query = extract_string (env, args[1]);
  if (!query) return env->intern (env, "nil");

  /* Determine return-type.  */
  bool set_mode = (nargs > 3 && env->is_not_nil (env, args[3])
                   && env->eq (env, args[3], env->intern (env, "set")));
  bool full_mode = (nargs > 3 && env->is_not_nil (env, args[3])
                    && env->eq (env, args[3], env->intern (env, "full")));

  /* Prepared-statement path.  */
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

      /* Enable max_length update so we can size result buffers.  */
      my_bool update_max = 1;
      mysql_stmt_attr_set (stmt, STMT_ATTR_UPDATE_MAX_LENGTH,
                           &update_max);
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
          if (buflen < 256)
            buflen = 256;  /* reasonable minimum */
          col_bufs[c] = calloc (1, buflen + 1);
          res_binds[c].buffer_type = MYSQL_TYPE_STRING;
          res_binds[c].buffer = col_bufs[c];
          res_binds[c].buffer_length = buflen + 1;
          res_binds[c].length = &col_lens[c];
          res_binds[c].is_null = &col_nulls[c];
        }
      mysql_stmt_bind_result (stmt, res_binds);

      /* 'set mode: return a cursor object for lazy iteration.  */
      if (set_mode)
        {
          struct mysql_set *set = calloc (1, sizeof *set);
          if (!set)
            {
              signal_error (env, "malloc failed");
              for (unsigned int c = 0; c < ncols; c++)
                free (col_bufs[c]);
              free (col_bufs); free (col_lens);
              free (col_nulls); free (res_binds);
              mysql_free_result (meta);
              mysql_stmt_close (stmt);
              free (query);
              return env->intern (env, "nil");
            }
          set->conn = conn;
          set->stmt = stmt;
          set->meta = meta;
          set->res = NULL;
          set->res_binds = res_binds;
          set->col_bufs = col_bufs;
          set->col_lens = col_lens;
          set->col_nulls = col_nulls;
          set->ncols = ncols;
          set->eof = false;
          set->is_prepared = true;
          free (query);
          return env->make_user_ptr (env, mysql_set_finalizer, set);
        }

      /* Eagerly fetch all rows.  */
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

      for (unsigned int c = 0; c < ncols; c++)
        free (col_bufs[c]);
      free (col_bufs);
      free (col_lens);
      free (col_nulls);
      free (res_binds);
      mysql_free_result (meta);
      mysql_stmt_close (stmt);
      free (query);
      return retval;
    }

  /* Simple path (no bind parameters).  */
  if (mysql_real_query (conn, query, strlen (query)))
    {
      signal_mysql_error (env, conn);
      free (query);
      return env->intern (env, "nil");
    }
  free (query);

  /* For 'set mode, use mysql_use_result for streaming (no full
     buffering in client memory).  Otherwise use mysql_store_result
     as before.  */
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
      set->conn = conn;
      set->res = res;
      set->stmt = NULL;
      set->meta = NULL;
      set->res_binds = NULL;
      set->col_bufs = NULL;
      set->col_lens = NULL;
      set->col_nulls = NULL;
      set->ncols = mysql_num_fields (res);
      set->eof = false;
      set->is_prepared = false;
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

/* ------------------------------------------------------------------ */
/*  mysql-next                                                        */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_next (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
             void *data)
{
  /* (mysql-next SET)
     Return the next row from SET as a list, or nil when exhausted.  */

  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set)
    {
      signal_error (env, "Invalid or closed set object");
      return env->intern (env, "nil");
    }

  if (set->eof)
    return env->intern (env, "nil");

  if (set->is_prepared)
    {
      int ret = mysql_stmt_fetch (set->stmt);
      if (ret == MYSQL_NO_DATA)
        {
          set->eof = true;
          return env->intern (env, "nil");
        }
      if (ret != 0 && ret != MYSQL_DATA_TRUNCATED)
        {
          signal_mysql_stmt_error (env, set->stmt);
          set->eof = true;
          return env->intern (env, "nil");
        }

      /* Build a list from the bound result buffers.  */
      emacs_value Qcons = env->intern (env, "cons");
      emacs_value row = env->intern (env, "nil");
      for (int c = (int)set->ncols - 1; c >= 0; c--)
        {
          emacs_value v;
          if (set->col_nulls[c])
            v = env->intern (env, "nil");
          else
            v = env->make_string (env, set->col_bufs[c], set->col_lens[c]);
          emacs_value ca[] = { v, row };
          row = env->funcall (env, Qcons, 2, ca);
        }
      return row;
    }
  else
    {
      /* Non-prepared (mysql_use_result) path.  */
      MYSQL_ROW mysql_row = mysql_fetch_row (set->res);
      if (!mysql_row)
        {
          set->eof = true;
          return env->intern (env, "nil");
        }
      return row_to_list (env, set->res, mysql_row);
    }
}

/* ------------------------------------------------------------------ */
/*  mysql-more-p                                                      */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_more_p (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
               void *data)
{
  /* (mysql-more-p SET)
     Return t if there are more rows in SET, nil otherwise.  */

  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set)
    {
      signal_error (env, "Invalid or closed set object");
      return env->intern (env, "nil");
    }

  return set->eof ? env->intern (env, "nil") : env->intern (env, "t");
}

/* ------------------------------------------------------------------ */
/*  mysql-columns                                                     */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_columns (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                void *data)
{
  /* (mysql-columns SET)
     Return the column names of SET as a list of strings.  */

  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set)
    {
      signal_error (env, "Invalid or closed set object");
      return env->intern (env, "nil");
    }

  MYSQL_FIELD *fields;
  unsigned int ncols;

  if (set->is_prepared)
    {
      if (!set->meta)
        {
          signal_error (env, "Set has no metadata");
          return env->intern (env, "nil");
        }
      fields = mysql_fetch_fields (set->meta);
      ncols = set->ncols;
    }
  else
    {
      if (!set->res)
        {
          signal_error (env, "Set has no result");
          return env->intern (env, "nil");
        }
      fields = mysql_fetch_fields (set->res);
      ncols = set->ncols;
    }

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

/* ------------------------------------------------------------------ */
/*  mysql-finalize                                                    */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_finalize (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                 void *data)
{
  /* (mysql-finalize SET)
     Free the resources held by SET.  */

  struct mysql_set *set = env->get_user_ptr (env, args[0]);
  if (!set)
    {
      signal_error (env, "Invalid or closed set object");
      return env->intern (env, "nil");
    }

  /* Call the finalizer to free all internal resources.  */
  mysql_set_finalizer (set);

  /* Null out the user pointer so it won't be finalized again by GC.  */
  env->set_user_ptr (env, args[0], NULL);
  env->set_user_finalizer (env, args[0], NULL);

  return env->intern (env, "t");
}

/* ------------------------------------------------------------------ */
/*  mysql-execute-batch                                               */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_execute_batch (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                      void *data)
{
  /* (mysql-execute-batch DB STATEMENTS)
     Execute multiple semicolon-separated SQL statements.  */

  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  char *stmts = extract_string (env, args[1]);
  if (!stmts) return env->intern (env, "nil");

  if (mysql_real_query (conn, stmts, strlen (stmts)))
    {
      signal_mysql_error (env, conn);
      free (stmts);
      return env->intern (env, "nil");
    }
  free (stmts);

  /* Drain all result sets produced by the multi-statement query.
     For DDL statements mysql_store_result returns NULL and
     mysql_field_count is 0 — that is normal, not an error.  */
  for (;;)
    {
      MYSQL_RES *res = mysql_store_result (conn);
      if (res)
        mysql_free_result (res);
      else if (mysql_field_count (conn) != 0)
        {
          /* A query that should return results failed.  */
          signal_mysql_error (env, conn);
          return env->intern (env, "nil");
        }
      int status = mysql_next_result (conn);
      if (status == -1)
        break;            /* no more results */
      if (status > 0)
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "nil");
        }
      /* status == 0: more results, continue loop */
    }

  return env->intern (env, "t");
}

/* ------------------------------------------------------------------ */
/*  mysql-transaction / mysql-commit / mysql-rollback                 */
/* ------------------------------------------------------------------ */

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
Fmysql_transaction (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                    void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn) { signal_error (env, "Invalid or closed database object"); return env->intern (env, "nil"); }
  return mysql_simple_exec (env, conn, "START TRANSACTION");
}

static emacs_value
Fmysql_commit (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
               void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn) { signal_error (env, "Invalid or closed database object"); return env->intern (env, "nil"); }
  return mysql_simple_exec (env, conn, "COMMIT");
}

static emacs_value
Fmysql_rollback (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                 void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn) { signal_error (env, "Invalid or closed database object"); return env->intern (env, "nil"); }
  return mysql_simple_exec (env, conn, "ROLLBACK");
}

/* ------------------------------------------------------------------ */
/*  mysql-version                                                     */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_version (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                void *data)
{
  const char *ver = mysql_get_client_info ();
  return env->make_string (env, ver, strlen (ver));
}

/* ------------------------------------------------------------------ */
/*  mysqlp / mysql-available-p                                        */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysqlp (emacs_env *env, ptrdiff_t nargs, emacs_value args[], void *data)
{
  /* Return t if the argument is a live MySQL connection user-ptr.  */
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (env->non_local_exit_check (env) != emacs_funcall_exit_return)
    {
      env->non_local_exit_clear (env);
      return env->intern (env, "nil");
    }
  return conn ? env->intern (env, "t") : env->intern (env, "nil");
}

static emacs_value
Fmysql_available_p (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                    void *data)
{
  return env->intern (env, "t");
}

/* ------------------------------------------------------------------ */
/*  mysql-escape-string                                               */
/* ------------------------------------------------------------------ */

static emacs_value
Fmysql_escape_string (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                      void *data)
{
  /* (mysql-escape-string DB STRING) */
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

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

  unsigned long new_len = mysql_real_escape_string (conn, escaped, str, len);
  emacs_value result = env->make_string (env, escaped, new_len);

  free (escaped);
  free (str);
  return result;
}

/* ------------------------------------------------------------------ */
/*  Asynchronous (non-blocking) query API                             */
/*  Uses mysql_real_query_nonblocking / mysql_store_result_nonblocking */
/*  so that Emacs can remain responsive during long queries.          */
/* ------------------------------------------------------------------ */

/* Include the header that defines net_async_status.  */
#include <mysql/plugin_auth_common.h>

/* (mysql-query-start DB SQL) → symbol
   Begin an asynchronous query.  Returns:
     'complete  — query already finished (fast path)
     'not-ready — still in progress, call mysql-query-continue
     'error     — query failed  */
static emacs_value
Fmysql_query_start (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                    void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  char *sql = extract_string (env, args[1]);
  if (!sql) return env->intern (env, "nil");

  enum net_async_status status =
    mysql_real_query_nonblocking (conn, sql, strlen (sql));
  free (sql);

  switch (status)
    {
    case NET_ASYNC_COMPLETE:
      /* The query phase completed, but the server may have returned
         an error (e.g. ERROR 1690 BIGINT out of range).  Check errno
         so that the caller sees an Emacs error instead of silently
         falling through to the result-fetch path.  */
      if (mysql_errno (conn))
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "error");
        }
      return env->intern (env, "complete");
    case NET_ASYNC_NOT_READY:
      return env->intern (env, "not-ready");
    default:
      signal_mysql_error (env, conn);
      return env->intern (env, "error");
    }
}

/* (mysql-query-continue DB) → symbol
   Continue a previously started asynchronous query.
   Returns 'complete, 'not-ready, or 'error.  */
static emacs_value
Fmysql_query_continue (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                       void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  /* Re-call with the same args (NULL, 0) — the library keeps state
     internally on the MYSQL handle.  */
  enum net_async_status status =
    mysql_real_query_nonblocking (conn, NULL, 0);

  switch (status)
    {
    case NET_ASYNC_COMPLETE:
      if (mysql_errno (conn))
        {
          signal_mysql_error (env, conn);
          return env->intern (env, "error");
        }
      return env->intern (env, "complete");
    case NET_ASYNC_NOT_READY:
      return env->intern (env, "not-ready");
    default:
      signal_mysql_error (env, conn);
      return env->intern (env, "error");
    }
}

/* (mysql-store-result-start DB) → (STATUS . RESULT-OR-NIL)
   Begin asynchronously fetching the result set into client memory.
   STATUS is 'complete, 'not-ready, or 'error.
   When STATUS is 'complete, cdr is the result-set user-ptr (or nil
   if the query was not a SELECT).  */
static emacs_value
Fmysql_store_result_start (emacs_env *env, ptrdiff_t nargs,
                           emacs_value args[], void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  MYSQL_RES *res = NULL;
  enum net_async_status status =
    mysql_store_result_nonblocking (conn, &res);

  emacs_value Qcons = env->intern (env, "cons");
  emacs_value status_sym;
  emacs_value result_val;

  switch (status)
    {
    case NET_ASYNC_COMPLETE:
      status_sym = env->intern (env, "complete");
      if (res)
        result_val = env->make_user_ptr (env, NULL, res);
      else
        {
          /* res is NULL.  This is normal for non-SELECT statements
             (field_count == 0).  But if field_count != 0, the query
             was a SELECT that failed — report the error.  */
          if (mysql_field_count (conn) != 0)
            {
              /* Always use signal_mysql_error — even if errno is 0
                 the structured data format will carry that info.  */
              signal_mysql_error (env, conn);
              return env->intern (env, "nil");
            }
          result_val = env->intern (env, "nil");
        }
      break;
    case NET_ASYNC_NOT_READY:
      status_sym = env->intern (env, "not-ready");
      result_val = env->intern (env, "nil");
      break;
    default:
      signal_mysql_error (env, conn);
      return env->intern (env, "nil");
    }

  emacs_value cons_args[] = { status_sym, result_val };
  return env->funcall (env, Qcons, 2, cons_args);
}

/* (mysql-store-result-continue DB) → (STATUS . RESULT-OR-NIL)
   Continue fetching the result set.  Same return as -start.  */
static emacs_value
Fmysql_store_result_continue (emacs_env *env, ptrdiff_t nargs,
                              emacs_value args[], void *data)
{
  /* Identical to -start: the nonblocking API uses the MYSQL handle
     to track state internally.  */
  return Fmysql_store_result_start (env, nargs, args, data);
}

/* (mysql-async-result DB RESULT-PTR FULL) → LIST
   Synchronously convert a MYSQL_RES* (already fully stored in client
   memory) to an Emacs list.  FULL non-nil means include column names
   as the first element.  This does NOT block on I/O.  */
static emacs_value
Fmysql_async_result (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                     void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }

  MYSQL_RES *res = env->get_user_ptr (env, args[1]);
  if (!res)
    {
      /* Not a SELECT — return affected row count.  */
      if (mysql_field_count (conn) == 0)
        {
          my_ulonglong affected = mysql_affected_rows (conn);
          return env->make_integer (env, (intmax_t) affected);
        }
      signal_mysql_error (env, conn);
      return env->intern (env, "nil");
    }

  bool full_mode = (nargs > 2 && env->is_not_nil (env, args[2]));

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
  /* Null out the user-ptr so it won't be double-freed.  */
  env->set_user_ptr (env, args[1], NULL);
  return retval;
}

/* (mysql-async-affected-rows DB) → INTEGER
   After an async non-SELECT query completes, return the affected row count.  */
static emacs_value
Fmysql_async_affected_rows (emacs_env *env, ptrdiff_t nargs,
                            emacs_value args[], void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }
  my_ulonglong affected = mysql_affected_rows (conn);
  return env->make_integer (env, (intmax_t) affected);
}

/* (mysql-async-field-count DB) → INTEGER
   After an async query completes, return field_count.
   0 means the query was not a SELECT.  */
static emacs_value
Fmysql_async_field_count (emacs_env *env, ptrdiff_t nargs,
                          emacs_value args[], void *data)
{
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }
  return env->make_integer (env, (intmax_t) mysql_field_count (conn));
}

/* (mysql-warning-count DB) → INTEGER
   Return the number of warnings generated by the most recent
   statement on DB.  Returns 0 when there are no warnings.  */
static emacs_value
Fmysql_warning_count (emacs_env *env, ptrdiff_t nargs, emacs_value args[],
                      void *data)
{
  (void) nargs; (void) data;
  MYSQL *conn = env->get_user_ptr (env, args[0]);
  if (!conn)
    {
      signal_error (env, "Invalid or closed database object");
      return env->intern (env, "nil");
    }
  return env->make_integer (env, (intmax_t) mysql_warning_count (conn));
}

/* ------------------------------------------------------------------ */
/*  Module initialisation                                             */
/* ------------------------------------------------------------------ */

int
emacs_module_init (struct emacs_runtime *ert)
{
  emacs_env *env = ert->get_environment (ert);

  /* mysql-open: 3 required (host user password), 2 optional (database port) */
  bind_function (env, "mysql-open",
                 env->make_function (env, 3, 6, Fmysql_open,
    "Open a MySQL connection.\n"
    "(mysql-open HOST USER PASSWORD &optional DATABASE PORT TIMEOUT)\n"
    "TIMEOUT is the read/write/connect timeout in seconds (0 or nil = no timeout).\n"
    "Return a database handle object.",
                                     NULL));

  /* mysql-close: 1 arg */
  bind_function (env, "mysql-close",
                 env->make_function (env, 1, 1, Fmysql_close,
    "Close the MySQL database DB.",
                                     NULL));

  /* mysql-execute: 2-3 args */
  bind_function (env, "mysql-execute",
                 env->make_function (env, 2, 3, Fmysql_execute,
    "Execute SQL QUERY on DB.\n"
    "(mysql-execute DB QUERY &optional VALUES)\n"
    "VALUES is an optional vector of bind parameters.\n"
    "For queries that produce rows, return a list of row-lists.\n"
    "Otherwise return the number of affected rows.",
                                     NULL));

  /* mysql-select: 2-4 args */
  bind_function (env, "mysql-select",
                 env->make_function (env, 2, 4, Fmysql_select,
    "Select data from DB matching QUERY.\n"
    "(mysql-select DB QUERY &optional VALUES RETURN-TYPE)\n"
    "RETURN-TYPE nil: list of rows.  'full: (columns row1 row2 ...).\n"
    "'set: return a set object for lazy iteration with mysql-next.",
                                     NULL));

  /* mysql-next: 1 arg */
  bind_function (env, "mysql-next",
                 env->make_function (env, 1, 1, Fmysql_next,
    "Return the next row from SET as a list.\n"
    "Return nil when all rows have been fetched.",
                                     NULL));

  /* mysql-more-p: 1 arg */
  bind_function (env, "mysql-more-p",
                 env->make_function (env, 1, 1, Fmysql_more_p,
    "Return t if there are more results in SET.",
                                     NULL));

  /* mysql-columns: 1 arg */
  bind_function (env, "mysql-columns",
                 env->make_function (env, 1, 1, Fmysql_columns,
    "Return the column names of SET as a list of strings.",
                                     NULL));

  /* mysql-finalize: 1 arg */
  bind_function (env, "mysql-finalize",
                 env->make_function (env, 1, 1, Fmysql_finalize,
    "Free the resources held by SET.\n"
    "After this call, SET can no longer be used.",
                                     NULL));

  /* mysql-execute-batch: 2 args */
  bind_function (env, "mysql-execute-batch",
                 env->make_function (env, 2, 2, Fmysql_execute_batch,
    "Execute multiple semicolon-separated SQL STATEMENTS in DB.",
                                     NULL));

  /* mysql-transaction / mysql-commit / mysql-rollback: 1 arg each */
  bind_function (env, "mysql-transaction",
                 env->make_function (env, 1, 1, Fmysql_transaction,
    "Start a transaction in DB.",
                                     NULL));

  bind_function (env, "mysql-commit",
                 env->make_function (env, 1, 1, Fmysql_commit,
    "Commit a transaction in DB.",
                                     NULL));

  bind_function (env, "mysql-rollback",
                 env->make_function (env, 1, 1, Fmysql_rollback,
    "Roll back a transaction in DB.",
                                     NULL));

  /* mysql-version: 0 args */
  bind_function (env, "mysql-version",
                 env->make_function (env, 0, 0, Fmysql_version,
    "Return the version string of the MySQL client library.",
                                     NULL));

  /* mysqlp: 1 arg */
  bind_function (env, "mysqlp",
                 env->make_function (env, 1, 1, Fmysqlp,
    "Return t if OBJECT is a MySQL connection object.",
                                     NULL));

  /* mysql-available-p: 0 args */
  bind_function (env, "mysql-available-p",
                 env->make_function (env, 0, 0, Fmysql_available_p,
    "Return t if MySQL support is available.",
                                     NULL));

  /* mysql-escape-string: 2 args */
  bind_function (env, "mysql-escape-string",
                 env->make_function (env, 2, 2, Fmysql_escape_string,
    "Escape STRING for safe use in SQL on DB.\n"
    "(mysql-escape-string DB STRING)",
                                     NULL));

  /* --- Asynchronous (non-blocking) API --- */

  bind_function (env, "mysql-query-start",
                 env->make_function (env, 2, 2, Fmysql_query_start,
    "Begin an asynchronous query on DB.\n"
    "(mysql-query-start DB SQL)\n"
    "Return 'complete, 'not-ready, or 'error.",
                                     NULL));

  bind_function (env, "mysql-query-continue",
                 env->make_function (env, 1, 1, Fmysql_query_continue,
    "Continue a previously started asynchronous query on DB.\n"
    "(mysql-query-continue DB)\n"
    "Return 'complete, 'not-ready, or 'error.",
                                     NULL));

  bind_function (env, "mysql-store-result-start",
                 env->make_function (env, 1, 1, Fmysql_store_result_start,
    "Begin async fetching of result set into client memory.\n"
    "(mysql-store-result-start DB)\n"
    "Return (STATUS . RESULT-OR-NIL).",
                                     NULL));

  bind_function (env, "mysql-store-result-continue",
                 env->make_function (env, 1, 1, Fmysql_store_result_continue,
    "Continue async fetching of result set.\n"
    "(mysql-store-result-continue DB)\n"
    "Return (STATUS . RESULT-OR-NIL).",
                                     NULL));

  bind_function (env, "mysql-async-result",
                 env->make_function (env, 2, 3, Fmysql_async_result,
    "Convert an async MYSQL_RES to an Emacs list (no I/O).\n"
    "(mysql-async-result DB RESULT-PTR &optional FULL)\n"
    "FULL non-nil: include column names as first element.",
                                     NULL));

  bind_function (env, "mysql-async-affected-rows",
                 env->make_function (env, 1, 1, Fmysql_async_affected_rows,
    "Return affected row count after async non-SELECT query.\n"
    "(mysql-async-affected-rows DB)",
                                     NULL));

  bind_function (env, "mysql-async-field-count",
                 env->make_function (env, 1, 1, Fmysql_async_field_count,
    "Return field_count after async query (0 = not a SELECT).\n"
    "(mysql-async-field-count DB)",
                                     NULL));

  bind_function (env, "mysql-warning-count",
                 env->make_function (env, 1, 1, Fmysql_warning_count,
    "Return the number of warnings from the most recent statement on DB.\n"
    "(mysql-warning-count DB)\n"
    "Returns 0 when there are no warnings.",
                                     NULL));

  /* Define the `mysql-error' error symbol.
     Equivalent to: (define-error 'mysql-error "MySQL error" 'error)
     which expands to:
       (put 'mysql-error 'error-conditions '(mysql-error error))
       (put 'mysql-error 'error-message "MySQL error")  */
  {
    emacs_value Qput = env->intern (env, "put");
    emacs_value Qmysql_error = env->intern (env, "mysql-error");

    /* (put 'mysql-error 'error-conditions '(mysql-error error)) */
    emacs_value Qerror_conditions = env->intern (env, "error-conditions");
    emacs_value Qerror = env->intern (env, "error");
    emacs_value Qlist = env->intern (env, "list");
    emacs_value cond_args[] = { Qmysql_error, Qerror };
    emacs_value conds = env->funcall (env, Qlist, 2, cond_args);
    emacs_value put_args1[] = { Qmysql_error, Qerror_conditions, conds };
    env->funcall (env, Qput, 3, put_args1);

    /* (put 'mysql-error 'error-message "MySQL error") */
    emacs_value Qerror_message = env->intern (env, "error-message");
    emacs_value msg = env->make_string (env, "MySQL error", 11);
    emacs_value put_args2[] = { Qmysql_error, Qerror_message, msg };
    env->funcall (env, Qput, 3, put_args2);
  }

  provide (env, "mysql-el");
  return 0;
}
