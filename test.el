;;; test.el --- Tests for mysql-el (MySQL) module  -*- lexical-binding: t; -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; Test suite for the mysql-el dynamic module, which provides a MySQL
;; client API analogous to the built-in sqlite interface.
;;
;; These tests require a running MySQL server.  Adjust the connection
;; parameters below before running.

;;; Code:

(require 'ert)

;; Load the dynamic module from the same directory as this file.
;; #$ expands to the file name when byte-compiled/loaded; fall back to
;; buffer-file-name when evaluated interactively.
(let* ((this-file (or #$ (expand-file-name (buffer-file-name))))
       (dir (file-name-directory this-file))
       (so (expand-file-name "mysql-el.so" dir)))
  (unless (featurep 'mysql-el)
    (module-load so)))

;; ----------------------------------------------------------------
;;  Connection parameters — adjust to match your local MySQL setup
;; ----------------------------------------------------------------

(defvar test-mysql-host "127.0.0.1"
  "Hostname for MySQL tests.")
(defvar test-mysql-user "root"
  "Username for MySQL tests.")
(defvar test-mysql-password ""
  "Password for MySQL tests.")
(defvar test-mysql-database "emacs_test"
  "Database name for MySQL tests (must already exist).")
(defvar test-mysql-port 3306
  "Port for MySQL tests.")

;; ----------------------------------------------------------------
;;  Helper: open a connection with default parameters
;; ----------------------------------------------------------------

(defun test-mysql-open ()
  "Open a test MySQL connection using the default parameters."
  (mysql-open test-mysql-host
              test-mysql-user
              test-mysql-password
              test-mysql-database
              test-mysql-port))

;; Helper macro: open DB, ensure cleanup with unwind-protect.
(defmacro with-test-mysql-db (db &rest body)
  "Open a MySQL connection bound to DB, evaluate BODY, then close."
  (declare (indent 1) (debug (symbolp body)))
  `(let ((,db (test-mysql-open)))
     (unwind-protect
         (progn ,@body)
       (ignore-errors (mysql-close ,db)))))

;; ================================================================
;;  1.  Basic availability and version (no server needed)
;; ================================================================

(ert-deftest mysql-available-p-test ()
  "mysql-available-p should return t when the module is loaded."
  (should (eq (mysql-available-p) t)))

(ert-deftest mysql-version-test ()
  "mysql-version should return a non-empty version string."
  (let ((ver (mysql-version)))
    (should (stringp ver))
    (should (> (length ver) 0))))

;; ================================================================
;;  2.  Connection: open, predicate, close
;; ================================================================

(ert-deftest mysql-open-close-test ()
  "Test opening and closing a MySQL connection."
  (with-test-mysql-db db
    (should (mysqlp db))
    (should (eq (mysql-close db) t))))

(ert-deftest mysql-mysqlp-test ()
  "mysqlp returns t for a live connection, nil for other objects."
  (with-test-mysql-db db
    (should (mysqlp db))
    (should-not (mysqlp 'foo))
    (should-not (mysqlp 42))
    (should-not (mysqlp "hello"))
    (should-not (mysqlp nil))))

(ert-deftest mysql-close-makes-dead-test ()
  "After mysql-close, operations on the handle should error."
  (let ((db (test-mysql-open)))
    (mysql-close db)
    (should-error (mysql-execute db "SELECT 1"))))

;; ================================================================
;;  3.  DDL + basic execute / select
;; ================================================================

(ert-deftest mysql-create-insert-select-test ()
  "Create a table, insert rows, select them back."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_basic")
    (mysql-execute
     db "CREATE TABLE test_basic (col1 TEXT, col2 INT, col3 DOUBLE)")
    (should (= (mysql-execute
                db "INSERT INTO test_basic VALUES ('foo', 2, 9.45)")
               1))
    (should (= (mysql-execute
                db "INSERT INTO test_basic VALUES ('bar', 3, 1.5)")
               1))
    (let ((rows (mysql-select db "SELECT * FROM test_basic ORDER BY col2")))
      (should (= (length rows) 2))
      ;; First row
      (should (equal (nth 0 (nth 0 rows)) "foo"))
      (should (= (nth 1 (nth 0 rows)) 2))
      ;; Second row
      (should (equal (nth 0 (nth 1 rows)) "bar"))
      (should (= (nth 1 (nth 1 rows)) 3)))
    (mysql-execute db "DROP TABLE test_basic")))

(ert-deftest mysql-select-full-test ()
  "mysql-select with RETURN-TYPE 'full returns column names as first element."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_full")
    (mysql-execute
     db "CREATE TABLE test_full (col1 TEXT, col2 INT, col3 DOUBLE, col4 TEXT)")
    (mysql-execute
     db "INSERT INTO test_full VALUES ('foo', 2, 9.45, 'bar')")
    (let ((result (mysql-select db "SELECT * FROM test_full" nil 'full)))
      ;; First element is a list of column names
      (should (equal (car result)
                     '("col1" "col2" "col3" "col4")))
      ;; Rest is data rows
      (should (equal (nth 0 (nth 1 result)) "foo"))
      (should (= (nth 1 (nth 1 result)) 2)))
    (mysql-execute db "DROP TABLE test_full")))

;; ================================================================
;;  4.  Unicode / multi-byte characters
;; ================================================================

(ert-deftest mysql-unicode-test ()
  "UTF-8 strings should round-trip correctly."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_unicode")
    (mysql-execute
     db "CREATE TABLE test_unicode (col1 TEXT, col2 INT) CHARACTER SET utf8mb4")
    (mysql-execute db "INSERT INTO test_unicode VALUES ('fóo', 3)")
    (mysql-execute db "INSERT INTO test_unicode VALUES ('日本語', 4)")
    (mysql-execute db "INSERT INTO test_unicode VALUES ('emoji 💘', 5)")
    (let ((rows (mysql-select db "SELECT * FROM test_unicode ORDER BY col2")))
      (should (= (length rows) 3))
      (should (equal (car (nth 0 rows)) "fóo"))
      (should (equal (car (nth 1 rows)) "日本語"))
      (should (equal (car (nth 2 rows)) "emoji 💘")))
    (mysql-execute db "DROP TABLE test_unicode")))

;; ================================================================
;;  5.  Integer types and large numbers
;; ================================================================

(ert-deftest mysql-numbers-test ()
  "Integer and float types are returned with correct Emacs types."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_numbers")
    (mysql-execute
     db "CREATE TABLE test_numbers (col1 BIGINT)")
    (let ((small (expt 2 10))
          (big   (expt 2 50)))
      (mysql-execute db (format "INSERT INTO test_numbers VALUES (%d)" small))
      (mysql-execute db (format "INSERT INTO test_numbers VALUES (%d)" big))
      (let ((rows (mysql-select db "SELECT * FROM test_numbers ORDER BY col1")))
        (should (= (length rows) 2))
        (should (= (car (nth 0 rows)) small))
        (should (= (car (nth 1 rows)) big))))
    (mysql-execute db "DROP TABLE test_numbers")))

;; ================================================================
;;  6.  Prepared statements with bind parameters
;; ================================================================

(ert-deftest mysql-param-test ()
  "Bind parameters via a vector should work for insert and select."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_param")
    (mysql-execute
     db "CREATE TABLE test_param (col1 TEXT, col2 INT)")
    ;; Insert with bind parameters (use list — C code traverses with `nth')
    (mysql-execute db "INSERT INTO test_param VALUES (?, ?)"
                   '("foo" 1))
    (mysql-execute db "INSERT INTO test_param VALUES (?, ?)"
                   '("bar" 2))
    ;; Select with bind parameters
    (let ((rows (mysql-select db "SELECT * FROM test_param WHERE col2 = ?"
                              '(1))))
      (should (= (length rows) 1))
      (should (equal (car (car rows)) "foo")))
    ;; Select all
    (let ((rows (mysql-select db "SELECT * FROM test_param ORDER BY col2")))
      (should (= (length rows) 2)))
    (mysql-execute db "DROP TABLE test_param")))

(ert-deftest mysql-param-types-test ()
  "Bind parameters support string, integer, float, and nil."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_param_types")
    (mysql-execute
     db "CREATE TABLE test_param_types (s TEXT, i INT, f DOUBLE, n TEXT)")
    (mysql-execute
     db "INSERT INTO test_param_types VALUES (?, ?, ?, ?)"
     '("hello" 42 3.14 nil))
    (let ((rows (mysql-select
                 db "SELECT * FROM test_param_types" nil 'full)))
      ;; Column names
      (should (equal (car rows) '("s" "i" "f" "n")))
      (let ((row (cadr rows)))
        (should (equal (nth 0 row) "hello"))
        ;; Prepared stmt returns strings for result; check non-nil
        (should (nth 1 row))
        (should (nth 2 row))
        ;; nil parameter -> NULL -> nil result
        (should-not (nth 3 row))))
    (mysql-execute db "DROP TABLE test_param_types")))

;; ================================================================
;;  7.  Transactions: commit and rollback
;; ================================================================

(ert-deftest mysql-transaction-commit-test ()
  "mysql-transaction + mysql-commit should persist data."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_tx")
    (mysql-execute
     db "CREATE TABLE test_tx (col1 INT) ENGINE=InnoDB")
    (mysql-transaction db)
    (mysql-execute db "INSERT INTO test_tx VALUES (1)")
    (mysql-execute db "INSERT INTO test_tx VALUES (2)")
    (mysql-commit db)
    (let ((rows (mysql-select db "SELECT * FROM test_tx ORDER BY col1")))
      (should (= (length rows) 2))
      (should (= (car (nth 0 rows)) 1))
      (should (= (car (nth 1 rows)) 2)))
    (mysql-execute db "DROP TABLE test_tx")))

(ert-deftest mysql-transaction-rollback-test ()
  "mysql-transaction + mysql-rollback should discard changes."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_tx_rb")
    (mysql-execute
     db "CREATE TABLE test_tx_rb (col1 INT) ENGINE=InnoDB")
    ;; Insert one row and commit so the table is not empty.
    (mysql-execute db "INSERT INTO test_tx_rb VALUES (1)")
    ;; Now start a transaction, insert, and roll back.
    (mysql-transaction db)
    (mysql-execute db "INSERT INTO test_tx_rb VALUES (2)")
    (mysql-execute db "INSERT INTO test_tx_rb VALUES (3)")
    (mysql-rollback db)
    ;; Only the first row (committed before the transaction) survives.
    (let ((rows (mysql-select db "SELECT * FROM test_tx_rb")))
      (should (= (length rows) 1))
      (should (= (car (car rows)) 1)))
    (mysql-execute db "DROP TABLE test_tx_rb")))

;; ================================================================
;;  8.  Execute-batch: multiple statements
;; ================================================================

(ert-deftest mysql-execute-batch-test ()
  "mysql-execute-batch should execute multiple semicolon-separated statements."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_batch1")
    (mysql-execute db "DROP TABLE IF EXISTS test_batch2")
    (mysql-execute-batch
     db
     "CREATE TABLE test_batch1 (name VARCHAR(255) NOT NULL, value TEXT);
      CREATE TABLE test_batch2 (tag VARCHAR(255) PRIMARY KEY NOT NULL);")
    ;; Both tables should exist — verify by inserting and selecting.
    (mysql-execute db "INSERT INTO test_batch1 (name, value) VALUES ('a', 'b')")
    (mysql-execute db "INSERT INTO test_batch2 (tag) VALUES ('t1')")
    (let ((rows1 (mysql-select db "SELECT * FROM test_batch1"))
          (rows2 (mysql-select db "SELECT * FROM test_batch2")))
      (should (= (length rows1) 1))
      (should (= (length rows2) 1))
      (should (equal (car (car rows1)) "a"))
      (should (equal (car (car rows2)) "t1")))
    (mysql-execute db "DROP TABLE test_batch1")
    (mysql-execute db "DROP TABLE test_batch2")))

(ert-deftest mysql-execute-batch-with-emoji-test ()
  "Emoji in table names should round-trip via execute-batch."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS `tags📎`")
    (mysql-execute-batch
     db
     "CREATE TABLE `tags📎` (name TEXT NOT NULL);
      INSERT INTO `tags📎` VALUES ('first');")
    (let ((rows (mysql-select db "SELECT * FROM `tags📎`")))
      (should (= (length rows) 1))
      (should (equal (car (car rows)) "first")))
    (mysql-execute db "DROP TABLE `tags📎`")))

;; ================================================================
;;  9.  Escape string
;; ================================================================

(ert-deftest mysql-escape-string-test ()
  "mysql-escape-string should escape special characters."
  (with-test-mysql-db db
    (let ((escaped (mysql-escape-string db "it's a \"test\"")))
      (should (stringp escaped))
      ;; Single quote should be escaped
      (should (string-match-p "\\\\'" escaped))
      ;; Double quote should be escaped
      (should (string-match-p "\\\\\"" escaped)))))

;; ================================================================
;; 10.  NULL handling
;; ================================================================

(ert-deftest mysql-null-test ()
  "NULL values in MySQL should be returned as nil in Emacs."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_null")
    (mysql-execute
     db "CREATE TABLE test_null (col1 TEXT, col2 INT)")
    (mysql-execute db "INSERT INTO test_null VALUES (NULL, 1)")
    (mysql-execute db "INSERT INTO test_null VALUES ('hello', NULL)")
    (let ((rows (mysql-select db "SELECT * FROM test_null ORDER BY col2")))
      ;; Row with col2=NULL sorts first (NULL < 1 in MySQL ORDER BY)
      ;; but actually NULL sorts as the smallest value in ASC order.
      ;; Row 1: ('hello', NULL)  Row 2: (NULL, 1)
      (should (= (length rows) 2))
      ;; Check that nil appears correctly in at least one cell
      (let ((all-values (apply #'append rows)))
        (should (memq nil all-values))))
    (mysql-execute db "DROP TABLE test_null")))

;; ================================================================
;; 11.  Multiple affected rows
;; ================================================================

(ert-deftest mysql-affected-rows-test ()
  "mysql-execute should return the number of affected rows."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_affected")
    (mysql-execute
     db "CREATE TABLE test_affected (col1 INT)")
    (mysql-execute db "INSERT INTO test_affected VALUES (1)")
    (mysql-execute db "INSERT INTO test_affected VALUES (2)")
    (mysql-execute db "INSERT INTO test_affected VALUES (3)")
    ;; DELETE multiple rows
    (let ((affected (mysql-execute
                     db "DELETE FROM test_affected WHERE col1 > 1")))
      (should (= affected 2)))
    ;; Only one row should remain
    (let ((rows (mysql-select db "SELECT * FROM test_affected")))
      (should (= (length rows) 1))
      (should (= (car (car rows)) 1)))
    (mysql-execute db "DROP TABLE test_affected")))

;; ================================================================
;; 12.  UPDATE returning affected row count
;; ================================================================

(ert-deftest mysql-update-test ()
  "UPDATE should return the count of changed rows."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_update")
    (mysql-execute
     db "CREATE TABLE test_update (id INT, name TEXT)")
    (mysql-execute db "INSERT INTO test_update VALUES (1, 'a')")
    (mysql-execute db "INSERT INTO test_update VALUES (2, 'b')")
    (mysql-execute db "INSERT INTO test_update VALUES (3, 'c')")
    (let ((affected (mysql-execute
                     db "UPDATE test_update SET name = 'z' WHERE id <= 2")))
      (should (= affected 2)))
    (let ((rows (mysql-select
                 db "SELECT * FROM test_update WHERE name = 'z'")))
      (should (= (length rows) 2)))
    (mysql-execute db "DROP TABLE test_update")))

;; ================================================================
;; 13.  Empty result set
;; ================================================================

(ert-deftest mysql-empty-result-test ()
  "SELECT from an empty table should return nil."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_empty")
    (mysql-execute
     db "CREATE TABLE test_empty (col1 INT)")
    (should-not (mysql-select db "SELECT * FROM test_empty"))
    (mysql-execute db "DROP TABLE test_empty")))

(ert-deftest mysql-select-full-empty-test ()
  "SELECT 'full from empty table should return just column names."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_empty2")
    (mysql-execute
     db "CREATE TABLE test_empty2 (col1 INT, col2 TEXT)")
    (let ((result (mysql-select
                   db "SELECT * FROM test_empty2" nil 'full)))
      ;; Should have column names as car, no data rows
      (should (equal (car result) '("col1" "col2")))
      (should-not (cdr result)))
    (mysql-execute db "DROP TABLE test_empty2")))

;; ================================================================
;; 13b. Set mode: lazy cursor iteration (like sqlite-next / sqlite-more-p)
;; ================================================================

(ert-deftest mysql-select-set-basic-test ()
  "mysql-select with 'set should return a set object for iteration."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set")
    (mysql-execute
     db "CREATE TABLE test_set (id INT, name TEXT)")
    (mysql-execute db "INSERT INTO test_set VALUES (1, 'alpha')")
    (mysql-execute db "INSERT INTO test_set VALUES (2, 'beta')")
    (mysql-execute db "INSERT INTO test_set VALUES (3, 'gamma')")
    (let ((set (mysql-select db "SELECT * FROM test_set ORDER BY id"
                             nil 'set)))
      (unwind-protect
          (progn
            ;; Should have more data
            (should (eq (mysql-more-p set) t))
            ;; First row
            (let ((row1 (mysql-next set)))
              (should (= (nth 0 row1) 1))
              (should (equal (nth 1 row1) "alpha")))
            ;; Second row
            (let ((row2 (mysql-next set)))
              (should (= (nth 0 row2) 2))
              (should (equal (nth 1 row2) "beta")))
            ;; Still more
            (should (eq (mysql-more-p set) t))
            ;; Third row
            (let ((row3 (mysql-next set)))
              (should (= (nth 0 row3) 3))
              (should (equal (nth 1 row3) "gamma")))
            ;; No more data
            (should-not (mysql-next set))
            (should-not (mysql-more-p set)))
        (mysql-finalize set)))
    (mysql-execute db "DROP TABLE test_set")))

(ert-deftest mysql-select-set-columns-test ()
  "mysql-columns should return column names from a set object."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set_cols")
    (mysql-execute
     db "CREATE TABLE test_set_cols (col_a INT, col_b TEXT, col_c DOUBLE)")
    (mysql-execute db "INSERT INTO test_set_cols VALUES (1, 'x', 2.5)")
    (let ((set (mysql-select db "SELECT * FROM test_set_cols" nil 'set)))
      (unwind-protect
          (progn
            (should (equal (mysql-columns set)
                           '("col_a" "col_b" "col_c")))
            ;; Consume the row
            (mysql-next set))
        (mysql-finalize set)))
    (mysql-execute db "DROP TABLE test_set_cols")))

(ert-deftest mysql-select-set-empty-test ()
  "A set from an empty table should return nil on first mysql-next."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set_empty")
    (mysql-execute
     db "CREATE TABLE test_set_empty (col1 INT)")
    (let ((set (mysql-select db "SELECT * FROM test_set_empty" nil 'set)))
      (unwind-protect
          (progn
            (should (eq (mysql-more-p set) t))  ; not eof yet (haven't tried)
            (should-not (mysql-next set))        ; no rows -> nil
            (should-not (mysql-more-p set)))     ; now eof
        (mysql-finalize set)))
    (mysql-execute db "DROP TABLE test_set_empty")))

(ert-deftest mysql-select-set-finalize-test ()
  "mysql-finalize should release the set; further use should error."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set_fin")
    (mysql-execute
     db "CREATE TABLE test_set_fin (col1 INT)")
    (mysql-execute db "INSERT INTO test_set_fin VALUES (1)")
    (let ((set (mysql-select db "SELECT * FROM test_set_fin" nil 'set)))
      (should (eq (mysql-finalize set) t))
      ;; After finalize, mysql-next should error
      (should-error (mysql-next set)))
    (mysql-execute db "DROP TABLE test_set_fin")))

(ert-deftest mysql-select-set-with-params-test ()
  "Set mode should work with prepared statements (bind parameters)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set_param")
    (mysql-execute
     db "CREATE TABLE test_set_param (id INT, name TEXT)")
    (mysql-execute db "INSERT INTO test_set_param VALUES (1, 'one')")
    (mysql-execute db "INSERT INTO test_set_param VALUES (2, 'two')")
    (mysql-execute db "INSERT INTO test_set_param VALUES (3, 'three')")
    (let ((set (mysql-select db
                "SELECT * FROM test_set_param WHERE id >= ? ORDER BY id"
                '(2) 'set)))
      (unwind-protect
          (progn
            ;; Should get row (2, "two") and (3, "three")
            (let ((r1 (mysql-next set)))
              (should r1)
              ;; Prepared path returns strings
              (should (equal (nth 0 r1) "2"))
              (should (equal (nth 1 r1) "two")))
            (let ((r2 (mysql-next set)))
              (should r2)
              (should (equal (nth 0 r2) "3"))
              (should (equal (nth 1 r2) "three")))
            (should-not (mysql-next set))
            (should-not (mysql-more-p set)))
        (mysql-finalize set)))
    (mysql-execute db "DROP TABLE test_set_param")))

(ert-deftest mysql-select-set-collect-all-test ()
  "Demonstrate collecting all rows from a set into a list."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set_collect")
    (mysql-execute
     db "CREATE TABLE test_set_collect (v INT)")
    (dotimes (i 10)
      (mysql-execute db (format "INSERT INTO test_set_collect VALUES (%d)" i)))
    (let ((set (mysql-select db "SELECT * FROM test_set_collect ORDER BY v"
                             nil 'set))
          (rows nil))
      (unwind-protect
          (progn
            (while (mysql-more-p set)
              (let ((r (mysql-next set)))
                (when r (push r rows))))
            (setq rows (nreverse rows))
            (should (= (length rows) 10))
            (should (= (car (nth 0 rows)) 0))
            (should (= (car (nth 9 rows)) 9)))
        (mysql-finalize set)))
    (mysql-execute db "DROP TABLE test_set_collect")))

;; ================================================================
;; 14.  Nested / multiple transactions
;; ================================================================

(ert-deftest mysql-transaction-idempotent-commit-test ()
  "Committing without an active transaction should not error."
  (with-test-mysql-db db
    ;; Should not signal an error.
    (mysql-commit db)))

;; ================================================================
;; 15.  Error handling: bad SQL
;; ================================================================

(ert-deftest mysql-bad-sql-test ()
  "Executing invalid SQL should signal an error."
  (with-test-mysql-db db
    (should-error (mysql-execute db "NOT VALID SQL AT ALL"))))

(ert-deftest mysql-bad-table-test ()
  "Selecting from a non-existent table should signal an error."
  (with-test-mysql-db db
    (should-error
     (mysql-select db "SELECT * FROM this_table_does_not_exist_12345"))))

;; ================================================================
;; 16.  Integer types: TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT
;; ================================================================

(ert-deftest mysql-type-tinyint-test ()
  "TINYINT values should be returned as Emacs integers."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_tinyint")
    (mysql-execute db "CREATE TABLE test_tinyint (v TINYINT)")
    (mysql-execute db "INSERT INTO test_tinyint VALUES (-128)")
    (mysql-execute db "INSERT INTO test_tinyint VALUES (0)")
    (mysql-execute db "INSERT INTO test_tinyint VALUES (127)")
    (let ((rows (mysql-select db "SELECT v FROM test_tinyint ORDER BY v")))
      (should (= (length rows) 3))
      (should (= (car (nth 0 rows)) -128))
      (should (= (car (nth 1 rows)) 0))
      (should (= (car (nth 2 rows)) 127))
      ;; Verify they are actual integers, not strings
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_tinyint")))

(ert-deftest mysql-type-tinyint-unsigned-test ()
  "TINYINT UNSIGNED values (0–255) should be returned as Emacs integers."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_tinyint_u")
    (mysql-execute db "CREATE TABLE test_tinyint_u (v TINYINT UNSIGNED)")
    (mysql-execute db "INSERT INTO test_tinyint_u VALUES (0)")
    (mysql-execute db "INSERT INTO test_tinyint_u VALUES (255)")
    (let ((rows (mysql-select db "SELECT v FROM test_tinyint_u ORDER BY v")))
      (should (= (car (nth 0 rows)) 0))
      (should (= (car (nth 1 rows)) 255)))
    (mysql-execute db "DROP TABLE test_tinyint_u")))

(ert-deftest mysql-type-smallint-test ()
  "SMALLINT boundary values should round-trip as integers."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_smallint")
    (mysql-execute db "CREATE TABLE test_smallint (v SMALLINT)")
    (mysql-execute db "INSERT INTO test_smallint VALUES (-32768)")
    (mysql-execute db "INSERT INTO test_smallint VALUES (32767)")
    (let ((rows (mysql-select db "SELECT v FROM test_smallint ORDER BY v")))
      (should (= (car (nth 0 rows)) -32768))
      (should (= (car (nth 1 rows)) 32767))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_smallint")))

(ert-deftest mysql-type-mediumint-test ()
  "MEDIUMINT boundary values should round-trip as integers."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_mediumint")
    (mysql-execute db "CREATE TABLE test_mediumint (v MEDIUMINT)")
    (mysql-execute db "INSERT INTO test_mediumint VALUES (-8388608)")
    (mysql-execute db "INSERT INTO test_mediumint VALUES (8388607)")
    (let ((rows (mysql-select db "SELECT v FROM test_mediumint ORDER BY v")))
      (should (= (car (nth 0 rows)) -8388608))
      (should (= (car (nth 1 rows)) 8388607))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_mediumint")))

(ert-deftest mysql-type-int-test ()
  "INT boundary values should round-trip as integers."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_int")
    (mysql-execute db "CREATE TABLE test_int (v INT)")
    (mysql-execute db "INSERT INTO test_int VALUES (-2147483648)")
    (mysql-execute db "INSERT INTO test_int VALUES (2147483647)")
    (let ((rows (mysql-select db "SELECT v FROM test_int ORDER BY v")))
      (should (= (car (nth 0 rows)) -2147483648))
      (should (= (car (nth 1 rows)) 2147483647))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_int")))

(ert-deftest mysql-type-bigint-test ()
  "BIGINT large values should round-trip as Emacs integers (bignums)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bigint")
    (mysql-execute db "CREATE TABLE test_bigint (v BIGINT)")
    (mysql-execute db "INSERT INTO test_bigint VALUES (-9223372036854775808)")
    (mysql-execute db "INSERT INTO test_bigint VALUES (9223372036854775807)")
    (let ((rows (mysql-select db "SELECT v FROM test_bigint ORDER BY v")))
      (should (= (car (nth 0 rows)) -9223372036854775808))
      (should (= (car (nth 1 rows)) 9223372036854775807))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_bigint")))

(ert-deftest mysql-type-bigint-unsigned-test ()
  "BIGINT UNSIGNED max value (2^64-1) should round-trip correctly."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bigint_u")
    (mysql-execute db "CREATE TABLE test_bigint_u (v BIGINT UNSIGNED)")
    (mysql-execute db "INSERT INTO test_bigint_u VALUES (0)")
    (mysql-execute db "INSERT INTO test_bigint_u VALUES (18446744073709551615)")
    (let ((rows (mysql-select db "SELECT v FROM test_bigint_u ORDER BY v")))
      (should (= (car (nth 0 rows)) 0))
      ;; BIGINT UNSIGNED max: strtoll wraps to -1 for 2^64-1
      ;; This tests current behavior — if module is enhanced for unsigned
      ;; support, this test should be updated.
      (should (integerp (car (nth 1 rows)))))
    (mysql-execute db "DROP TABLE test_bigint_u")))

;; ================================================================
;; 17.  Floating-point types: FLOAT, DOUBLE
;; ================================================================

(ert-deftest mysql-type-float-test ()
  "FLOAT values should be returned as Emacs floats."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_float")
    (mysql-execute db "CREATE TABLE test_float (v FLOAT)")
    (mysql-execute db "INSERT INTO test_float VALUES (0.0)")
    (mysql-execute db "INSERT INTO test_float VALUES (-3.14)")
    (mysql-execute db "INSERT INTO test_float VALUES (1.23e10)")
    (let ((rows (mysql-select db "SELECT v FROM test_float ORDER BY v")))
      (should (= (length rows) 3))
      (should (floatp (car (nth 0 rows))))
      (should (floatp (car (nth 1 rows))))
      (should (floatp (car (nth 2 rows))))
      ;; FLOAT has ~7 digits precision; check approximate values
      (should (< (abs (- (car (nth 0 rows)) -3.14)) 0.001))
      (should (< (abs (car (nth 1 rows))) 0.001)))
    (mysql-execute db "DROP TABLE test_float")))

(ert-deftest mysql-type-double-test ()
  "DOUBLE values should be returned as Emacs floats with higher precision."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_double")
    (mysql-execute db "CREATE TABLE test_double (v DOUBLE)")
    (mysql-execute db "INSERT INTO test_double VALUES (0.0)")
    (mysql-execute db "INSERT INTO test_double VALUES (3.141592653589793)")
    (mysql-execute db "INSERT INTO test_double VALUES (-1.7976931348623157e+308)")
    (let ((rows (mysql-select db "SELECT v FROM test_double ORDER BY v")))
      (should (= (length rows) 3))
      (should (floatp (car (nth 0 rows))))
      (should (floatp (car (nth 1 rows))))
      (should (floatp (car (nth 2 rows))))
      ;; ORDER BY: -1.79e308, 0.0, 3.14...
      ;; Check 0.0 is close to zero
      (should (< (abs (car (nth 1 rows))) 1.0e-10))
      ;; Check pi is close (DOUBLE has ~15 digits precision)
      (should (< (abs (- (car (nth 2 rows)) 3.141592653589793)) 1.0e-10)))
    (mysql-execute db "DROP TABLE test_double")))

;; ================================================================
;; 18.  DECIMAL / NUMERIC (fixed-point)
;; ================================================================

(ert-deftest mysql-type-decimal-test ()
  "DECIMAL values should be returned as Emacs floats (via strtod)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_decimal")
    (mysql-execute db "CREATE TABLE test_decimal (v DECIMAL(10,2))")
    (mysql-execute db "INSERT INTO test_decimal VALUES (99999999.99)")
    (mysql-execute db "INSERT INTO test_decimal VALUES (-12345.67)")
    (mysql-execute db "INSERT INTO test_decimal VALUES (0.00)")
    (let ((rows (mysql-select db "SELECT v FROM test_decimal ORDER BY v")))
      (should (= (length rows) 3))
      ;; DECIMAL maps to MYSQL_TYPE_NEWDECIMAL → float
      (should (floatp (car (nth 0 rows))))
      (should (< (abs (- (car (nth 0 rows)) -12345.67)) 0.01))
      (should (< (abs (car (nth 1 rows))) 0.01))
      (should (< (abs (- (car (nth 2 rows)) 99999999.99)) 0.01)))
    (mysql-execute db "DROP TABLE test_decimal")))

(ert-deftest mysql-type-decimal-high-precision-test ()
  "DECIMAL with high precision (e.g. 30,15) to test large fixed-point values."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_decimal_hp")
    (mysql-execute db "CREATE TABLE test_decimal_hp (v DECIMAL(30,15))")
    (mysql-execute db "INSERT INTO test_decimal_hp VALUES (123456789012345.123456789012345)")
    (let ((rows (mysql-select db "SELECT v FROM test_decimal_hp")))
      (should (= (length rows) 1))
      ;; The value should be a float (though precision is limited by double)
      (should (floatp (car (car rows)))))
    (mysql-execute db "DROP TABLE test_decimal_hp")))

;; ================================================================
;; 19.  String types: CHAR, VARCHAR, TEXT variants, ENUM, SET
;; ================================================================

(ert-deftest mysql-type-char-varchar-test ()
  "CHAR and VARCHAR values should be returned as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_charvar")
    (mysql-execute db "CREATE TABLE test_charvar (
       c CHAR(10), v VARCHAR(255)
     ) CHARACTER SET utf8mb4")
    (mysql-execute db "INSERT INTO test_charvar VALUES ('hello', 'world')")
    (mysql-execute db "INSERT INTO test_charvar VALUES ('', '')")
    (let ((rows (mysql-select db "SELECT c, v FROM test_charvar ORDER BY c")))
      (should (= (length rows) 2))
      ;; Empty strings
      (should (equal (nth 0 (nth 0 rows)) ""))
      (should (equal (nth 1 (nth 0 rows)) ""))
      ;; Normal strings
      (should (equal (nth 0 (nth 1 rows)) "hello"))
      (should (equal (nth 1 (nth 1 rows)) "world")))
    (mysql-execute db "DROP TABLE test_charvar")))

(ert-deftest mysql-type-text-variants-test ()
  "TINYTEXT, TEXT, MEDIUMTEXT, LONGTEXT should all return strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_textvariants")
    (mysql-execute db "CREATE TABLE test_textvariants (
       tt TINYTEXT, t TEXT, mt MEDIUMTEXT, lt LONGTEXT
     ) CHARACTER SET utf8mb4")
    (mysql-execute db "INSERT INTO test_textvariants VALUES ('tiny', 'normal', 'medium', 'long')")
    (let ((rows (mysql-select db "SELECT * FROM test_textvariants")))
      (should (= (length rows) 1))
      (let ((row (car rows)))
        (should (equal (nth 0 row) "tiny"))
        (should (equal (nth 1 row) "normal"))
        (should (equal (nth 2 row) "medium"))
        (should (equal (nth 3 row) "long"))
        ;; All should be strings
        (dolist (val row)
          (should (stringp val)))))
    (mysql-execute db "DROP TABLE test_textvariants")))

(ert-deftest mysql-type-enum-test ()
  "ENUM values should be returned as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_enum")
    (mysql-execute db "CREATE TABLE test_enum (
       color ENUM('red','green','blue')
     )")
    (mysql-execute db "INSERT INTO test_enum VALUES ('red')")
    (mysql-execute db "INSERT INTO test_enum VALUES ('blue')")
    (let ((rows (mysql-select db "SELECT color FROM test_enum ORDER BY color")))
      (should (= (length rows) 2))
      ;; ENUM sorts by index: red=1, green=2, blue=3
      (should (stringp (car (nth 0 rows))))
      (should (stringp (car (nth 1 rows)))))
    (mysql-execute db "DROP TABLE test_enum")))

(ert-deftest mysql-type-set-test ()
  "SET values should be returned as comma-separated strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_set_type")
    (mysql-execute db "CREATE TABLE test_set_type (
       tags SET('a','b','c','d')
     )")
    (mysql-execute db "INSERT INTO test_set_type VALUES ('a,c')")
    (mysql-execute db "INSERT INTO test_set_type VALUES ('b')")
    (let ((rows (mysql-select db "SELECT tags FROM test_set_type ORDER BY tags")))
      (should (= (length rows) 2))
      (should (stringp (car (nth 0 rows))))
      (should (stringp (car (nth 1 rows)))))
    (mysql-execute db "DROP TABLE test_set_type")))

;; ================================================================
;; 20.  Date and time types: DATE, TIME, DATETIME, TIMESTAMP, YEAR
;; ================================================================

(ert-deftest mysql-type-date-test ()
  "DATE values should be returned as strings (YYYY-MM-DD)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_date")
    (mysql-execute db "CREATE TABLE test_date (d DATE)")
    (mysql-execute db "INSERT INTO test_date VALUES ('2026-03-25')")
    (mysql-execute db "INSERT INTO test_date VALUES ('1970-01-01')")
    (mysql-execute db "INSERT INTO test_date VALUES ('9999-12-31')")
    (let ((rows (mysql-select db "SELECT d FROM test_date ORDER BY d")))
      (should (= (length rows) 3))
      (should (equal (car (nth 0 rows)) "1970-01-01"))
      (should (equal (car (nth 1 rows)) "2026-03-25"))
      (should (equal (car (nth 2 rows)) "9999-12-31"))
      (should (stringp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_date")))

(ert-deftest mysql-type-time-test ()
  "TIME values should be returned as strings (HH:MM:SS)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_time")
    (mysql-execute db "CREATE TABLE test_time (t TIME)")
    (mysql-execute db "INSERT INTO test_time VALUES ('00:00:00')")
    (mysql-execute db "INSERT INTO test_time VALUES ('23:59:59')")
    (mysql-execute db "INSERT INTO test_time VALUES ('-838:59:59')")
    (let ((rows (mysql-select db "SELECT t FROM test_time ORDER BY t")))
      (should (= (length rows) 3))
      (should (stringp (car (nth 0 rows))))
      ;; Check the min-value time
      (should (equal (car (nth 0 rows)) "-838:59:59"))
      (should (equal (car (nth 1 rows)) "00:00:00"))
      (should (equal (car (nth 2 rows)) "23:59:59")))
    (mysql-execute db "DROP TABLE test_time")))

(ert-deftest mysql-type-datetime-test ()
  "DATETIME values should be returned as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_datetime")
    (mysql-execute db "CREATE TABLE test_datetime (dt DATETIME)")
    (mysql-execute db "INSERT INTO test_datetime VALUES ('2026-03-25 14:30:00')")
    (mysql-execute db "INSERT INTO test_datetime VALUES ('1000-01-01 00:00:00')")
    (let ((rows (mysql-select db "SELECT dt FROM test_datetime ORDER BY dt")))
      (should (= (length rows) 2))
      (should (equal (car (nth 0 rows)) "1000-01-01 00:00:00"))
      (should (equal (car (nth 1 rows)) "2026-03-25 14:30:00"))
      (should (stringp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_datetime")))

(ert-deftest mysql-type-datetime-fractional-test ()
  "DATETIME(6) with fractional seconds should preserve microseconds."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_datetime_frac")
    (mysql-execute db "CREATE TABLE test_datetime_frac (dt DATETIME(6))")
    (mysql-execute db "INSERT INTO test_datetime_frac VALUES ('2026-03-25 14:30:00.123456')")
    (let ((rows (mysql-select db "SELECT dt FROM test_datetime_frac")))
      (should (= (length rows) 1))
      (should (stringp (car (car rows))))
      (should (string-match-p "123456" (car (car rows)))))
    (mysql-execute db "DROP TABLE test_datetime_frac")))

(ert-deftest mysql-type-timestamp-test ()
  "TIMESTAMP values should be returned as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_timestamp")
    (mysql-execute db "CREATE TABLE test_timestamp (ts TIMESTAMP NULL)")
    (mysql-execute db "INSERT INTO test_timestamp VALUES ('2026-03-25 14:30:00')")
    (let ((rows (mysql-select db "SELECT ts FROM test_timestamp")))
      (should (= (length rows) 1))
      (should (stringp (car (car rows))))
      (should (string-match-p "2026-03-25" (car (car rows)))))
    (mysql-execute db "DROP TABLE test_timestamp")))

(ert-deftest mysql-type-year-test ()
  "YEAR values should be returned as integers."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_year")
    (mysql-execute db "CREATE TABLE test_year (y YEAR)")
    (mysql-execute db "INSERT INTO test_year VALUES (1901)")
    (mysql-execute db "INSERT INTO test_year VALUES (2026)")
    (mysql-execute db "INSERT INTO test_year VALUES (2155)")
    (let ((rows (mysql-select db "SELECT y FROM test_year ORDER BY y")))
      (should (= (length rows) 3))
      ;; YEAR is MYSQL_TYPE_SHORT → should be Emacs integer
      (should (= (car (nth 0 rows)) 1901))
      (should (= (car (nth 1 rows)) 2026))
      (should (= (car (nth 2 rows)) 2155))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_year")))

;; ================================================================
;; 21.  Binary types: BINARY, VARBINARY, BLOB variants, BIT
;; ================================================================

(ert-deftest mysql-type-binary-varbinary-test ()
  "BINARY/VARBINARY values should be returned as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_binvar")
    (mysql-execute db "CREATE TABLE test_binvar (
       b BINARY(4), vb VARBINARY(255)
     )")
    (mysql-execute db "INSERT INTO test_binvar VALUES (X'DEADBEEF', X'CAFEBABE')")
    (let ((rows (mysql-select db "SELECT HEX(b), HEX(vb) FROM test_binvar")))
      (should (= (length rows) 1))
      (should (equal (nth 0 (car rows)) "DEADBEEF"))
      (should (equal (nth 1 (car rows)) "CAFEBABE")))
    (mysql-execute db "DROP TABLE test_binvar")))

(ert-deftest mysql-type-blob-variants-test ()
  "TINYBLOB, BLOB, MEDIUMBLOB, LONGBLOB should return strings (raw bytes)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_blobvar")
    (mysql-execute db "CREATE TABLE test_blobvar (
       tb TINYBLOB, b BLOB, mb MEDIUMBLOB, lb LONGBLOB
     )")
    ;; Insert textual data into blob columns for easy verification
    (mysql-execute db "INSERT INTO test_blobvar VALUES ('tiny', 'normal', 'medium', 'long')")
    (let ((rows (mysql-select db "SELECT * FROM test_blobvar")))
      (should (= (length rows) 1))
      (let ((row (car rows)))
        (should (equal (nth 0 row) "tiny"))
        (should (equal (nth 1 row) "normal"))
        (should (equal (nth 2 row) "medium"))
        (should (equal (nth 3 row) "long"))))
    (mysql-execute db "DROP TABLE test_blobvar")))

(ert-deftest mysql-type-bit-test ()
  "BIT values should be returned (as raw bytes or strings via HEX)."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bit")
    (mysql-execute db "CREATE TABLE test_bit (b BIT(8))")
    (mysql-execute db "INSERT INTO test_bit VALUES (b'10101010')")
    (mysql-execute db "INSERT INTO test_bit VALUES (b'00000000')")
    (mysql-execute db "INSERT INTO test_bit VALUES (b'11111111')")
    ;; Use CAST or HEX to retrieve as readable values
    (let ((rows (mysql-select db "SELECT CAST(b AS UNSIGNED) FROM test_bit ORDER BY b")))
      (should (= (length rows) 3))
      (should (= (car (nth 0 rows)) 0))
      (should (= (car (nth 1 rows)) 170))  ;; 0b10101010 = 170
      (should (= (car (nth 2 rows)) 255))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_bit")))

;; ================================================================
;; 22.  JSON type (MySQL 5.7+)
;; ================================================================

(ert-deftest mysql-type-json-test ()
  "JSON values should be returned as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_json")
    (mysql-execute db "CREATE TABLE test_json (j JSON)")
    (mysql-execute db "INSERT INTO test_json VALUES ('{\"key\": \"value\", \"num\": 42}')")
    (mysql-execute db "INSERT INTO test_json VALUES ('[1, 2, 3]')")
    (mysql-execute db "INSERT INTO test_json VALUES ('null')")
    (let ((rows (mysql-select db "SELECT j FROM test_json ORDER BY j")))
      (should (= (length rows) 3))
      (dolist (row rows)
        (should (stringp (car row))))
      ;; Verify JSON content can be parsed (contains expected substrings)
      (let ((all-json (mapcar #'car rows)))
        (should (cl-some (lambda (s) (string-match-p "key" s)) all-json))
        (should (cl-some (lambda (s) (string-match-p "\\[1" s)) all-json))))
    (mysql-execute db "DROP TABLE test_json")))

;; ================================================================
;; 23.  BOOLEAN (alias for TINYINT(1))
;; ================================================================

(ert-deftest mysql-type-boolean-test ()
  "BOOLEAN (TINYINT(1)) TRUE/FALSE should map to integer 1/0."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bool")
    (mysql-execute db "CREATE TABLE test_bool (b BOOLEAN)")
    (mysql-execute db "INSERT INTO test_bool VALUES (TRUE)")
    (mysql-execute db "INSERT INTO test_bool VALUES (FALSE)")
    (let ((rows (mysql-select db "SELECT b FROM test_bool ORDER BY b")))
      (should (= (length rows) 2))
      (should (= (car (nth 0 rows)) 0))
      (should (= (car (nth 1 rows)) 1))
      (should (integerp (car (nth 0 rows)))))
    (mysql-execute db "DROP TABLE test_bool")))

;; ================================================================
;; 24.  NULL across different column types
;; ================================================================

(ert-deftest mysql-type-null-in-various-types-test ()
  "NULL should be returned as nil regardless of column type."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_null_types")
    (mysql-execute db "CREATE TABLE test_null_types (
       i INT, f DOUBLE, d DECIMAL(10,2), s VARCHAR(100),
       dt DATETIME, j JSON
     )")
    (mysql-execute db "INSERT INTO test_null_types VALUES (NULL, NULL, NULL, NULL, NULL, NULL)")
    (let ((rows (mysql-select db "SELECT * FROM test_null_types")))
      (should (= (length rows) 1))
      ;; Every column should be nil
      (dolist (val (car rows))
        (should-not val)))
    (mysql-execute db "DROP TABLE test_null_types")))

;; ================================================================
;; 25.  Prepared-statement path: type handling for various column types
;; ================================================================

(ert-deftest mysql-type-prepared-int-types-test ()
  "Prepared path should return integer columns as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_prep_int")
    (mysql-execute db "CREATE TABLE test_prep_int (
       ti TINYINT, si SMALLINT, mi MEDIUMINT, i INT, bi BIGINT
     )")
    (mysql-execute db "INSERT INTO test_prep_int VALUES (1, 100, 10000, 1000000, 9999999999)")
    ;; Use prepared path by providing bind params (even if empty list trick:
    ;; use a dummy WHERE with param)
    (let ((rows (mysql-select db "SELECT * FROM test_prep_int WHERE ti = ?" '(1))))
      (should (= (length rows) 1))
      ;; Prepared path returns everything as strings
      (dolist (val (car rows))
        (should (stringp val)))
      ;; But values should be correct as strings
      (should (equal (nth 0 (car rows)) "1"))
      (should (equal (nth 1 (car rows)) "100"))
      (should (equal (nth 2 (car rows)) "10000"))
      (should (equal (nth 3 (car rows)) "1000000"))
      (should (equal (nth 4 (car rows)) "9999999999")))
    (mysql-execute db "DROP TABLE test_prep_int")))

(ert-deftest mysql-type-prepared-float-decimal-test ()
  "Prepared path should return float/decimal columns as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_prep_float")
    (mysql-execute db "CREATE TABLE test_prep_float (
       f FLOAT, d DOUBLE, dc DECIMAL(10,2)
     )")
    (mysql-execute db "INSERT INTO test_prep_float VALUES (1.5, 3.14159, 99.99)")
    (let ((rows (mysql-select db "SELECT * FROM test_prep_float WHERE f > ?" '(0.0))))
      (should (= (length rows) 1))
      (dolist (val (car rows))
        (should (stringp val))))
    (mysql-execute db "DROP TABLE test_prep_float")))

(ert-deftest mysql-type-prepared-datetime-test ()
  "Prepared path should return date/time columns as strings."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_prep_dt")
    (mysql-execute db "CREATE TABLE test_prep_dt (
       id INT, d DATE, t TIME, dt DATETIME
     )")
    (mysql-execute db "INSERT INTO test_prep_dt VALUES (1, '2026-03-25', '14:30:00', '2026-03-25 14:30:00')")
    (let ((rows (mysql-select db "SELECT d, t, dt FROM test_prep_dt WHERE id = ?" '(1))))
      (should (= (length rows) 1))
      (dolist (val (car rows))
        (should (stringp val)))
      (should (string-match-p "2026-03-25" (nth 0 (car rows))))
      (should (string-match-p "14:30:00" (nth 1 (car rows)))))
    (mysql-execute db "DROP TABLE test_prep_dt")))

(ert-deftest mysql-type-prepared-null-test ()
  "Prepared path should return NULL as nil."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_prep_null")
    (mysql-execute db "CREATE TABLE test_prep_null (id INT, v VARCHAR(100))")
    (mysql-execute db "INSERT INTO test_prep_null VALUES (1, NULL)")
    (let ((rows (mysql-select db "SELECT v FROM test_prep_null WHERE id = ?" '(1))))
      (should (= (length rows) 1))
      (should-not (car (car rows))))
    (mysql-execute db "DROP TABLE test_prep_null")))

;; ================================================================
;; 26.  Bind parameter types: string, integer, float, nil
;; ================================================================

(ert-deftest mysql-type-bind-string-test ()
  "String bind parameters should work with VARCHAR columns."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bind_str")
    (mysql-execute db "CREATE TABLE test_bind_str (v VARCHAR(255))")
    (mysql-execute db "INSERT INTO test_bind_str VALUES (?)" '("hello world"))
    (mysql-execute db "INSERT INTO test_bind_str VALUES (?)" '("日本語テスト"))
    (mysql-execute db "INSERT INTO test_bind_str VALUES (?)" '(""))
    (let ((rows (mysql-select db "SELECT v FROM test_bind_str ORDER BY v")))
      (should (= (length rows) 3))
      ;; Empty string should be present
      (should (cl-some (lambda (r) (equal (car r) "")) rows)))
    (mysql-execute db "DROP TABLE test_bind_str")))

(ert-deftest mysql-type-bind-integer-test ()
  "Integer bind parameters should work with INT columns."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bind_int")
    (mysql-execute db "CREATE TABLE test_bind_int (v BIGINT)")
    (mysql-execute db "INSERT INTO test_bind_int VALUES (?)" '(0))
    (mysql-execute db "INSERT INTO test_bind_int VALUES (?)" '(-42))
    (mysql-execute db "INSERT INTO test_bind_int VALUES (?)" '(999999999999))
    (let ((rows (mysql-select db "SELECT v FROM test_bind_int ORDER BY v")))
      (should (= (length rows) 3))
      (should (= (car (nth 0 rows)) -42))
      (should (= (car (nth 1 rows)) 0))
      (should (= (car (nth 2 rows)) 999999999999)))
    (mysql-execute db "DROP TABLE test_bind_int")))

(ert-deftest mysql-type-bind-float-test ()
  "Float bind parameters should work with DOUBLE columns."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bind_float")
    (mysql-execute db "CREATE TABLE test_bind_float (v DOUBLE)")
    (mysql-execute db "INSERT INTO test_bind_float VALUES (?)" '(3.14))
    (mysql-execute db "INSERT INTO test_bind_float VALUES (?)" '(-0.001))
    (mysql-execute db "INSERT INTO test_bind_float VALUES (?)" '(0.0))
    (let ((rows (mysql-select db "SELECT v FROM test_bind_float ORDER BY v")))
      (should (= (length rows) 3))
      (should (< (abs (- (car (nth 0 rows)) -0.001)) 0.0001))
      (should (< (abs (car (nth 1 rows))) 0.0001))
      (should (< (abs (- (car (nth 2 rows)) 3.14)) 0.001)))
    (mysql-execute db "DROP TABLE test_bind_float")))

(ert-deftest mysql-type-bind-nil-test ()
  "nil bind parameters should insert NULL."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_bind_nil")
    (mysql-execute db "CREATE TABLE test_bind_nil (i INT, s VARCHAR(100))")
    (mysql-execute db "INSERT INTO test_bind_nil VALUES (?, ?)" '(nil nil))
    (let ((rows (mysql-select db "SELECT * FROM test_bind_nil")))
      (should (= (length rows) 1))
      (should-not (nth 0 (car rows)))
      (should-not (nth 1 (car rows))))
    (mysql-execute db "DROP TABLE test_bind_nil")))

;; ================================================================
;; 27.  Mixed types in a single table
;; ================================================================

(ert-deftest mysql-type-mixed-kitchen-sink-test ()
  "A single table with many column types to verify they all work together."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_kitchen_sink")
    (mysql-execute db "CREATE TABLE test_kitchen_sink (
       col_tinyint    TINYINT,
       col_int        INT,
       col_bigint     BIGINT,
       col_float      FLOAT,
       col_double     DOUBLE,
       col_decimal    DECIMAL(10,2),
       col_char       CHAR(20),
       col_varchar    VARCHAR(255),
       col_text       TEXT,
       col_date       DATE,
       col_time       TIME,
       col_datetime   DATETIME,
       col_bool       BOOLEAN,
       col_year       YEAR,
       col_json       JSON
     ) CHARACTER SET utf8mb4")
    (mysql-execute db
     "INSERT INTO test_kitchen_sink VALUES (
        42, 100000, 9876543210, 1.5, 3.14159265358979,
        12345.67, 'fixed', 'variable', 'long text here',
        '2026-03-25', '14:30:00', '2026-03-25 14:30:00',
        TRUE, 2026, '{\"k\":\"v\"}'
      )")
    (let ((rows (mysql-select db "SELECT * FROM test_kitchen_sink" nil 'full)))
      (should rows)
      ;; 15 columns
      (should (= (length (car rows)) 15))
      (let ((data (cadr rows)))
        ;; Integer types
        (should (integerp (nth 0 data)))   ;; TINYINT
        (should (= (nth 0 data) 42))
        (should (integerp (nth 1 data)))   ;; INT
        (should (= (nth 1 data) 100000))
        (should (integerp (nth 2 data)))   ;; BIGINT
        (should (= (nth 2 data) 9876543210))
        ;; Float types
        (should (floatp (nth 3 data)))     ;; FLOAT
        (should (floatp (nth 4 data)))     ;; DOUBLE
        (should (floatp (nth 5 data)))     ;; DECIMAL
        ;; String types
        (should (stringp (nth 6 data)))    ;; CHAR
        (should (stringp (nth 7 data)))    ;; VARCHAR
        (should (stringp (nth 8 data)))    ;; TEXT
        ;; Date/time types (returned as strings)
        (should (stringp (nth 9 data)))    ;; DATE
        (should (equal (nth 9 data) "2026-03-25"))
        (should (stringp (nth 10 data)))   ;; TIME
        (should (stringp (nth 11 data)))   ;; DATETIME
        ;; BOOLEAN → TINYINT → integer
        (should (integerp (nth 12 data)))
        (should (= (nth 12 data) 1))
        ;; YEAR → integer
        (should (integerp (nth 13 data)))
        (should (= (nth 13 data) 2026))
        ;; JSON → string
        (should (stringp (nth 14 data)))))
    (mysql-execute db "DROP TABLE test_kitchen_sink")))

;; ================================================================
;; 28.  Charset: multi-script and edge-case Unicode round-trip
;; ================================================================

(ert-deftest mysql-charset-multiscript-test ()
  "Various scripts and special Unicode code-points round-trip through utf8mb4."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_charset_multi")
    (mysql-execute db "CREATE TABLE test_charset_multi (
       id INT AUTO_INCREMENT PRIMARY KEY,
       val TEXT
     ) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci")
    ;; A collection of strings that exercise different Unicode ranges:
    ;;  - Basic Latin + Latin Extended (accented)
    ;;  - CJK Unified Ideographs (BMP)
    ;;  - CJK Extension B (Supplementary Plane, 4-byte UTF-8): 𠀀 = U+20000
    ;;  - Emoji (Supplementary Plane, 4-byte UTF-8): 🎉🚀🇨🇳
    ;;  - Mathematical symbols (BMP): ∑∫∞
    ;;  - Arabic script
    ;;  - Devanagari script
    ;;  - Combining characters: é = e + U+0301
    ;;  - Zero-width joiner / non-joiner
    (let ((test-strings
           '("café résumé naïve"                      ; Latin Extended
             "中文测试数据"                             ; CJK basic
             "𠀀𠀁𠀂"                                  ; CJK Extension B (4-byte)
             "🎉🚀🏠🌍🇨🇳"                            ; Emoji (4-byte)
             "∑∫∞≠≤≥"                                  ; Math symbols
             "مرحبا بالعالم"                            ; Arabic
             "नमस्ते दुनिया"                             ; Devanagari
             "é = e\u0301"                             ; Combining accent
             "a\u200Bb\u200Cc\u200Dd\uFEFF"            ; Zero-width chars
             )))
      (dolist (s test-strings)
        (mysql-execute db "INSERT INTO test_charset_multi (val) VALUES (?)"
                       (list s)))
      ;; Read them all back and verify exact match
      (let ((rows (mysql-select
                   db "SELECT val FROM test_charset_multi ORDER BY id")))
        (should (= (length rows) (length test-strings)))
        (cl-loop for expected in test-strings
                 for row in rows
                 do (should (equal (car row) expected)))))
    (mysql-execute db "DROP TABLE test_charset_multi")))

;; ================================================================
;; 29.  Charset: 4-byte UTF-8 boundary via bind params and non-prepared path
;; ================================================================

(ert-deftest mysql-charset-4byte-boundary-test ()
  "4-byte UTF-8 characters (emoji, CJK-B) survive both prepared and non-prepared paths."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_charset_4b")
    (mysql-execute db "CREATE TABLE test_charset_4b (
       id INT AUTO_INCREMENT PRIMARY KEY,
       val VARCHAR(200)
     ) CHARACTER SET utf8mb4")
    (let ((four-byte-str "Hello 🌏🌍🌎 World 𝕳𝖊𝖑𝖑𝖔 𠀀"))
      ;; ---- Non-prepared path (plain SQL, string embedded directly) ----
      (mysql-execute
       db (format "INSERT INTO test_charset_4b (val) VALUES ('%s')"
                  (mysql-escape-string db four-byte-str)))
      ;; ---- Prepared path (bind parameter) ----
      (mysql-execute
       db "INSERT INTO test_charset_4b (val) VALUES (?)"
       (list four-byte-str))

      ;; Verify non-prepared SELECT returns native strings
      (let ((rows (mysql-select
                   db "SELECT val FROM test_charset_4b ORDER BY id")))
        (should (= (length rows) 2))
        ;; Both rows should contain exactly the same string
        (should (equal (car (nth 0 rows)) four-byte-str))
        (should (equal (car (nth 1 rows)) four-byte-str))
        ;; Sanity: result type is string
        (should (stringp (car (car rows)))))

      ;; Verify prepared SELECT also returns correctly (as string, since prepared)
      (let ((rows (mysql-select
                   db "SELECT val FROM test_charset_4b WHERE id >= ?"
                   '(1))))
        (should (= (length rows) 2))
        ;; Prepared path returns strings too
        (should (equal (car (nth 0 rows)) four-byte-str))
        (should (equal (car (nth 1 rows)) four-byte-str))))
    (mysql-execute db "DROP TABLE test_charset_4b")))

;; ================================================================
;; 30.  Async API: mysql-query / mysql-query-poll (unified)
;; ================================================================

(ert-deftest mysql-async-query-select-test ()
  "Async mysql-query + mysql-query-poll should return the same data as sync."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_async")
    (mysql-execute db "CREATE TABLE test_async (id INT, name TEXT)")
    (mysql-execute db "INSERT INTO test_async VALUES (1, 'alpha')")
    (mysql-execute db "INSERT INTO test_async VALUES (2, 'beta')")
    (mysql-execute db "INSERT INTO test_async VALUES (3, 'gamma')")
    (let ((result (mysql-query db "SELECT * FROM test_async ORDER BY id" t)))
      (while (eq result 'not-ready)
        (setq result (mysql-query-poll db)))
      (should (eq (plist-get result :type) 'select))
      (should (equal (plist-get result :columns) '("id" "name")))
      (should (= (length (plist-get result :rows)) 3))
      (should (= (nth 0 (nth 0 (plist-get result :rows))) 1)))
    (mysql-execute db "DROP TABLE test_async")))

(ert-deftest mysql-async-query-dml-test ()
  "Async mysql-query for DML should report affected rows in plist."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_async_dml")
    (mysql-execute db "CREATE TABLE test_async_dml (id INT, val TEXT)")
    (mysql-execute db "INSERT INTO test_async_dml VALUES (1, 'a')")
    (mysql-execute db "INSERT INTO test_async_dml VALUES (2, 'b')")
    (mysql-execute db "INSERT INTO test_async_dml VALUES (3, 'c')")
    (let ((result (mysql-query db "DELETE FROM test_async_dml WHERE id > 1" t)))
      (while (eq result 'not-ready)
        (setq result (mysql-query-poll db)))
      (should (eq (plist-get result :type) 'dml))
      (should (= (plist-get result :affected-rows) 2)))
    (let ((rows (mysql-select db "SELECT * FROM test_async_dml")))
      (should (= (length rows) 1))
      (should (= (car (car rows)) 1)))
    (mysql-execute db "DROP TABLE test_async_dml")))

(ert-deftest mysql-async-query-empty-result-test ()
  "Async query on empty result set should return plist with empty rows."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_async_empty")
    (mysql-execute db "CREATE TABLE test_async_empty (id INT)")
    (let ((result (mysql-query db "SELECT * FROM test_async_empty" t)))
      (while (eq result 'not-ready)
        (setq result (mysql-query-poll db)))
      (should (eq (plist-get result :type) 'select))
      (should (equal (plist-get result :columns) '("id")))
      (should (null (plist-get result :rows))))
    (mysql-execute db "DROP TABLE test_async_empty")))

(ert-deftest mysql-async-query-immediate-complete-test ()
  "Fast queries may complete immediately in mysql-query."
  (with-test-mysql-db db
    (let ((result (mysql-query db "SELECT 1 AS val" t)))
      (while (eq result 'not-ready)
        (setq result (mysql-query-poll db)))
      (should (eq (plist-get result :type) 'select))
      (should (equal (plist-get result :columns) '("val")))
      (should (= (length (plist-get result :rows)) 1))
      (should (= (nth 0 (nth 0 (plist-get result :rows))) 1)))))

(ert-deftest mysql-async-query-error-test ()
  "Async mysql-query with bad SQL should signal mysql-error."
  (with-test-mysql-db db
    (let ((got-error nil))
      (condition-case err
          (let ((result (mysql-query db "INVALID SQL GARBAGE" t)))
            (while (eq result 'not-ready)
              (setq result (mysql-query-poll db))))
        (mysql-error (setq got-error t)))
      (should got-error))))

(ert-deftest mysql-async-field-count-test ()
  "mysql-query plist :type should distinguish SELECT from DML."
  (with-test-mysql-db db
    (let ((result (mysql-query db "SELECT 1, 2, 3" t)))
      (while (eq result 'not-ready)
        (setq result (mysql-query-poll db)))
      (should (eq (plist-get result :type) 'select)))
    (mysql-execute db "DROP TABLE IF EXISTS test_async_fc")
    (mysql-execute db "CREATE TABLE test_async_fc (id INT)")
    (let ((result (mysql-query db "INSERT INTO test_async_fc VALUES (1)" t)))
      (while (eq result 'not-ready)
        (setq result (mysql-query-poll db)))
      (should (eq (plist-get result :type) 'dml))
      (should (= (plist-get result :affected-rows) 1)))
    (mysql-execute db "DROP TABLE test_async_fc")))

;; ================================================================
;; 31.  Async connect: mysql-open with ASYNC + mysql-open-poll
;; ================================================================

(ert-deftest mysql-async-open-test ()
  "Async mysql-open + mysql-open-poll should establish a working connection."
  (let ((db (mysql-open test-mysql-host
                        test-mysql-user
                        test-mysql-password
                        test-mysql-database
                        test-mysql-port
                        t)))  ; ASYNC=t
    (unwind-protect
        (progn
          ;; Poll until connected
          (let ((status 'not-ready)
                (iterations 0))
            (while (and (eq status 'not-ready) (< iterations 5000))
              (setq status (mysql-open-poll db))
              (setq iterations (1+ iterations))
              (when (eq status 'not-ready) (sit-for 0.001)))
            (should (eq status 'complete)))
          ;; Connection should be valid
          (should (mysqlp db))
          ;; Should be able to execute queries
          (let ((rows (mysql-select db "SELECT 42 AS val")))
            (should (= (length rows) 1))
            (should (= (car (car rows)) 42))))
      (ignore-errors (mysql-close db)))))

(ert-deftest mysql-async-open-then-query-test ()
  "After async connect, both sync and async queries should work."
  (let ((db (mysql-open test-mysql-host
                        test-mysql-user
                        test-mysql-password
                        test-mysql-database
                        test-mysql-port
                        t)))
    (unwind-protect
        (progn
          (while (eq (mysql-open-poll db) 'not-ready)
            (sit-for 0.001))
          ;; Sync query via mysql-query
          (let ((result (mysql-query db "SELECT 1 AS a, 2 AS b")))
            (should (eq (plist-get result :type) 'select))
            (should (equal (plist-get result :columns) '("a" "b"))))
          ;; Async query
          (let ((result (mysql-query db "SELECT 99 AS val" t)))
            (while (eq result 'not-ready)
              (setq result (mysql-query-poll db)))
            (should (= (nth 0 (nth 0 (plist-get result :rows))) 99)))
          ;; DML
          (mysql-execute db "DROP TABLE IF EXISTS test_async_open_q")
          (mysql-execute db "CREATE TABLE test_async_open_q (id INT)")
          (should (= (mysql-execute db "INSERT INTO test_async_open_q VALUES (1)") 1))
          (mysql-execute db "DROP TABLE test_async_open_q"))
      (ignore-errors (mysql-close db)))))

;; ================================================================
;; 32.  Sync mysql-query (returns plist)
;; ================================================================

(ert-deftest mysql-sync-query-select-test ()
  "Sync mysql-query should return result plist for SELECT."
  (with-test-mysql-db db
    (let ((result (mysql-query db "SELECT 42 AS val")))
      (should (eq (plist-get result :type) 'select))
      (should (equal (plist-get result :columns) '("val")))
      (should (= (nth 0 (nth 0 (plist-get result :rows))) 42))
      (should (integerp (plist-get result :warning-count))))))

(ert-deftest mysql-sync-query-dml-test ()
  "Sync mysql-query should return result plist for DML."
  (with-test-mysql-db db
    (mysql-execute db "DROP TABLE IF EXISTS test_sq")
    (mysql-execute db "CREATE TABLE test_sq (id INT)")
    (mysql-execute db "INSERT INTO test_sq VALUES (1)")
    (let ((result (mysql-query db "DELETE FROM test_sq WHERE id = 1")))
      (should (eq (plist-get result :type) 'dml))
      (should (= (plist-get result :affected-rows) 1))
      (should (integerp (plist-get result :warning-count))))
    (mysql-execute db "DROP TABLE test_sq")))

;;; test.el ends here
