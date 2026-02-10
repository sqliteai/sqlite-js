# sqlite_js

SQLite JavaScript extension for Flutter/Dart. Execute JavaScript code directly within SQLite using the QuickJS engine.

## Installation

```
dart pub add sqlite_js
```

Requires Dart 3.10+ / Flutter 3.38+.

## Usage

### With `sqlite3`

```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_js/sqlite_js.dart';

void main() {
  // Load once at startup.
  sqlite3.loadSqliteJsExtension();

  final db = sqlite3.openInMemory();

  // Execute JavaScript code.
  final result = db.select("SELECT js_eval('1 + 2') AS result");
  print(result.first['result']); // 3

  // Use JavaScript for more complex operations.
  db.execute('''
    SELECT js_eval('
      const data = JSON.parse(input);
      return data.map(x => x * 2);
    ', '[1, 2, 3]') AS doubled
  ''');

  db.dispose();
}
```

### With `drift`

```dart
import 'package:sqlite3/sqlite3.dart';
import 'package:sqlite_js/sqlite_js.dart';
import 'package:drift/native.dart';

Sqlite3 loadExtensions() {
  sqlite3.loadSqliteJsExtension();
  return sqlite3;
}

// Use when creating the database:
NativeDatabase.createInBackground(
  File(path),
  sqlite3: loadExtensions,
);
```

## Supported platforms

| Platform | Architectures |
|----------|---------------|
| Android  | arm64, arm, x64 |
| iOS      | arm64 (device + simulator) |
| macOS    | arm64, x64 |
| Linux    | arm64, x64 |
| Windows  | x64 |

## API

See the full [sqlite-js API documentation](https://github.com/sqliteai/sqlite-js/blob/main/API.md).

## License

See [LICENSE](LICENSE).
