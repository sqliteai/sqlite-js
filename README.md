<div align="center">
  <a href="https://sqlite.ai">
    <img src="https://www.sqlite.ai/social/logo-ai.png" alt="SQLite AI" height="56">
  </a>

  <h1>SQLite-JS</h1>
  <p><strong>Write SQLite functions in JavaScript.</strong><br>
  Scalars, aggregates, window functions, and collations — directly in JS, with full QuickJS sandboxing. No more C extensions for custom logic.</p>

  <p>
    <a href="https://dashboard.sqlitecloud.io/auth/sign-in"><strong>Free managed instance →</strong></a> ·
    <a href="https://docs.sqlitecloud.io/docs/ai-overview">Docs</a> ·
    <a href="https://sqlite.ai">Website</a> ·
    <a href="https://blog.sqlite.ai">Blog</a>
  </p>

  <p>
    <sub><strong>Data:</strong>
    <a href="https://github.com/sqliteai/sqlite-vector">Vector</a> ·
    <a href="https://github.com/sqliteai/sqlite-sync">Sync</a> ·
    <a href="https://github.com/sqliteai/sqlite-columnar">Columnar</a> ·
    <a href="https://github.com/sqliteai/sqlite-js">JS</a>
    <br>
    <strong>AI:</strong>
    <a href="https://github.com/sqliteai/sqlite-ai">AI</a> ·
    <a href="https://github.com/sqliteai/sqlite-agent">Agent</a> ·
    <a href="https://github.com/sqliteai/sqlite-memory">Memory</a> ·
    <a href="https://github.com/sqliteai/sqlite-mcp">MCP</a>
    </sub>
  </p>
</div>

<br>

> **Want to run your JS functions in the cloud too?** SQLite-JS runs locally as an extension; pair it with **[SQLite Cloud Edge Functions](https://dashboard.sqlitecloud.io/auth/sign-in)** to deploy the same JS server-side. Free tier available.

---

# SQLite JS

**SQLite JS** is a powerful extension that brings JavaScript capabilities to SQLite. With this extension, you can create custom SQLite functions, aggregates, window functions, and collation sequences using JavaScript code, allowing for flexible and powerful data manipulation directly within your SQLite database.

## Table of Contents

- [Installation](#installation)
- [Functions Overview](#functions-overview)
- [Scalar Functions](#scalar-functions)
- [Aggregate Functions](#aggregate-functions)
- [Window Functions](#window-functions)
- [Collation Sequences](#collation-sequences)
- [Sync JavaScript Functions Across Devices](#syncing-across-devices)
- [JavaScript Evaluation](#javascript-evaluation)
- [Utility Functions](#utility-functions)
- [Examples](#examples)
- [Update Functions](#update-functions)
- [Functions in the Schema](#functions-in-the-schema)
- [Function Naming Rules](#function-naming-rules)
- [Building from Source](#building-from-source)
- [License](#license)

## Installation

### Pre-built Binaries

Download the appropriate pre-built binary for your platform from the official [Releases](https://github.com/sqliteai/sqlite-js/releases) page:

- Linux: x86 and ARM
- macOS: x86 and ARM
- Windows: x86
- Android
- iOS

### Loading the Extension

```sql
-- In SQLite CLI
.load ./js

-- In SQL
SELECT load_extension('./js');
```

### Swift Package

You can [add this repository as a package dependency to your Swift project](https://developer.apple.com/documentation/xcode/adding-package-dependencies-to-your-app#Add-a-package-dependency). After adding the package, you'll need to set up SQLite with extension loading by following steps 4 and 5 of [this guide](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/platforms/ios.md#4-set-up-sqlite-with-extension-loading).

Here's an example of how to use the package:
```swift
import js

...

var db: OpaquePointer?
sqlite3_open(":memory:", &db)
sqlite3_enable_load_extension(db, 1)
var errMsg: UnsafeMutablePointer<Int8>? = nil
sqlite3_load_extension(db, js.path, nil, &errMsg)
var stmt: OpaquePointer?
sqlite3_prepare_v2(db, "SELECT js_version()", -1, &stmt, nil)
defer { sqlite3_finalize(stmt) }
sqlite3_step(stmt)
log("js_version(): \(String(cString: sqlite3_column_text(stmt, 0)))")
sqlite3_close(db)
```

### Android Package

Add the [following](https://central.sonatype.com/artifact/ai.sqlite/js) to your Gradle dependencies:

```gradle
implementation 'ai.sqlite:js:1.3.0'
```

Here's an example of how to use the package:
```java
SQLiteCustomExtension jsExtension = new SQLiteCustomExtension(getApplicationInfo().nativeLibraryDir + "/js", null);
SQLiteDatabaseConfiguration config = new SQLiteDatabaseConfiguration(
    getCacheDir().getPath() + "/js_test.db",
    SQLiteDatabase.CREATE_IF_NECESSARY | SQLiteDatabase.OPEN_READWRITE,
    Collections.emptyList(),
    Collections.emptyList(),
    Collections.singletonList(jsExtension)
);
SQLiteDatabase db = SQLiteDatabase.openDatabase(config, null, null);
```

**Note:** Additional settings and configuration are required for a complete setup. For full implementation details, see the [complete Android example](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/examples/android/README.md).

### Node.js Package

Install the [@sqliteai/sqlite-js](https://www.npmjs.com/package/@sqliteai/sqlite-js) package:

```bash
npm install @sqliteai/sqlite-js
```

Usage with `better-sqlite3`:
```typescript
import { getExtensionPath } from '@sqliteai/sqlite-js';
import Database from 'better-sqlite3';

const db = new Database(':memory:');
db.loadExtension(getExtensionPath());
console.log(db.prepare('SELECT js_version()').pluck().get());
```

### Flutter Package

Add the [sqlite_js](https://pub.dev/packages/sqlite_js) package to your project:

```bash
flutter pub add sqlite_js  # Flutter projects
dart pub add sqlite_js     # Dart projects
```

Usage with `sqlite3` package:
```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_js/sqlite_js.dart';

sqlite3.loadSqliteJsExtension();
final db = sqlite3.openInMemory();
print(db.select('SELECT js_version()'));
```

For a complete example, see the [Flutter example](https://github.com/sqliteai/sqlite-extensions-guide/blob/main/examples/flutter/README.md).

## Functions Overview

SQLite-JS provides several ways to extend SQLite functionality with JavaScript:

| Function Type | Description |
|---------------|-------------|
| Scalar Functions | Process individual rows and return a single value |
| Aggregate Functions | Process multiple rows and return a single aggregated result |
| Window Functions | Similar to aggregates but can access the full dataset |
| Collation Sequences | Define custom sort orders for text values |
| JavaScript Evaluation | Directly evaluate JavaScript code within SQLite |

### Values

SQL values reach JavaScript as follows:

| SQL | JavaScript |
|-----|------------|
| NULL | `null` |
| INTEGER, REAL | number |
| TEXT | string |
| BLOB | `ArrayBuffer` |

A JavaScript result goes back to SQL as follows: `null` and `undefined` become NULL, a boolean an INTEGER (1 or 0), a number an INTEGER if it is a whole number from -(2^53 - 1) to 2^53 - 1 (`Number.isSafeInteger`) and a REAL otherwise, a BigInt an INTEGER, a string TEXT, and an `ArrayBuffer` or a typed array such as `Uint8Array` a BLOB. Other objects become NULL; return `JSON.stringify(value)` to keep one as TEXT. The parameters of `db.exec()` convert the same way.

JavaScript has a single number type, a double. So a REAL that holds a whole number, such as 3.0, comes back as the INTEGER 3, and an INTEGER beyond ±(2^53 - 1) loses precision in JavaScript: pass it as TEXT, `CAST(x AS TEXT)`, and convert it with `BigInt(args[0])`.

## Scalar Functions

Scalar functions process one row at a time and return a single value. They are useful for data transformation, calculations, text manipulation, etc.

### Usage

```sql
SELECT js_create_scalar('function_name', 'function_code');
```

### Parameters

- **function_name**: The name of your custom function (see [Function Naming Rules](#function-naming-rules))
- **function_code**: JavaScript code that defines your function. Must be in the form `function(args) { /* your code here */ }`

### Example

```sql
-- Create a custom function to calculate age from birth date
SELECT js_create_scalar('age', '(function(args) {
  const birthDate = new Date(args[0]);
  const today = new Date();
  let age = today.getFullYear() - birthDate.getFullYear();
  const m = today.getMonth() - birthDate.getMonth();
  if (m < 0 || (m === 0 && today.getDate() < birthDate.getDate())) {
    age--;
  }
  return age;
})');

-- Use the function
SELECT name, age(birth_date) FROM people;
```

## Aggregate Functions

Aggregate functions process multiple rows and compute a single result. Examples include SUM, AVG, and COUNT in standard SQL.

### Usage

```sql
SELECT js_create_aggregate('function_name', 'init_code', 'step_code', 'final_code');
```

### Parameters

- **function_name**: The name of your custom aggregate function (see [Function Naming Rules](#function-naming-rules))
- **init_code**: JavaScript code that initializes variables for the aggregation
- **step_code**: JavaScript code that processes each row. Must be in the form `function(args) { /* your code here */ }`
- **final_code**: JavaScript code that computes the final result. Must be in the form `function() { /* your code here */ }`

### Example

```sql
-- Create a median function
SELECT js_create_aggregate('median', 
  -- Init code: initialize an array to store values
  'values = [];',
  
  -- Step code: collect values from each row
  '(function(args) {
    values.push(args[0]);
  })',
  
  -- Final code: calculate the median
  '(function() {
    values.sort((a, b) => a - b);
    const mid = Math.floor(values.length / 2);
    if (values.length % 2 === 0) {
      return (values[mid-1] + values[mid]) / 2;
    } else {
      return values[mid];
    }
  })'
);

-- Use the function
SELECT median(salary) FROM employees;
```

## Window Functions

Window functions, like aggregate functions, operate on a set of rows. However, they can access all rows in the current window without collapsing them into a single output row.

### Usage

```sql
SELECT js_create_window('function_name', 'init_code', 'step_code', 'final_code', 'value_code', 'inverse_code');
```

### Parameters

- **function_name**: The name of your custom window function (see [Function Naming Rules](#function-naming-rules))
- **init_code**: JavaScript code that initializes variables
- **step_code**: JavaScript code that processes each row. Must be in the form `function(args) { /* your code here */ }`
- **final_code**: JavaScript code that computes the final result. Must be in the form `function() { /* your code here */ }`
- **value_code**: JavaScript code that returns the current value. Must be in the form `function() { /* your code here */ }`
- **inverse_code**: JavaScript code that removes a row from the current window. Must be in the form `function(args) { /* your code here */ }`

### Example

```sql
-- Create a moving average window function
SELECT js_create_window('moving_avg',
  -- Init code
  'sum = 0; count = 0;',
  
  -- Step code: process each row
  '(function(args) {
    sum += args[0];
    count++;
  })',
  
  -- Final code: not needed for this example
  '(function() { })',
  
  -- Value code: return current average
  '(function() {
    return count > 0 ? sum / count : null;
  })',
  
  -- Inverse code: remove a value from the window
  '(function(args) {
    sum -= args[0];
    count--;
  })'
);

-- Use the function
SELECT id, value, moving_avg(value) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) 
FROM measurements;
```

## Collation Sequences

Collation sequences determine how text values are compared and sorted in SQLite. Custom collations enable advanced sorting capabilities like natural sorting, locale-specific sorting, etc.

### Usage

```sql
SELECT js_create_collation('collation_name', 'collation_function');
```

### Parameters

- **collation_name**: The name of your custom collation (see [Function Naming Rules](#function-naming-rules))
- **collation_function**: JavaScript code that compares two strings. Must return a negative number if the first string is less than the second, zero if they are equal, or a positive number if the first string is greater than the second.

### Example

```sql
-- Create a case-insensitive natural sort collation
SELECT js_create_collation('natural_nocase', '(function(a, b) {
  // Extract numbers for natural comparison
  const splitA = a.toLowerCase().split(/(\d+)/);
  const splitB = b.toLowerCase().split(/(\d+)/);
  
  for (let i = 0; i < Math.min(splitA.length, splitB.length); i++) {
    if (splitA[i] !== splitB[i]) {
      if (!isNaN(splitA[i]) && !isNaN(splitB[i])) {
        return parseInt(splitA[i]) - parseInt(splitB[i]);
      }
      return splitA[i].localeCompare(splitB[i]);
    }
  }
  return splitA.length - splitB.length;
})');

-- Use the collation
SELECT * FROM files ORDER BY name COLLATE natural_nocase;
```

## Syncing Across Devices

When used with [sqlite-sync](https://github.com/sqliteai/sqlite-sync/), user-defined functions created via sqlite-js are automatically replicated across the SQLite Cloud cluster, ensuring that all connected peers share the same logic and behavior — even offline. To enable automatic persistence and sync the special `js_init_table` function must be executed.

### Usage
```sql
SELECT js_init_table();         -- Create table if needed (no loading)
SELECT js_init_table(1);        -- Create table and load all stored functions
```

## JavaScript Evaluation

The extension also provides a way to directly evaluate JavaScript code within SQLite queries.

### Usage

```sql
SELECT js_eval('javascript_code');
```

### Parameters

- **javascript_code**: Any valid JavaScript code to evaluate

### Example

```sql
-- Perform a calculation
SELECT js_eval('Math.PI * Math.pow(5, 2)');

-- Format a date
SELECT js_eval('new Date(1629381600000).toLocaleDateString()');
```

### Running SQL from JavaScript

JavaScript can run SQL on the same connection through the global `db` object. `db.exec(sql, ...parameters)` prepares one statement, binds the parameters to its `?` placeholders, and returns a result set. The statement runs as you step through it: `next()` moves to the next row and returns `true`, or `false` after the last; `get(i)` and `name(i)` give a column's value and name; `columnCount` is the number of columns; and `toArray()` returns the remaining rows as arrays. A statement that returns no rows, such as an `INSERT`, also runs only when you call `next()` or `toArray()`.

An error while the statement runs, such as a failed constraint, is thrown from `next()` or `toArray()`, as an error in the SQL itself is thrown from `db.exec()`.

```sql
CREATE TABLE t(a UNIQUE);
SELECT js_eval('db.exec("INSERT INTO t VALUES (?)", 1).next()');  -- 0: the row is inserted, and next() is false, as there are no rows
SELECT js_eval('(function () { try { db.exec("INSERT INTO t VALUES (?)", 1).next(); } catch (e) { return e.message; } })()');  -- UNIQUE constraint failed: t.a
```

### Interrupting JavaScript

JavaScript runs inside the SQL statement that calls it, so `sqlite3_interrupt()` stops it as it stops any statement: the statement fails with an "interrupted" error, even if the code catches exceptions. In the `sqlite3` shell, Ctrl-C does this. Interrupting JavaScript needs SQLite 3.41.0 or later.

## Utility Functions

### js_version

Returns the extension version string, or the internal QuickJS engine version when called with an argument.

```sql
SELECT js_version();      -- Returns the SQLite-JS version (e.g. '1.3.0')
SELECT js_version(1);     -- Returns the QuickJS engine version
```

### js_load_text / js_load_blob

Load file contents into SQLite — as text or as a blob.

```sql
SELECT js_load_text('/path/to/file.txt');   -- Returns file contents as text
SELECT js_load_blob('/path/to/file.bin');   -- Returns file contents as a blob
```

### js_set_max_stack_size

Sets the most stack, in bytes, that JavaScript may use, which limits how deeply functions can call themselves; code that goes deeper fails with "Maximum call stack size exceeded". The default is 1 MB. Each call into JavaScript measures the stack of the thread it runs on, so a connection can be used from several threads, and the size is lowered for a call on a thread with less stack. The size must be an integer no larger than the calling thread's stack allows, less a margin for the native code that runs between QuickJS's checks (`SQLITE_JS_STACK_MARGIN`, 256 KB); the error message gives the largest size allowed. A larger size, or text, which would read as 0, is an error, because QuickJS's check would never trip and code that calls itself without end would crash the process.

`0` turns the check off. Code that calls itself without end then crashes the process.

```sql
SELECT js_set_max_stack_size(1048576);  -- Set max stack size to 1 MB
SELECT js_set_max_stack_size(0);        -- No check
```

### js_set_memory_limit

Sets the most memory, in bytes, that JavaScript may allocate on this connection; 0 means no limit. The default is 64 MB (64 × 1024 × 1024 bytes); build with `-DSQLITE_JS_DEFAULT_MEMORY_LIMIT=bytes` to change it. Code that needs more fails with an "out of memory" error, and the connection stays usable. JavaScript that catches the error receives `null` instead when the limit leaves no room even for the error.

Garbage is collected before the limit is reached, and as the limit changes, so a new limit under the memory a connection has already allocated still leaves it usable. A limit under the memory that is still *in use*, such as a large value held in a global, is another matter: nothing can be allocated until the limit is raised again.

```sql
SELECT js_set_memory_limit(268435456);  -- Allow 256 MB
SELECT js_set_memory_limit(0);          -- No limit
```

## Examples

### Example 1: String Manipulation

```sql
-- Create a function to extract domain from email
SELECT js_create_scalar('get_domain', '(function(args) {
  const email = args[0];
  return email.split("@")[1] || null;
})');

-- Use it in a query
SELECT email, get_domain(email) AS domain FROM users;
```

### Example 2: Statistical Aggregation

```sql
-- Create a function to calculate standard deviation
SELECT js_create_aggregate('stddev',
  'sum = 0; sumSq = 0; count = 0;',
  
  '(function(args) {
    const val = args[0];
    sum += val;
    sumSq += val * val;
    count++;
  })',
  
  '(function() {
    if (count < 2) return null;
    const variance = (sumSq - (sum * sum) / count) / (count - 1);
    return Math.sqrt(variance);
  })'
);

-- Use it in a query
SELECT department, stddev(salary) FROM employees GROUP BY department;
```

### Example 3: Custom Window Function

```sql
-- Create a window function to calculate percentile within a window
SELECT js_create_window('percentile_rank',
  'values = [];',
  
  '(function(args) {
    values.push(args[0]);
  })',
  
  '(function() {
    values.sort((a, b) => a - b);
  })',
  
  '(function() {
    const current = values[values.length - 1];
    const rank = values.indexOf(current);
    return (rank / (values.length - 1)) * 100;
  })',
  
  '(function(args) {
    const index = values.indexOf(args[0]);
    if (index !== -1) {
      values.splice(index, 1);
    }
  })'
);

-- Use it in a query
SELECT name, score, 
       percentile_rank(score) OVER (ORDER BY score) 
FROM exam_results;
```

## Update Functions

Due to a constraint in [SQLite](https://www3.sqlite.org/src/info/cabab62bc10568d4), it is not possible to update or redefine a user-defined function using the same database connection that was used to initially register it. To modify an existing JavaScript function, the update must be performed through a separate database connection.

## Functions in the Schema

Every SQL function this extension registers, except `js_version`, runs JavaScript, reads files or changes the JavaScript runtime, so it is registered with `SQLITE_DIRECTONLY`: only top-level SQL can call it, never a view, trigger, CHECK constraint, generated column or index stored in the database. (TEMP views and triggers can call it: SQLite trusts them, since only the connection itself can make them.) That way a database from an untrusted source cannot run JavaScript just by being queried. The functions you create with `js_create_scalar`, `js_create_aggregate`, `js_create_window` and `js_create_collation` are not restricted.

A database can still store JavaScript in its `js_functions` table, and `js_init_table(1)` runs it. Call it only on databases you trust.

## Function Naming Rules

Function names must comply with SQLite identifier rules and must be unique within the database and its schema.

### Unquoted Identifiers
These must follow typical SQL naming conventions:
- Must begin with a letter (A-Z or a-z) or an underscore `_`
- May contain letters, digits (0-9), and underscores `_`
- Are case-insensitive
- Cannot match a reserved keyword unless quoted

**Examples:**
- Valid: `identifier1`, `_temp`, `user_name`
- Invalid: `123abc`, `select`, `identifier-name`

### Quoted Identifiers
SQLite supports delimited identifiers, which allow almost any character, as long as the identifier is properly quoted.

You can use:
- Double quotes: `"identifier name"`
- Square brackets (Microsoft-style): `[identifier name]`
- Backticks (MySQL-style): `` `identifier name` ``

These quoting styles are interchangeable in SQLite. Inside a quoted identifier, you can include:
- Spaces: `"my column"`
- Special characters: `"name@domain"`, `"price€"`, `"weird!name"`
- Reserved SQL keywords: `"select"`, `"group"`

Quoted identifiers are case-sensitive.

## Building from Source

See the included Makefile for building instructions:

```bash
# Build for your current platform
make

# Build for a specific platform
make PLATFORM=macos
make PLATFORM=linux
make PLATFORM=windows

# Install
make install
```

## License

SQLite-JS is released under the [Apache License 2.0](LICENSE).

---

## ☁️ Hosted version

Don't want to manage runtime infrastructure? **[SQLite Cloud](https://sqlite.ai)** runs your JavaScript functions at the edge, with auth, REST APIs, webhooks, and observability included.

[**Start free →**](https://dashboard.sqlitecloud.io/auth/sign-in)

---

## Part of the SQLite AI stack

SQLite-JS is one piece of a larger ecosystem that turns SQLite into a runtime for intelligent, distributed data:

**Data layer**
- [sqlite-vector](https://github.com/sqliteai/sqlite-vector) — ANN vector search inside SQLite
- [sqlite-sync](https://github.com/sqliteai/sqlite-sync) — Offline-first CRDT sync across devices
- [sqlite-columnar](https://github.com/sqliteai/sqlite-columnar) — Column-oriented analytics for OLAP queries
- [**sqlite-js**](https://github.com/sqliteai/sqlite-js) — Custom SQLite functions written in JavaScript *(you are here)*

**AI layer**
- [sqlite-ai](https://github.com/sqliteai/sqlite-ai) — On-device LLM inference and embeddings
- [sqlite-agent](https://github.com/sqliteai/sqlite-agent) — Autonomous AI agents running inside SQLite
- [sqlite-memory](https://github.com/sqliteai/sqlite-memory) — Persistent, searchable memory for agents
- [sqlite-mcp](https://github.com/sqliteai/sqlite-mcp) — Call MCP tools directly from SQL queries

**Managed platform**
- [SQLite Cloud](https://sqlite.ai) — Hosted SQLite with sync, auth, edge functions, and analytics. [Free tier →](https://dashboard.sqlitecloud.io/auth/sign-in)

Built by [SQLite AI](https://sqlite.ai). Questions? [Contact us](https://sqlite.ai/support).
