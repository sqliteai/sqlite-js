# SQLite-JS Extension

SQLite-JS is a powerful extension that brings JavaScript capabilities to SQLite. With this extension, you can create custom SQLite functions, aggregates, window functions, and collation sequences using JavaScript code, allowing for flexible and powerful data manipulation directly within your SQLite database.

## Table of Contents

- [Installation](#installation)
- [Functions Overview](#functions-overview)
- [Scalar Functions](#scalar-functions)
- [Aggregate Functions](#aggregate-functions)
- [Window Functions](#window-functions)
- [Collation Sequences](#collation-sequences)
- [JavaScript Evaluation](#javascript-evaluation)
- [Examples](#examples)
- [Building from Source](#building-from-source)
- [License](#license)

## Installation

### Pre-built Binaries

Download the appropriate pre-built binary for your platform from the official [Release](https://github.com/sqliteai/sqlite-js/releases) page:

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

- **function_name**: The name of your custom function
- **function_code**: JavaScript code that defines your function. Must be in the form `function(args) { /* your code here */ }`

### Example

```sql
-- Create a custom function to calculate age from birth date
SELECT js_create_scalar('age', 'function(args) {
  const birthDate = new Date(args[0]);
  const today = new Date();
  let age = today.getFullYear() - birthDate.getFullYear();
  const m = today.getMonth() - birthDate.getMonth();
  if (m < 0 || (m === 0 && today.getDate() < birthDate.getDate())) {
    age--;
  }
  return age;
}');

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

- **function_name**: The name of your custom aggregate function
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
  'function(args) {
    values.push(args[0]);
  }',
  
  -- Final code: calculate the median
  'function() {
    values.sort((a, b) => a - b);
    const mid = Math.floor(values.length / 2);
    if (values.length % 2 === 0) {
      return (values[mid-1] + values[mid]) / 2;
    } else {
      return values[mid];
    }
  }'
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

- **function_name**: The name of your custom window function
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
  'function(args) {
    sum += args[0];
    count++;
  }',
  
  -- Final code: not needed for this example
  'function() { }',
  
  -- Value code: return current average
  'function() {
    return count > 0 ? sum / count : null;
  }',
  
  -- Inverse code: remove a value from the window
  'function(args) {
    sum -= args[0];
    count--;
  }'
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

- **collation_name**: The name of your custom collation
- **collation_function**: JavaScript code that compares two strings. Must return a negative number if the first string is less than the second, zero if they are equal, or a positive number if the first string is greater than the second.

### Example

```sql
-- Create a case-insensitive natural sort collation
SELECT js_create_collation('natural_nocase', 'function(a, b) {
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
}');

-- Use the collation
SELECT * FROM files ORDER BY name COLLATE natural_nocase;
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
SELECT js_create_scalar('get_domain', 'function(args) {
  const email = args[0];
  return email.split("@")[1] || null;
}');

-- Use it in a query
SELECT email, get_domain(email) AS domain FROM users;
```

### Example 2: Statistical Aggregation

```sql
-- Create a function to calculate standard deviation
SELECT js_create_aggregate('stddev',
  'sum = 0; sumSq = 0; count = 0;',
  
  'function(args) {
    const val = args[0];
    sum += val;
    sumSq += val * val;
    count++;
  }',
  
  'function() {
    if (count < 2) return null;
    const variance = (sumSq - (sum * sum) / count) / (count - 1);
    return Math.sqrt(variance);
  }'
);

-- Use it in a query
SELECT department, stddev(salary) FROM employees GROUP BY department;
```

### Example 3: Custom Window Function

```sql
-- Create a window function to calculate percentile within a window
SELECT js_create_window('percentile_rank',
  'values = [];',
  
  'function(args) {
    values.push(args[0]);
  }',
  
  'function() {
    values.sort((a, b) => a - b);
  }',
  
  'function() {
    const current = values[values.length - 1];
    const rank = values.indexOf(current);
    return (rank / (values.length - 1)) * 100;
  }',
  
  'function(args) {
    const index = values.indexOf(args[0]);
    if (index !== -1) {
      values.splice(index, 1);
    }
  }'
);

-- Use it in a query
SELECT name, score, 
       percentile_rank(score) OVER (ORDER BY score) 
FROM exam_results;
```

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

This project is licensed under the MIT License - see the LICENSE file for details.
