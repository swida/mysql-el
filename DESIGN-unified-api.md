# mysql-el Unified API Redesign v3

## Overview

Unify sync/async APIs: same function names, async mode via parameter.
Three core operations, each supports sync/async:

| Operation | Function | Poll |
|-----------|----------|------|
| Connect | `mysql-open ... ASYNC` | `mysql-open-poll` |
| Query | `mysql-query ... ASYNC` | `mysql-query-poll` |
| Close | `mysql-close` (always sync) | — |

## Core API (new)

```elisp
(mysql-open HOST USER PASSWORD &optional DATABASE PORT ASYNC) → DB
(mysql-open-poll DB) → 'not-ready | 'complete

(mysql-query DB SQL &optional ASYNC) → result-plist | 'not-ready
(mysql-query-poll DB) → 'not-ready | result-plist

(mysql-close DB) → t
```

## Convenience aliases (preserved, sync, with prepared stmt support)

```elisp
(mysql-execute DB QUERY &optional VALUES) → integer | list
(mysql-select DB QUERY &optional VALUES RETURN-TYPE) → list
```

## Result plist format

```elisp
;; SELECT
(:type select :columns ("id" "name") :rows ((1 "Alice")) :warning-count 0)

;; DML/DDL
(:type dml :affected-rows 3 :warning-count 2)
```

## C layer: connection object

```c
enum mysql_phase {
  PHASE_IDLE,         // ready for new operations
  PHASE_CONNECTING,   // async connect in progress
  PHASE_QUERY,        // async query in progress
  PHASE_STORE,        // async store-result in progress
};

struct mysql_conn {
  MYSQL *conn;
  enum mysql_phase phase;
  MYSQL_RES *pending_res;
};
```

## Removed functions (Phase 3)

- `mysql-query-start` → `(mysql-query db sql t)`
- `mysql-query-continue` → `(mysql-query-poll db)`
- `mysql-store-result-start/continue` → internal to `mysql-query-poll`
- `mysql-async-result` → internal to `mysql-query-poll`
- `mysql-async-affected-rows` → plist `:affected-rows`
- `mysql-async-field-count` → plist `:type`
- `mysql-warning-count` → plist `:warning-count`
- TIMEOUT parameter from `mysql-open` → async connect replaces it

## Preserved unchanged

`mysql-execute`, `mysql-select`, `mysql-execute-batch`,
`mysql-transaction`/`commit`/`rollback`,
`mysql-next`/`more-p`/`columns`/`finalize`,
`mysql-escape-string`, `mysqlp`, `mysql-available-p`, `mysql-version`, `mysql-close`.

## Migration phases

1. Phase 1: Implement `struct mysql_conn` + `mysql-query` + `mysql-query-poll` + `mysql-open ... ASYNC` + `mysql-open-poll`, old API coexists
2. Phase 2: isqlm switches to new API
3. Phase 3: Remove old 7 async functions + TIMEOUT
