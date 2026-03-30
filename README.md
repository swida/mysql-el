# mysql-el — Emacs MySQL 动态模块

一个 Emacs 动态模块，提供原生 MySQL 客户端 API，接口风格与 Emacs 内置的 `sqlite.c` 保持一致。

## 目录

- [编译安装](#编译安装)
- [快速上手：一个完整的例子](#快速上手一个完整的例子)
- [API 参考](#api-参考)
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
- [数据类型映射](#数据类型映射)
- [运行测试](#运行测试)

---

## 编译安装

### 前置依赖

- GNU Emacs 源码（带动态模块支持，即编译时 `--with-modules`）——编译本模块需要 Emacs 的 C 头文件（`emacs-module.h` 等）
- MySQL 客户端库（`libmysqlclient`）及其头文件
- GCC
- `makeinfo`（Texinfo，用于生成 `.info` 文档）

### 配置 Makefile

编译前需要修改 `Makefile` 顶部的两个路径变量，使其指向你的环境：

```makefile
# Emacs 源码根目录（需要其中的 src/emacs-module.h）
ROOT = $(HOME)/emacs

# MySQL 安装目录（需要其中的 include/ 和 lib/）
MYSQL_DIR = $(HOME)/mysqlinst
```

| 变量 | 说明 | 需要的内容 |
|------|------|-----------|
| `ROOT` | Emacs 源码根目录 | 该目录下应存在 `src/emacs-module.h` |
| `MYSQL_DIR` | MySQL 安装目录 | 该目录下应存在 `include/mysql.h` 和 `lib/libmysqlclient.so` |

例如，如果你的 Emacs 源码在 `/opt/emacs-30`，MySQL 安装在 `/usr/local/mysql`，则修改为：

```makefile
ROOT = /opt/emacs-30
MYSQL_DIR = /usr/local/mysql
```

### 编译

```bash
cd mysql-el

# 如果 MySQL 安装在非标准路径，先导出库路径
export LD_LIBRARY_PATH=$MYSQL_DIR/lib   # 按你的实际路径

make
```

编译成功后会生成：
- `mysql-el.o` — 目标文件
- `mysql-el.so` — 动态模块（加载到 Emacs 中使用）
- `mysql-el.info` — Info 格式文档

清理编译产物：

```bash
make clean
```

### 加载模块

```elisp
;; 方式一：直接加载 .so
(module-load "/path/to/mysql-el.so")

;; 方式二：加入 load-path 后 require
(add-to-list 'load-path "/path/to/modules/mysql-el/")
(require 'mysql-el)
```

---

## 快速上手：一个完整的例子

下面用一个「员工管理」场景，从建表、插入、查询、更新、事务到关闭，串起所有核心 API。

```elisp
;; 0. 加载模块
(module-load "mysql-el.so")

;; 1. 连接数据库
(setq db (mysql-open "127.0.0.1" "root" "" "emacs_test" 3306))

;; 2. 建表（DDL）
(mysql-execute db "DROP TABLE IF EXISTS employees")
(mysql-execute db
  "CREATE TABLE employees (
     id    INT PRIMARY KEY AUTO_INCREMENT,
     name  VARCHAR(100) NOT NULL,
     dept  VARCHAR(50),
     salary DOUBLE
   ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")

;; 3. 插入数据 —— 返回值是受影响的行数
(mysql-execute db "INSERT INTO employees (name, dept, salary)
                   VALUES ('张三', '工程部', 25000.50)")
;; => 1

(mysql-execute db "INSERT INTO employees (name, dept, salary)
                   VALUES ('李四', '产品部', 22000)")
;; => 1

;; 4. 使用绑定参数（Prepared Statement）安全地插入
;;    参数以 list 形式传递，支持 string / integer / float / nil
(mysql-execute db
  "INSERT INTO employees (name, dept, salary) VALUES (?, ?, ?)"
  '("王五" "工程部" 30000.0))
;; => 1

;; 5. 普通查询 —— 返回行列表，每行是一个 list
(mysql-select db "SELECT * FROM employees ORDER BY id")
;; => (("张三" "工程部" 25000.5)
;;     ("李四" "产品部" 22000.0)
;;     ("王五" "工程部" 30000.0))
;;  注意：id 列是 INT 类型，会被自动转为 Emacs 整数

;; 6. 带列名的查询（full 模式）
(mysql-select db "SELECT name, salary FROM employees" nil 'full)
;; => (("name" "salary")          ; <-- 第一个元素是列名列表
;;     ("张三" 25000.5)
;;     ("李四" 22000.0)
;;     ("王五" 30000.0))

;; 7. 带参数的条件查询
(mysql-select db
  "SELECT name, salary FROM employees WHERE dept = ?"
  '("工程部"))
;; => (("张三" 25000.5)
;;     ("王五" 30000.0))

;; 8. 更新数据 —— 返回受影响行数
(mysql-execute db
  "UPDATE employees SET salary = salary * 1.1 WHERE dept = '工程部'")
;; => 2

;; 9. 事务：批量操作，出错时回滚
(mysql-transaction db)
(condition-case err
    (progn
      (mysql-execute db "INSERT INTO employees (name, dept, salary)
                         VALUES ('赵六', '财务部', 20000)")
      (mysql-execute db "INSERT INTO employees (name, dept, salary)
                         VALUES ('钱七', '财务部', 21000)")
      (mysql-commit db)
      (message "事务提交成功"))
  (error
   (mysql-rollback db)
   (message "事务回滚: %s" (error-message-string err))))

;; 10. 批量执行多条语句（用分号分隔）
(mysql-execute-batch db
  "INSERT INTO employees (name, dept, salary) VALUES ('孙八', 'HR', 19000);
   INSERT INTO employees (name, dept, salary) VALUES ('周九', 'HR', 18500);")

;; 11. 使用 set 模式逐行遍历大结果集（不一次性加载全部到内存）
(let ((set (mysql-select db "SELECT id, name, salary FROM employees ORDER BY id"
                         nil 'set)))
  (unwind-protect
      (progn
        (message "列名: %S" (mysql-columns set))
        (while (mysql-more-p set)
          (let ((row (mysql-next set)))
            (when row
              (message "Row: %S" row)))))
    (mysql-finalize set)))

;; 12. 字符串转义（用于动态拼接场景，推荐优先用参数绑定）
(mysql-escape-string db "it's a \"test\"")
;; => "it\\'s a \\\"test\\\""

;; 13. 查看最终结果
(mysql-select db "SELECT id, name, dept, salary FROM employees ORDER BY id"
              nil 'full)

;; 14. 清理并关闭
(mysql-execute db "DROP TABLE employees")
(mysql-close db)
```

---

## API 参考

### mysql-available-p

```
(mysql-available-p) → t
```

检查 MySQL 模块是否已加载可用。加载后始终返回 `t`。

---

### mysql-version

```
(mysql-version) → STRING
```

返回 MySQL 客户端库的版本字符串，如 `"8.0.36"`。

---

### mysql-open

```
(mysql-open HOST USER PASSWORD &optional DATABASE PORT) → DB
```

打开一个 MySQL 连接，返回数据库句柄对象。

| 参数 | 类型 | 说明 |
|------|------|------|
| HOST | string | 主机地址，如 `"127.0.0.1"` |
| USER | string | 用户名 |
| PASSWORD | string | 密码，无密码传 `""` |
| DATABASE | string \| nil | 可选，默认数据库名 |
| PORT | integer \| nil | 可选，默认 3306 |

**特性：**
- 连接自动设置字符集为 `utf8mb4`
- 连接启用了 `CLIENT_MULTI_STATEMENTS`，支持 `mysql-execute-batch`
- 连接对象被 GC 回收时会自动调用 `mysql_close`

```elisp
;; 最简用法（3 个必填参数）
(setq db (mysql-open "127.0.0.1" "root" ""))

;; 完整用法
(setq db (mysql-open "127.0.0.1" "root" "mypassword" "mydb" 3306))
```

---

### mysql-close

```
(mysql-close DB) → t
```

关闭数据库连接。关闭后再操作该句柄会报错。

```elisp
(mysql-close db)  ;; => t
(mysql-execute db "SELECT 1")  ;; => 报错：Invalid or closed database object
```

---

### mysqlp

```
(mysqlp OBJECT) → t | nil
```

判断对象是否为一个**活跃的** MySQL 连接句柄。

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

执行一条 SQL 语句。

**返回值：**
- **DDL / INSERT / UPDATE / DELETE**：返回受影响的行数（整数）
- **SELECT**：返回行列表（与 `mysql-select` 相同，但无 `full` 模式）

| 参数 | 类型 | 说明 |
|------|------|------|
| DB | user-ptr | 数据库连接句柄 |
| QUERY | string | SQL 语句，可含 `?` 占位符 |
| VALUES | list \| nil | 可选，绑定参数列表 |

**绑定参数类型：**

| Elisp 类型 | MySQL 类型 |
|------------|-----------|
| `string` | MYSQL_TYPE_STRING |
| `integer` | MYSQL_TYPE_LONGLONG |
| `float` | MYSQL_TYPE_DOUBLE |
| `nil` | MYSQL_TYPE_NULL |

```elisp
;; DDL
(mysql-execute db "CREATE TABLE t (id INT, name TEXT)")

;; INSERT，返回受影响行数
(mysql-execute db "INSERT INTO t VALUES (1, 'foo')")  ;; => 1

;; 带参数的 INSERT
(mysql-execute db "INSERT INTO t VALUES (?, ?)" '(2 "bar"))  ;; => 1

;; DELETE，返回删除行数
(mysql-execute db "DELETE FROM t WHERE id > 1")  ;; => 1

;; SELECT（不推荐，优先用 mysql-select）
(mysql-execute db "SELECT * FROM t")  ;; => ((1 "foo"))
```

---

### mysql-select

```
(mysql-select DB QUERY &optional VALUES RETURN-TYPE) → LIST | SET
```

执行 SELECT 查询，专门用于读取数据。

| 参数 | 类型 | 说明 |
|------|------|------|
| DB | user-ptr | 数据库连接句柄 |
| QUERY | string | SELECT 语句，可含 `?` 占位符 |
| VALUES | list \| nil | 可选，绑定参数列表 |
| RETURN-TYPE | symbol \| nil | `nil`=只返回数据行；`'full`=首元素为列名；`'set`=返回游标对象 |

**普通模式**（RETURN-TYPE = nil）：

```elisp
(mysql-select db "SELECT name, age FROM users")
;; => (("Alice" 30) ("Bob" 25))
```

**Full 模式**（RETURN-TYPE = 'full）：

```elisp
(mysql-select db "SELECT name, age FROM users" nil 'full)
;; => (("name" "age")      ; <-- 列名
;;     ("Alice" 30)
;;     ("Bob" 25))
```

**Set 模式**（RETURN-TYPE = 'set）——适用于大结果集：

返回一个 set 对象（游标），通过 `mysql-next` 逐行取数据，避免一次性将所有结果加载到内存。用法类似 SQLite 的 `sqlite-next` / `sqlite-more-p`。

```elisp
(let ((set (mysql-select db "SELECT * FROM big_table" nil 'set)))
  (unwind-protect
      (while (mysql-more-p set)
        (let ((row (mysql-next set)))
          (when row
            (message "Row: %S" row))))
    (mysql-finalize set)))
```

**带参数查询：**

```elisp
(mysql-select db "SELECT * FROM users WHERE age > ?" '(20))
;; => (("Alice" 30) ("Bob" 25))
```

**空结果集：**

```elisp
;; 普通模式：返回 nil
(mysql-select db "SELECT * FROM empty_table")  ;; => nil

;; full 模式：仍返回列名，但无数据行
(mysql-select db "SELECT * FROM empty_table" nil 'full)
;; => (("col1" "col2"))
```

---

### mysql-next

```
(mysql-next SET) → LIST | nil
```

从 set 对象中取出下一行数据，返回一个 list。当所有行都已取完时，返回 `nil`。

```elisp
(let ((set (mysql-select db "SELECT id, name FROM users" nil 'set)))
  (mysql-next set)  ;; => (1 "Alice")
  (mysql-next set)  ;; => (2 "Bob")
  (mysql-next set)  ;; => nil（已无更多数据）
  (mysql-finalize set))
```

---

### mysql-more-p

```
(mysql-more-p SET) → t | nil
```

判断 set 对象中是否还有更多数据可取。在 `mysql-next` 返回 `nil` 之后，此函数也返回 `nil`。

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

返回 set 对象的列名列表。

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

释放 set 对象持有的所有资源。调用后 set 不能再使用。

> **注意：** 即使不手动调用此函数，set 对象被 GC 回收时也会自动释放资源。
> 但建议在 `unwind-protect` 中显式调用以尽早释放数据库连接上的资源。

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

执行多条以分号分隔的 SQL 语句。适用于初始化脚本、批量 DDL 等场景。

```elisp
(mysql-execute-batch db
  "CREATE TABLE a (id INT);
   CREATE TABLE b (id INT);
   INSERT INTO a VALUES (1);")
;; => t
```

> **注意：** 此函数不支持绑定参数。如需参数化，请逐条使用 `mysql-execute`。

---

### mysql-transaction

```
(mysql-transaction DB) → t
```

开启一个事务（执行 `START TRANSACTION`）。

---

### mysql-commit

```
(mysql-commit DB) → t
```

提交当前事务（执行 `COMMIT`）。无活跃事务时调用不会报错。

---

### mysql-rollback

```
(mysql-rollback DB) → t
```

回滚当前事务（执行 `ROLLBACK`）。

**事务使用模式：**

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

对字符串进行 MySQL 转义，防止 SQL 注入。转义会考虑当前连接的字符集。

```elisp
(mysql-escape-string db "it's a \"test\"")
;; => "it\\'s a \\\"test\\\""
```

> **最佳实践：** 优先使用 `mysql-execute` / `mysql-select` 的绑定参数 `?`，
> 仅在必须动态拼接 SQL 时使用此函数。

---

## 数据类型映射

### 写入方向（Elisp → MySQL）

通过绑定参数 `?` 传值时的类型映射：

| Elisp 类型 | MySQL 绑定类型 |
|------------|---------------|
| `string` | MYSQL_TYPE_STRING |
| `integer` | MYSQL_TYPE_LONGLONG |
| `float` | MYSQL_TYPE_DOUBLE |
| `nil` | MYSQL_TYPE_NULL |

### 读取方向（MySQL → Elisp）

查询结果的自动类型转换（`mysql-select` 非 prepared path）：

| MySQL 类型 | Elisp 类型 |
|-----------|-----------|
| TINY / SHORT / INT / LONG / BIGINT | `integer` |
| FLOAT / DOUBLE / DECIMAL | `float` |
| TEXT / VARCHAR / CHAR / DATE / ... | `string` |
| NULL | `nil` |

> **注意：** Prepared statement path 的结果全部以 `string` 返回（NULL 除外为 `nil`），
> 因为 MySQL 的 stmt API 统一用 `MYSQL_TYPE_STRING` 缓冲区接收。

---

## 运行测试

模块自带了 30 个单元测试，覆盖连接、CRUD、Unicode、事务、批量执行、游标迭代等场景。

```bash
# 1. 确保 MySQL 服务已启动，且 emacs_test 数据库已创建
mysql -u root -e "CREATE DATABASE IF NOT EXISTS emacs_test"

# 2. 编译模块
cd mysql-el
make

# 3. 运行全部测试
export LD_LIBRARY_PATH=$MYSQL_DIR/lib   # 按你的实际 MySQL 路径
emacs -batch -L . -l ert -l test.el -f ert-run-tests-batch-and-exit

# 4. 运行指定测试（按名称正则匹配）
emacs -batch -L . -l ert -l test.el \
  --eval '(ert-run-tests-batch-and-exit "mysql-param")'
```

如需修改连接参数，编辑 `test.el` 顶部的变量：

```elisp
(defvar test-mysql-host "127.0.0.1")
(defvar test-mysql-user "root")
(defvar test-mysql-password "")
(defvar test-mysql-database "emacs_test")
(defvar test-mysql-port 3306)
```
