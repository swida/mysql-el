# mysql-el — Emacs MySQL Dynamic Module

An Emacs dynamic module providing a native MySQL client API, with an interface style consistent with Emacs's built-in `sqlite.c`.

## Table of Contents

- [Building & Installation](#building--installation)
- [Quick Start: A Complete Example](#quick-start-a-complete-example)
- [API Reference](#api-reference)
  - [mysql-available-p](#mysql-available-p)
  - [mysql-version](#mysql-version)
  - [mysql-open](#mysql-open)
  - [mysql-close](#mysql-close)
  - [mysqlp](#mysqlp)
  - [mysql-execute](#mysql-execute)
  - [mysql-select](#mysql-select)
  - [mysql-next](#mysql-next)
  - [mysql-more-p](#mysql-more-p)
  - [mysql-columns](#mysql-columns)
  - [mysql-finalize](#mysql-finalize)
  - [mysql-execute-batch](#mysql-execute-batch)
  - [mysql-transaction](#mysql-transaction)
  - [mysql-commit](#mysql-commit)
  - [mysql-rollback](#mysql-rollback)
  - [mysql-escape-string](#mysql-escape-string)
- [Data Type Mapping](#data-type-mapping)
- [Running Tests](#running-tests)

---

## Building & Installation

### Prerequisites

- GNU Emacs source (built with dynamic module support, i.e. `--with-modules`) — the module needs Emacs C headers (`emacs-module.h`, etc.)
- MySQL client library (`libmysqlclient`) and its header files
- GCC
- `makeinfo` (Texinfo, for generating `.info` documentation)

### Configuring the Makefile

Before building, edit the two path variables at the top of the `Makefile` to point to your environment:

```makefile
# Emacs source root directory (needs src/emacs-module.h)
ROOT = $(HOME)/emacs

# MySQL installation directory (needs include/ and lib/)
MYSQL_DIR = $(HOME)/mysqlinst
```

| Variable | Description | Required Contents |
|----------|-------------|-------------------|
| `ROOT` | Emacs source root directory | Must contain `src/emacs-module.h` |
| `MYSQL_DIR` | MySQL installation directory | Must contain `include/mysql.h` and `lib/libmysqlclient.so` |

For example, if your Emacs source is at `/opt/emacs-30` and MySQL is installed at `/usr/local/mysql`:

```makefile
ROOT = /opt/emacs-30
MYSQL_DIR = /usr/local/mysql
```

### Building

```bash
cd mysql-el

# If MySQL is installed in a non-standard path, export the library path first
export LD_LIBRARY_PATH=$MYSQL_DIR/lib   # adjust to your actual path

make
```

A successful build produces:
- `mysql-el.o` — object file
- `mysql-el.so` — dynamic module (loaded into Emacs)
- `mysql-el.info` — Info-format documentation

To clean build artifacts:

```bash
make clean
```

### Loading the Module

```elisp
;; Option 1: Load the .so directly
(module-load "/path/to/mysql-el.so")

;; Option 2: Add to load-path and require
(add-to-list 'load-path "/path/to/modules/mysql-el/")
(require 'mysql-el)
```

---

## Quick Start: A Complete Example

The following example walks through an "employee management" scenario — from table creation, insertion, querying, updating, transactions, to closing — covering all core APIs.

```elisp
;; 0. Load the module
(module-load "mysql-el.so")

;; 1. Connect to the database
(setq db (mysql-open "127.0.0.1" "root" "" "emacs_test" 3306))

;; 2. Create table (DDL)
(mysql-execute db "DROP TABLE IF EXISTS employees")
(mysql-execute db
  "CREATE TABLE employees (
     id    INT PRIMARY KEY AUTO_INCREMENT,
     name  VARCHAR(100) NOT NULL,
     dept  VARCHAR(50),
     salary DOUBLE
   ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")

;; 3. Insert data — returns the number of affected rows
(mysql-execute db "INSERT INTO employees (name, dept, salary)
                   VALUES ('Alice', 'Engineering', 25000.50)")
;; => 1

(mysql-execute db "INSERT INTO employees (name, dept, salary)
                   VALUES ('Bob', 'Product', 22000)")
;; => 1

;; 4. Insert with bound parameters (Prepared Statement) for safety
;;    Parameters are passed as a list; supports string / integer / float / nil
(mysql-execute db
  "INSERT INTO employees (name, dept, salary) VALUES (?, ?, ?)"
  '("Charlie" "Engineering" 30000.0))
;; => 1

;; 5. Basic query — returns a list of rows, each row is a list
(mysql-select db "SELECT * FROM employees ORDER BY id")
;; => (("Alice" "Engineering" 25000.5)
;;     ("Bob" "Product" 22000.0)
;;     ("Charlie" "Engineering" 30000.0))
;;  Note: INT columns are automatically converted to Emacs integers

;; 6. Query with column names (full mode)
(mysql-select db "SELECT name, salary FROM employees" nil 'full)
;; => (("name" "salary")          ; <-- first element is the column name list
;;     ("Alice" 25000.5)
;;     ("Bob" 22000.0)
;;     ("Charlie" 30000.0))

;; 7. Parameterized conditional query
(mysql-select db
  "SELECT name, salary FROM employees WHERE dept = ?"
  '("Engineering"))
;; => (("Alice" 25000.5)
;;     ("Charlie" 30000.0))

;; 8. Update data — returns the number of affected rows
(mysql-execute db
  "UPDATE employees SET salary = salary * 1.1 WHERE dept = 'Engineering'")
;; => 2

;; 9. Transaction: batch operations with rollback on error
(mysql-transaction db)
(condition-case err
    (progn
      (mysql-execute db "INSERT INTO employees (name, dept, salary)
                         VALUES ('Diana', 'Finance', 20000)")
      (mysql-execute db "INSERT INTO employees (name, dept, salary)
                         VALUES ('Eve', 'Finance', 21000)")
      (mysql-commit db)
      (message "Transaction committed successfully"))
  (error
   (mysql-rollback db)
   (message "Transaction rolled back: %s" (error-message-string err))))

;; 10. Batch execute multiple statements (semicolon-separated)
(mysql-execute-batch db
  "INSERT INTO employees (name, dept, salary) VALUES ('Frank', 'HR', 19000);
   INSERT INTO employees (name, dept, salary) VALUES ('Grace', 'HR', 18500);")

;; 11. Use set mode to iterate over large result sets row by row
;;     (without loading everything into memory at once)
(let ((set (mysql-select db "SELECT id, name, salary FROM employees ORDER BY id"
                         nil 'set)))
  (unwind-protect
      (progn
        (message "Columns: %S" (mysql-columns set))
        (while (mysql-more-p set)
          (let ((row (mysql-next set)))
            (when row
              (message "Row: %S" row)))))
    (mysql-finalize set)))

;; 12. String escaping (for dynamic SQL construction; prefer bound parameters)
(mysql-escape-string db "it's a \"test\"")
;; => "it\\'s a \\\"test\\\""

;; 13. View final results
(mysql-select db "SELECT id, name, dept, salary FROM employees ORDER BY id"
              nil 'full)

;; 14. Clean up and close
(mysql-execute db "DROP TABLE employees")
(mysql-close db)
```

---

## API Reference

### mysql-available-p

```
(mysql-available-p) → t
```

Check whether the MySQL module is loaded and available. Always returns `t` once loaded.

---

### mysql-version

```
(mysql-version) → STRING
```

Return the MySQL client library version string, e.g. `"8.0.36"`.

---

### mysql-open

```
(mysql-open HOST USER PASSWORD &optional DATABASE PORT) → DB
```

Open a MySQL connection and return a database handle object.

| Parameter | Type | Description |
|-----------|------|-------------|
| HOST | string | Host address, e.g. `"127.0.0.1"` |
| USER | string | Username |
| PASSWORD | string | Password; pass `""` for no password |
| DATABASE | string \| nil | Optional, default database name |
| PORT | integer \| nil | Optional, defaults to 3306 |

**Features:**
- The connection charset is automatically set to `utf8mb4`
- The connection is opened with `CLIENT_MULTI_STATEMENTS`, enabling `mysql-execute-batch`
- When the connection object is garbage collected, `mysql_close` is called automatically

```elisp
;; Minimal usage (3 required parameters)
(setq db (mysql-open "127.0.0.1" "root" ""))

;; Full usage
(setq db (mysql-open "127.0.0.1" "root" "mypassword" "mydb" 3306))
```

---

### mysql-close

```
(mysql-close DB) → t
```

Close the database connection. Operating on the handle after closing will signal an error.

```elisp
(mysql-close db)  ;; => t
(mysql-execute db "SELECT 1")  ;; => error: Invalid or closed database object
```

---

### mysqlp

```
(mysqlp OBJECT) → t | nil
```

Test whether an object is an **active** MySQL connection handle.

```elisp
(mysqlp db)      ;; => t
(mysqlp "hello") ;; => nil
(mysqlp 42)      ;; => nil
```

---

### mysql-execute

```
(mysql-execute DB QUERY &optional VALUES) → INTEGER | LIST
```

Execute a single SQL statement.

**Return value:**
- **DDL / INSERT / UPDATE / DELETE**: returns the number of affected rows (integer)
- **SELECT**: returns a list of rows (same as `mysql-select`, but without `full` mode)

| Parameter | Type | Description |
|-----------|------|-------------|
| DB | user-ptr | Database connection handle |
| QUERY | string | SQL statement, may contain `?` placeholders |
| VALUES | list \| nil | Optional, list of bound parameters |

**Bound parameter types:**

| Elisp Type | MySQL Type |
|------------|-----------|
| `string` | MYSQL_TYPE_STRING |
| `integer` | MYSQL_TYPE_LONGLONG |
| `float` | MYSQL_TYPE_DOUBLE |
| `nil` | MYSQL_TYPE_NULL |

```elisp
;; DDL
(mysql-execute db "CREATE TABLE t (id INT, name TEXT)")

;; INSERT, returns affected row count
(mysql-execute db "INSERT INTO t VALUES (1, 'foo')")  ;; => 1

;; INSERT with bound parameters
(mysql-execute db "INSERT INTO t VALUES (?, ?)" '(2 "bar"))  ;; => 1

;; DELETE, returns deleted row count
(mysql-execute db "DELETE FROM t WHERE id > 1")  ;; => 1

;; SELECT (not recommended; prefer mysql-select)
(mysql-execute db "SELECT * FROM t")  ;; => ((1 "foo"))
```

---

### mysql-select

```
(mysql-select DB QUERY &optional VALUES RETURN-TYPE) → LIST | SET
```

Execute a SELECT query, designed specifically for reading data.

| Parameter | Type | Description |
|-----------|------|-------------|
| DB | user-ptr | Database connection handle |
| QUERY | string | SELECT statement, may contain `?` placeholders |
| VALUES | list \| nil | Optional, list of bound parameters |
| RETURN-TYPE | symbol \| nil | `nil` = data rows only; `'full` = first element is column names; `'set` = return a cursor object |

**Normal mode** (RETURN-TYPE = nil):

```elisp
(mysql-select db "SELECT name, age FROM users")
;; => (("Alice" 30) ("Bob" 25))
```

**Full mode** (RETURN-TYPE = 'full):

```elisp
(mysql-select db "SELECT name, age FROM users" nil 'full)
;; => (("name" "age")      ; <-- column names
;;     ("Alice" 30)
;;     ("Bob" 25))
```

**Set mode** (RETURN-TYPE = 'set) — for large result sets:

Returns a set object (cursor) that fetches data row by row via `mysql-next`, avoiding loading the entire result set into memory at once. Usage is similar to SQLite's `sqlite-next` / `sqlite-more-p`.

```elisp
(let ((set (mysql-select db "SELECT * FROM big_table" nil 'set)))
  (unwind-protect
      (while (mysql-more-p set)
        (let ((row (mysql-next set)))
          (when row
            (message "Row: %S" row))))
    (mysql-finalize set)))
```

**Parameterized query:**

```elisp
(mysql-select db "SELECT * FROM users WHERE age > ?" '(20))
;; => (("Alice" 30) ("Bob" 25))
```

**Empty result set:**

```elisp
;; Normal mode: returns nil
(mysql-select db "SELECT * FROM empty_table")  ;; => nil

;; Full mode: still returns column names, but no data rows
(mysql-select db "SELECT * FROM empty_table" nil 'full)
;; => (("col1" "col2"))
```

---

### mysql-next

```
(mysql-next SET) → LIST | nil
```

Fetch the next row from a set object, returning a list. Returns `nil` when all rows have been consumed.

```elisp
(let ((set (mysql-select db "SELECT id, name FROM users" nil 'set)))
  (mysql-next set)  ;; => (1 "Alice")
  (mysql-next set)  ;; => (2 "Bob")
  (mysql-next set)  ;; => nil (no more data)
  (mysql-finalize set))
```

---

### mysql-more-p

```
(mysql-more-p SET) → t | nil
```

Test whether the set object has more data to fetch. Returns `nil` after `mysql-next` has returned `nil`.

```elisp
(while (mysql-more-p set)
  (let ((row (mysql-next set)))
    (when row (process-row row))))
```

---

### mysql-columns

```
(mysql-columns SET) → LIST
```

Return the list of column names for a set object.

```elisp
(let ((set (mysql-select db "SELECT id, name FROM users" nil 'set)))
  (mysql-columns set)  ;; => ("id" "name")
  (mysql-finalize set))
```

---

### mysql-finalize

```
(mysql-finalize SET) → t
```

Release all resources held by the set object. The set cannot be used after this call.

> **Note:** Even without calling this function explicitly, resources are automatically released when the set object is garbage collected. However, it is recommended to call this explicitly in `unwind-protect` to release database connection resources as early as possible.

```elisp
(let ((set (mysql-select db "SELECT * FROM t" nil 'set)))
  (unwind-protect
      (while (mysql-more-p set)
        (let ((row (mysql-next set)))
          (when row (process-row row))))
    (mysql-finalize set)))
```

---

### mysql-execute-batch

```
(mysql-execute-batch DB STATEMENTS) → t
```

Execute multiple semicolon-separated SQL statements. Suitable for initialization scripts, batch DDL, etc.

```elisp
(mysql-execute-batch db
  "CREATE TABLE a (id INT);
   CREATE TABLE b (id INT);
   INSERT INTO a VALUES (1);")
;; => t
```

> **Note:** This function does not support bound parameters. For parameterized execution, use `mysql-execute` for each statement individually.

---

### mysql-transaction

```
(mysql-transaction DB) → t
```

Begin a transaction (executes `START TRANSACTION`).

---

### mysql-commit

```
(mysql-commit DB) → t
```

Commit the current transaction (executes `COMMIT`). Calling this without an active transaction does not signal an error.

---

### mysql-rollback

```
(mysql-rollback DB) → t
```

Roll back the current transaction (executes `ROLLBACK`).

**Transaction usage pattern:**

```elisp
(mysql-transaction db)
(condition-case err
    (progn
      (mysql-execute db "INSERT INTO t VALUES (1)")
      (mysql-execute db "INSERT INTO t VALUES (2)")
      (mysql-commit db))
  (error
   (mysql-rollback db)
   (signal (car err) (cdr err))))
```

---

### mysql-escape-string

```
(mysql-escape-string DB STRING) → STRING
```

Escape a string for safe use in MySQL queries, preventing SQL injection. The escaping respects the current connection's character set.

```elisp
(mysql-escape-string db "it's a \"test\"")
;; => "it\\'s a \\\"test\\\""
```

> **Best practice:** Prefer using the `?` bound parameters of `mysql-execute` / `mysql-select`. Only use this function when dynamic SQL construction is unavoidable.

---

## Data Type Mapping

### Write Direction (Elisp → MySQL)

Type mapping when passing values via `?` bound parameters:

| Elisp Type | MySQL Bind Type |
|------------|-----------------|
| `string` | MYSQL_TYPE_STRING |
| `integer` | MYSQL_TYPE_LONGLONG |
| `float` | MYSQL_TYPE_DOUBLE |
| `nil` | MYSQL_TYPE_NULL |

### Read Direction (MySQL → Elisp)

Automatic type conversion for query results (`mysql-select`, non-prepared path):

| MySQL Type | Elisp Type |
|-----------|-----------|
| TINY / SHORT / INT / LONG / BIGINT | `integer` |
| FLOAT / DOUBLE / DECIMAL | `float` |
| TEXT / VARCHAR / CHAR / DATE / ... | `string` |
| NULL | `nil` |

> **Note:** Results from the prepared statement path are all returned as `string` (except NULL which is `nil`), because MySQL's stmt API uses a unified `MYSQL_TYPE_STRING` buffer for receiving data.

---

## Running Tests

The module includes 30 unit tests covering connections, CRUD, Unicode, transactions, batch execution, cursor iteration, and more.

```bash
# 1. Ensure the MySQL server is running and the emacs_test database exists
mysql -u root -e "CREATE DATABASE IF NOT EXISTS emacs_test"

# 2. Build the module
cd mysql-el
make

# 3. Run all tests
export LD_LIBRARY_PATH=$MYSQL_DIR/lib   # adjust to your actual MySQL path
emacs -batch -L . -l ert -l test.el -f ert-run-tests-batch-and-exit

# 4. Run specific tests (matched by name regex)
emacs -batch -L . -l ert -l test.el \
  --eval '(ert-run-tests-batch-and-exit "mysql-param")'
```

To modify connection parameters, edit the variables at the top of `test.el`:

```elisp
(defvar test-mysql-host "127.0.0.1")
(defvar test-mysql-user "root")
(defvar test-mysql-password "")
(defvar test-mysql-database "emacs_test")
(defvar test-mysql-port 3306)
```
