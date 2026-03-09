# mruby-postgresql

mruby binding for libpq from PostgreSQL.


Connection
----------
Connecting to a PostgreSQL server:
```ruby
conn = Pq.new("postgresql://localhost/postgres")
```

Disconnecting:
```ruby
conn.close
```
Any IO operation afterwards raises an `IOError`.


Executing queries
-----------------
Without arguments:
```ruby
res = conn.exec("select * from pg_database")
puts res.to_ary
```

With arguments:
```ruby
res = conn.exec("select * from pg_type where typname = $1", "bool")
puts res.to_ary
```
Passed arguments are automatically encoded in binary to prevent SQL injection.
The first argument is `$1`, the second `$2`, and so on.

Integer parameters are encoded using the smallest PostgreSQL integer type that
fits the runtime value — regardless of the platform's `MRB_INT_BIT`:

| Value range                     | Wire type | PG OID |
|---------------------------------|-----------|--------|
| −32 768 … 32 767                | `int2`    | 21     |
| −2 147 483 648 … 2 147 483 647  | `int4`    | 23     |
| everything else                 | `int8`    | 20     |

PostgreSQL coerces automatically, so `$1::int4` accepts an `int2` wire value
without a cast. Use an explicit SQL cast only when you need to guarantee a
specific column type is inferred for an untyped expression.


Prepared statements
-------------------
Creating a prepared statement:
```ruby
statement = conn.prepare("mystatement", "select * from pg_type where typname = $1")
```
The statement name can be empty (but not `nil`), which defines the unnamed
(default) statement.

Executing a prepared statement:
```ruby
res = statement.exec("bool")
puts res.to_ary
```


Retrieving results row-by-row
-----------------------------
```ruby
conn.exec("select * from pg_database") do |row|
  puts row.getvalue(0, 0)
end
```
The block is called for every row of the answer. To cancel mid-stream call
`conn.cancel` — remaining results are freed. If the block raises an exception
all remaining results are freed too.

Error results are `Exception` objects but are **not** raised; you must handle
them yourself. All result errors are a subclass of `Pq::Result::Error`.


SQL NULL value
--------------
The SQL `NULL` value is returned as the symbol `:NULL`.


Error handling
--------------
Exceptions are only raised when the connection has issues or you call functions
that require a higher protocol version. Errors in result objects are exceptions
but are not raised automatically.

Each `Result::Error` exposes the `PQresultErrorField` diagnostics as methods
(see [libpq docs](https://www.postgresql.org/docs/current/libpq-exec.html#LIBPQ-PQRESULTERRORFIELD)).
The `PG_DIAG_*` constants are mapped to snake_case methods, e.g.
`PG_DIAG_SEVERITY` → `error.severity`.

`error.sqlstate` returns the [error code](https://www.postgresql.org/docs/current/errcodes-appendix.html)
as a string.

```ruby
res = conn.exec("i am a syn;tax error")
res.is_a? Pq::Result::FatalError   # => true
res.severity                        # => "ERROR"
res.sqlstate                        # => "42601"
res.message_primary                 # => "syntax error at or near \"i\""
```


Result introspection
--------------------
```ruby
res.ntuples                          # number of rows
res.nfields                          # number of columns
res.fname(column_number)             # column name (0-based)
res.fnumber(column_name)             # column number for name
res.ftablecol(column_number)         # column number within its source table
res.ftype(column_number)             # OID of the column's data type
res.getvalue(row_number, column_number)    # single field value
res.getisnull(row_number, column_number)   # true if field is NULL
```