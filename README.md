# mysql-el — Emacs MySQL Dynamic Module

An Emacs dynamic module providing a native MySQL client API, with an interface style consistent with Emacs's built-in `sqlite.c`.

Every I/O function supports both **synchronous** (blocking) and **asynchronous** (non-blocking) modes through an optional `ASYNC` parameter — same function name, one extra argument.

## Table of Contents

- [Building & Installation](#building--installation)
- [Quick Start](#quick-start)
- [API Reference](#api-reference)
  - [Connection](#connection) — `mysql-open`, `mysql-open-poll`, `mysql-close`, `mysqlp`, `mysql-available-p`, `mysql-version`
  - [Query](#query) — `mysql-query`, `mysql-query-poll`
  - [Convenience Aliases](#convenience-aliases-sync-with-prepared-statement-support) — `mysql-execute`, `mysql-select`
  - [Set Objects (Cursor)](#set-objects-cursor) — `mysql-next`, `mysql-more-p`, `mysql-columns`, `mysql-finalize`
  - [Batch & Transactions](#batch--transactions) — `mysql-execute-batch`, `mysql-transaction`, `mysql-commit`, `mysql-rollback`
  - [Utility](#utility) — `mysql-escape-string`, `mysql-warning-count`
- [Error Handling](#error-handling)
- [Data Type Mapping](#data-type-mapping)
- [Running Tests](#running-tests)

---

## Building & Installation

### Prerequisites

- GNU Emacs source (built with dynamic module support, i.e. `--with-modules`)
- MySQL client library (`libmysqlclient`) and its header files
- GCC
- `makeinfo` (Texinfo, for generating `.info` documentation)

### Configuring the Makefile

Edit the two path variables at the top of the `Makefile`:

```makefile
ROOT = $(HOME)/emacs          # Emacs source root (needs src/emacs-module.h)
MYSQL_DIR = $(HOME)/mysqlinst  # MySQL install dir (needs include/ and lib/)
```

### Building

```bash
cd mysql-el
export LD_LIBRARY_PATH=$MYSQL_DIR/lib
make
```

Produces: `mysql-el.so` (dynamic module), `mysql-el.info` (documentation).

### Loading the Module

```elisp
(add-to-list 'load-path "/path/to/mysql-el/")
(require 'mysql-el)
```

---

## Quick Start

```elisp
(require 'mysql-el)

;; Connect (sync)
(setq db (mysql-open "127.0.0.1" "root" "" "mydb" 3306))

;; Connect (async — Emacs stays responsive)
(setq db (mysql-open "10.0.0.1" "root" "" "mydb" 3306 t))
(while (eq (mysql-open-poll db) 'not-ready) (sit-for 0.02))

;; Query (sync) — returns a plist
(mysql-query db "SELECT * FROM users")
;; => (:type select :columns ("id" "name") :rows ((1 "Alice") (2 "Bob")) :warning-count 0)

;; Query (async)
(mysql-query db "SELECT * FROM big_table" t)
(let (result)
  (while (eq (setq result (mysql-query-poll db)) 'not-ready)
    (sit-for 0.02))
  (plist-get result :rows))

;; Convenience aliases (sync, support prepared statements)
(mysql-execute db "INSERT INTO t VALUES (?, ?)" '(1 "foo"))  ;; => 1
(mysql-select db "SELECT * FROM t" nil 'full)
;; => (("id" "name") (1 "foo"))

;; Transactions
(mysql-transaction db)
(mysql-execute db "INSERT INTO t VALUES (2, 'bar')")
(mysql-commit db)

;; Error handling
(condition-case err
    (mysql-execute db "BAD SQL")
  (mysql-error
   (message "ERROR %d (%s): %s"
            (nth 1 err) (nth 2 err) (nth 3 err))))

;; Close
(mysql-close db)
```

---

## API Reference

### Connection

#### mysql-open

```
(mysql-open HOST USER PASSWORD &optional DATABASE PORT ASYNC) → DB
```

Open a MySQL connection. Returns a database handle object.

| Parameter | Type | Description |
|-----------|------|-------------|
| HOST | string | Host address, e.g. `"127.0.0.1"` |
| USER | string | Username |
| PASSWORD | string | Password; `""` for no password |
| DATABASE | string \| nil | Default database |
| PORT | integer \| nil | TCP port (default 3306) |
| ASYNC | non-nil \| nil | Non-nil for non-blocking connect |

When ASYNC is nil, the call blocks until the connection is established or fails. When ASYNC is non-nil, the call returns immediately with a handle in "connecting" state — poll with `mysql-open-poll` until ready.

The connection is automatically configured with `utf8mb4` charset and `CLIENT_MULTI_STATEMENTS` mode. The handle is garbage-collection safe (`mysql_close` is called by the finalizer).

```elisp
;; Sync
(setq db (mysql-open "127.0.0.1" "root" "" "mydb" 3306))

;; Async
(setq db (mysql-open "10.0.0.1" "root" "" "mydb" 3306 t))
(while (eq (mysql-open-poll db) 'not-ready) (sit-for 0.02))
```

#### mysql-open-poll

```
(mysql-open-poll DB) → 'not-ready | 'complete
```

Poll an async connect. Returns `'complete` when the connection is established. Signals `mysql-error` on failure.

#### mysql-close

```
(mysql-close DB) → t
```

Close the connection. Always synchronous.

#### mysqlp

```
(mysqlp OBJECT) → t | nil
```

Return `t` if OBJECT is a live MySQL connection handle.

#### mysql-available-p

```
(mysql-available-p) → t
```

Return `t` if the MySQL module is loaded.

#### mysql-version

```
(mysql-version) → STRING
```

Return the MySQL client library version string (e.g. `"8.0.36"`).

---

### Query

The unified query API. Same function for sync and async — the `ASYNC` parameter selects the mode.

#### mysql-query

```
(mysql-query DB SQL &optional ASYNC) → plist | 'not-ready
```

Execute SQL on DB.

| ASYNC | Behavior | Return |
|-------|----------|--------|
| nil | Synchronous (blocking) | Result plist |
| non-nil | Non-blocking start | `'not-ready`, or result plist if query completed instantly |

**Result plist:**

```elisp
;; SELECT
(:type select
 :columns ("id" "name" "salary")
 :rows ((1 "Alice" 95000.0) (2 "Bob" 88000.0))
 :warning-count 0)

;; DML / DDL
(:type dml
 :affected-rows 3
 :warning-count 2)
```

The `:warning-count` is always included — no need for a separate `mysql-warning-count` call.

```elisp
;; Sync
(let ((r (mysql-query db "SELECT * FROM t")))
  (plist-get r :rows))

;; Async
(mysql-query db "SELECT * FROM big_table" t)
(let (r)
  (while (eq (setq r (mysql-query-poll db)) 'not-ready)
    (sit-for 0.02))
  (plist-get r :rows))
```

#### mysql-query-poll

```
(mysql-query-poll DB) → 'not-ready | plist
```

Poll an async query started by `mysql-query`. Returns `'not-ready` while in progress, or the result plist when complete. Signals `mysql-error` on failure.

Internally manages both the query phase and the store-result phase — the caller only needs one poll loop.

---

### Convenience Aliases (sync, with prepared statement support)

These are synchronous-only convenience functions that support prepared statements (`?` placeholders). They return results in the traditional format (not plists), matching the style of Emacs's built-in `sqlite-execute` / `sqlite-select`.

#### mysql-execute

```
(mysql-execute DB QUERY &optional VALUES) → INTEGER | LIST
```

Execute a SQL statement. Returns affected row count (integer) for DML/DDL, or a list of rows for SELECT.

VALUES is an optional list of bind parameters (`?` placeholders):

| Elisp Type | MySQL Bind Type |
|------------|-----------------|
| `string` | MYSQL_TYPE_STRING |
| `integer` | MYSQL_TYPE_LONGLONG |
| `float` | MYSQL_TYPE_DOUBLE |
| `nil` | MYSQL_TYPE_NULL |

```elisp
(mysql-execute db "INSERT INTO t VALUES (?, ?)" '(1 "foo"))  ;; => 1
(mysql-execute db "DELETE FROM t WHERE id > 1")               ;; => 1
```

#### mysql-select

```
(mysql-select DB QUERY &optional VALUES RETURN-TYPE) → LIST | SET
```

Execute a SELECT query. RETURN-TYPE controls the format:

| RETURN-TYPE | Returns |
|-------------|---------|
| nil | `((row1) (row2) ...)` |
| `'full` | `((columns) (row1) (row2) ...)` — first element is column names |
| `'set` | A cursor object for lazy row-by-row iteration |

```elisp
(mysql-select db "SELECT * FROM t" nil 'full)
;; => (("id" "name") (1 "Alice") (2 "Bob"))

(mysql-select db "SELECT * FROM t WHERE id = ?" '(1))
;; => (("Alice"))
```

---

### Set Objects (Cursor)

For lazy iteration over large result sets without loading all rows into memory.

```elisp
(let ((set (mysql-select db "SELECT * FROM big_table" nil 'set)))
  (unwind-protect
      (progn
        (message "Columns: %S" (mysql-columns set))
        (while (mysql-more-p set)
          (let ((row (mysql-next set)))
            (when row (message "Row: %S" row)))))
    (mysql-finalize set)))
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `mysql-next` | `(SET) → list \| nil` | Fetch next row; nil when exhausted |
| `mysql-more-p` | `(SET) → t \| nil` | Non-nil if more rows available |
| `mysql-columns` | `(SET) → list` | Column names as list of strings |
| `mysql-finalize` | `(SET) → t` | Free resources (recommended in `unwind-protect`) |

---

### Batch & Transactions

#### mysql-execute-batch

```
(mysql-execute-batch DB STATEMENTS) → t
```

Execute multiple semicolon-separated SQL statements. Does not support bind parameters.

```elisp
(mysql-execute-batch db "CREATE TABLE a (id INT); CREATE TABLE b (id INT);")
```

#### mysql-transaction / mysql-commit / mysql-rollback

```
(mysql-transaction DB) → t    ;; START TRANSACTION
(mysql-commit DB) → t         ;; COMMIT
(mysql-rollback DB) → t       ;; ROLLBACK
```

```elisp
(mysql-transaction db)
(condition-case err
    (progn
      (mysql-execute db "INSERT INTO t VALUES (1)")
      (mysql-execute db "INSERT INTO t VALUES (2)")
      (mysql-commit db))
  (mysql-error
   (mysql-rollback db)
   (signal (car err) (cdr err))))
```

---

### Utility

#### mysql-escape-string

```
(mysql-escape-string DB STRING) → STRING
```

Escape a string for safe SQL inclusion. Prefer `?` bind parameters instead.

#### mysql-warning-count

```
(mysql-warning-count DB) → INTEGER
```

Warning count from the most recent statement. Usually not needed — `mysql-query` includes `:warning-count` in the result plist.

---

## Error Handling

The module defines a `mysql-error` error symbol (inheriting from `error`) with structured data:

```
(mysql-error ERRNO SQLSTATE ERRMSG)
```

| Field | Type | Example |
|-------|------|---------|
| ERRNO | integer | `1690` |
| SQLSTATE | string | `"22003"` |
| ERRMSG | string | `"BIGINT value is out of range..."` |

```elisp
(condition-case err
    (mysql-query db "SELECT * FROM t WHERE col + 1 > 300")
  (mysql-error
   (let ((errno   (nth 1 err))
         (sqlstate (nth 2 err))
         (errmsg   (nth 3 err)))
     (message "ERROR %d (%s): %s" errno sqlstate errmsg))))
```

Generic programming errors (`"Invalid or closed database object"`, `"malloc failed"`, `"Connection busy"`) signal the standard `error` symbol.

---

## Data Type Mapping

### Write: Elisp → MySQL (bind parameters)

| Elisp | MySQL |
|-------|-------|
| `string` | MYSQL_TYPE_STRING (UTF-8) |
| `integer` | MYSQL_TYPE_LONGLONG |
| `float` | MYSQL_TYPE_DOUBLE |
| `nil` | MYSQL_TYPE_NULL |

### Read: MySQL → Elisp (query results)

| MySQL | Elisp (non-prepared) | Elisp (prepared) |
|-------|---------------------|------------------|
| TINY / SHORT / INT / LONG / BIGINT / YEAR | `integer` | `string` |
| FLOAT / DOUBLE / DECIMAL | `float` | `string` |
| TEXT / VARCHAR / DATE / JSON / ... | `string` | `string` |
| NULL | `nil` | `nil` |

> **Note:** The prepared statement path returns all non-NULL values as strings because MySQL's stmt API uses a uniform string buffer.

---

## Running Tests

```bash
mysql -u root -e "CREATE DATABASE IF NOT EXISTS emacs_test"
cd mysql-el && make
export LD_LIBRARY_PATH=$MYSQL_DIR/lib
emacs -batch -L . -l ert -l test.el -f ert-run-tests-batch-and-exit
```

Connection parameters are at the top of `test.el`:

```elisp
(defvar test-mysql-host "127.0.0.1")
(defvar test-mysql-user "root")
(defvar test-mysql-password "")
(defvar test-mysql-database "emacs_test")
(defvar test-mysql-port 3306)
```
