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
- [Examples](#examples)
- [Update Functions](#update-functions)
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
implementation 'ai.sqlite:js:1.1.12'
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

## 📦 Integrations

Use SQLite-AI alongside:

* **[SQLite-AI](https://github.com/sqliteai/sqlite-ai)** – on-device inference, embedding generation, and model interaction directly into your database
* **[SQLite-Vector](https://github.com/sqliteai/sqlite-vector)** – vector search from SQL
* **[SQLite-Sync](https://github.com/sqliteai/sqlite-sync)** – sync on-device databases with the cloud

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). You can use, copy, modify, and distribute it under the terms of the license for non-production use. For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.
