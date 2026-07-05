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


Type mapping
------------
Parameters (Ruby -> PostgreSQL):

| Ruby value            | Sent as                                            |
|-----------------------|----------------------------------------------------|
| `nil`, `:NULL`        | SQL `NULL`                                         |
| `true` / `false`      | `bool`                                             |
| `Integer`             | `int2`/`int4`/`int8` (binary, smallest that fits)  |
| `Float`               | `float4`/`float8` (binary)                         |
| `String`              | untyped text (PostgreSQL infers from context)      |
| `Symbol`              | its name as untyped text                           |
| `Time`                | `timestamptz` (UTC, microsecond precision)         |
| `Rational`            | decimal text (untyped): exact when finite,         |
|                       | else the same 20 fractional digits PostgreSQL's    |
|                       | own numeric division produces                      |
| `Array`               | array literal; element type from the first typed   |
|                       | leaf (`int8[]`, `float8[]`, `text[]`, `bool[]`,    |
|                       | `timestamptz[]`); nested arrays are multidim; an   |
|                       | all-`nil`/empty array is untyped — add a cast      |
| anything else         | `to_str` as untyped text (serialize a Hash        |
|                       | yourself if you mean to send JSON)                 |

Results (PostgreSQL -> Ruby), text format:

| PostgreSQL type                     | Decoded to                           |
|-------------------------------------|--------------------------------------|
| `bool`                              | `true` / `false`                     |
| `int2`, `int4`, `int8`, `oid`       | `Integer`                            |
| `float4`, `float8`                  | `Float`                              |
| `numeric`                           | exact: `Integer` when integral       |
|                                     | (arbitrary size via mruby-bigint),   |
|                                     | `Rational` when fractional (via      |
|                                     | mruby-rational, both are gem deps);  |
|                                     | `NaN`/`±Infinity` -> `Float`; cast   |
|                                     | `::float8` for an approximate Float  |
| `date`                              | `Time` at UTC midnight               |
| `timestamp`                         | `Time` (interpreted as UTC)          |
| `timestamptz`                       | `Time` (offset applied, in UTC)      |
| `bytea`                             | binary `String`                      |
| `json`, `jsonb`                     | via `JSON.parse` when available      |
| `xml`                               | via `XML.parse` when available       |
| `void`                              | `nil`                                |
| SQL `NULL`                          | `:NULL`                              |
| everything else (`uuid`, `inet`,    | `String` — PostgreSQL's canonical    |
| `interval`, `time`, arrays, enums,  | text form (`unnest` / `to_jsonb`     |
| `money`, ranges, composites, ...)   | in SQL when you want structure)      |

When you want structure out of the canonical-text types, ask PostgreSQL —
its own output functions convert anything into types this gem decodes:

```sql
select to_jsonb(u) from users u         -- whole row -> Ruby Hash
select jsonb_agg(p.title) ...           -- aggregate straight into jsonb
select hstore_to_jsonb(attrs) ...       -- hstore -> Ruby Hash
select to_jsonb(compo) ...              -- composite/range/enum -> JSON
select unnest(tags) ...                 -- or any array -> plain rows
```

(Note that JSON flattens type fidelity: timestamps arrive as ISO-8601
strings and exact numerics as JSON numbers — keep values you need typed
as their own columns.)

Decoding never raises: anything that fails to parse (e.g. `'infinity'` /
BC timestamps, which `Time` cannot represent) comes back as the raw text.
`Time` decoding requires mruby-time (part of the default gembox); without
it the date/timestamp types also fall back to text.


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

Note that this form still drives `PQgetResult` synchronously under the hood:
it streams in single-row mode, but the calling thread still blocks on the
socket. For true non-blocking row streaming use `send_query` + `set_single_row_mode`
(see the async section below).

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
res.ftable_oid(column_number)        # like ftable, but 1:1 with libpq: returns
                                     # Pq::InvalidOid instead of raising for
                                     # computed columns
res.ftablecol_num(column_number)     # like ftablecol: returns 0 instead of
                                     # raising
res.ftype(column_number)             # OID of the column's data type
res.getvalue(row_number, column_number)    # single field value
res.getisnull(row_number, column_number)   # true if field is NULL
```


Notice receivers
----------------
Register a block to be called whenever PostgreSQL emits a notice or warning:
```ruby
conn.notice_receiver do |notice|
  warn notice.message_primary
end
```
The block receives a `Pq::Result::NonFatalError` (or similar) wrapping a
private copy of the notice — libpq's own buffer is freed as soon as the
callback returns, so the copy is what keeps it alive on the Ruby side.

The block runs inside a libpq C callback, so a raise out of it cannot
unwind directly (longjmp across libpq's stack is undefined behaviour).
Instead, the exception is caught locally and parked in `mrb->exc`; the
enclosing mruby C wrapper (`exec`, `consume_input`, whatever was driving
libpq when the notice fired) returns to the VM, which raises naturally on
its next opcode. From the caller's point of view a block raise in
`notice_receiver` looks identical to a raise from regular code, with the
exception surfacing at the call site that triggered the notice.

Subsequent notices in the same libpq call are skipped once an exception
is pending, so the first raise wins.


Asynchronous / event-loop integration
-------------------------------------
The methods above (`Pq.new`, `exec`, `prepare`, `exec_prepared`, …) all do
their I/O synchronously and will stall an event loop while waiting for the
server. For non-blocking use the gem also exposes thin wrappers around
libpq's async API.

The pattern is always:

1. Initiate work with a non-blocking call (`connect_start`, `send_query`, …).
2. Drive the socket with `flush` (write side) and `consume_input` (read
   side), waking up via `conn.socket` when your event loop says it is
   readable or writable.
3. Pull completed results out with `get_result` until it returns `nil`.

### Async connect

```ruby
conn = Pq.connect_start("postgresql://localhost/postgres")
loop do
  case conn.connect_poll
  when :ok      then break
  when :failed  then raise Pq::ConnectionError, conn.error_message
  when :reading then your_loop.wait_readable(conn.socket)
  when :writing then your_loop.wait_writable(conn.socket)
  end
end
```

`connect_start` accepts the same URL string as `Pq.new` (`postgresql://…`).
It returns an instance whose underlying `PGconn` is mid-handshake; the Ruby
`#initialize` is **not** called on it, so the synchronous `PQconnectdb`
path is bypassed entirely.

### Async query

```ruby
conn.nonblocking = true
conn.send_query("select * from foo where id = $1", 42)

# Push outgoing data onto the socket
while (r = conn.flush) != 0
  your_loop.wait_writable(conn.socket)
end

# Read incoming results
loop do
  while conn.busy?
    your_loop.wait_readable(conn.socket)
    conn.consume_input
  end
  res = conn.get_result
  break if res.nil?            # nil = all results drained
  # handle res …
end
```

After `send_query` returns, `get_result` must keep being called until it
returns `nil`, even if you only expected one result. This matters for
multi-statement queries (`"select 1; select 2"` yields two results) and for
the trailing `TUPLES_OK` marker in single-row mode.

`flush` returns `0` when fully flushed and `1` when data is still queued;
`-1` raises.

### A concrete event loop with IO.select

If `mruby-io` is available, `IO.select` is the simplest wakeup mechanism.
The fd belongs to libpq, so flip `autoclose = false` on the wrapper to
keep the IO finalizer from closing it out from under us:

```ruby
io = IO.for_fd(conn.socket)
io.autoclose = false

# wait_readable / wait_writable equivalents:
IO.select([io], nil, nil, timeout)   # readable
IO.select(nil, [io], nil, timeout)   # writable
```

Substitute these `IO.select` calls for the `your_loop.wait_*` lines in the
examples above.

### Streaming rows

Call `conn.set_single_row_mode` immediately after `send_query` (and before
the first `get_result`) to receive one `Pq::Result` per row instead of one
buffered `Pq::Result` for the whole query. The end of the stream is marked
by a zero-row `Pq::Result`; skip it with `next if res.ntuples == 0`.

### LISTEN / NOTIFY

```ruby
conn.exec("LISTEN events")
loop do
  your_loop.wait_readable(conn.socket)
  conn.consume_input
  while (n = conn.notifies)
    puts "#{n.relname} from pid #{n.be_pid}: #{n.extra}"
  end
end
```

`n` is a `Pq::Notify` with `relname`, `be_pid`, and `extra` accessors.

### COPY (manual, row-by-row)

The gem deliberately ships no bulk COPY helpers — mruby caps strings at 1MB
on some platforms, and file-based bulk loading is already served by `psql`.
What it wraps is the minimal per-row API, so a `COPY` issued manually
through `exec` can be driven (or aborted) instead of wedging the connection.
No call ever handles more than one row at a time, so the string cap is
never in play.

Sending data (`COPY ... FROM STDIN`):
```ruby
res = conn.exec("COPY mytable FROM STDIN")
raise "unexpected #{res.status}" unless res.copy_in?
conn.put_copy_data("1\tfoo\n")
conn.put_copy_data("2\tbar\n")
conn.put_copy_end
while (res = conn.get_result); end   # drain the final COMMAND_OK / error
```

Receiving data (`COPY ... TO STDOUT`):
```ruby
res = conn.exec("COPY mytable TO STDOUT")
raise "unexpected #{res.status}" unless res.copy_out?
while (row = conn.get_copy_data)
  print row                          # one data row per call
end
while (res = conn.get_result); end
```

`get_copy_data` takes no arguments — whether it blocks follows the
connection's own nonblocking state. It returns one row per call, `nil` once
the COPY is finished (then drain `get_result`), or `:would_block` when the
connection is in nonblocking mode and no complete row has arrived yet (wait
readable, `consume_input`, retry).

`put_copy_data` and `put_copy_end` return `true` when the data was queued
and `false` when the send would block in nonblocking mode (wait writable,
`flush`, retry).

`put_copy_end("some message")` force-fails the COPY server-side. This is
also the escape hatch when SQL fed through `exec` unexpectedly starts a
COPY: abort it, drain `get_result`, and the connection stays usable — no
`reset` needed.

Don't use the block form of `exec` for COPY statements — it drives
single-row mode and cannot service the copy protocol.

### Method reference

| Method                                | Wraps                       |
|---------------------------------------|-----------------------------|
| `Pq.connect_start(url = "")`          | `PQconnectStart`            |
| `conn.connect_poll`                   | `PQconnectPoll`             |
| `conn.status`                         | `PQstatus`                  |
| `conn.error_message`                  | `PQerrorMessage`            |
| `conn.nonblocking = bool`             | `PQsetnonblocking`          |
| `conn.nonblocking?`                   | `PQisnonblocking`           |
| `conn.send_query(sql, *args)`         | `PQsendQuery` / `PQsendQueryParams`     |
| `conn.send_prepare(name, sql)`        | `PQsendPrepare`             |
| `conn.send_query_prepared(name, ...)` | `PQsendQueryPrepared`       |
| `conn.send_describe_prepared(name)`   | `PQsendDescribePrepared`    |
| `conn.send_describe_portal(portal)`   | `PQsendDescribePortal`      |
| `conn.flush`                          | `PQflush`                   |
| `conn.consume_input`                  | `PQconsumeInput`            |
| `conn.busy?`                          | `PQisBusy`                  |
| `conn.get_result`                     | `PQgetResult`               |
| `conn.set_single_row_mode`            | `PQsetSingleRowMode`        |
| `conn.notifies`                       | `PQnotifies`                |
| `conn.put_copy_data(data)`            | `PQputCopyData`             |
| `conn.put_copy_end(message = nil)`    | `PQputCopyEnd`              |
| `conn.get_copy_data`                  | `PQgetCopyData`             |
| `conn.transaction_status`             | `PQtransactionStatus`       |
| `conn.escape_identifier(str)`         | `PQescapeIdentifier`        |
| `conn.escape_literal(str)`            | `PQescapeLiteral`           |
| `res.cmd_status`                      | `PQcmdStatus`               |
| `res.cmd_tuples`                      | `PQcmdTuples`               |
| `res.error_message`                   | `PQresultErrorMessage`      |

`connect_poll` returns `:reading`, `:writing`, `:ok`, or `:failed`. (libpq
also defines a deprecated `:active` state; the gem exposes it for
completeness but it never appears on modern libpq.)

`status` returns symbols matching the libpq `CONNECTION_*` names in
lowercase: `:ok`, `:bad`, `:started`, `:made`, `:awaiting_response`,
`:auth_ok`, `:setenv`, `:ssl_startup`, `:needed`. Newer libpq states fall
through to the underlying integer.

Building tools on top (migrations etc.)
---------------------------------------
Higher-level concerns like schema migrations are deliberately **not** part
of this gem — they belong in a database-agnostic gem that drives multiple
backends (PostgreSQL, SQLite, MySQL, ...) through per-backend adapters.
This gem exposes the primitives such an adapter needs:

- `conn.exec(sql, *params)` — parameterized execution; errors come back as
  result objects with full diagnostics (`sqlstate`, `error_message`, ...)
- transactions are plain SQL (`BEGIN` / `COMMIT` / `ROLLBACK`), and
  `conn.transaction_status` reports where the connection stands:
  `:idle`, `:active`, `:intrans` (in a transaction), `:inerror` (in an
  aborted transaction), `:unknown`
- `conn.escape_identifier(name)` / `conn.escape_literal(value)` — libpq's
  own quoting for the places `$1` parameters cannot go (dynamic table
  names, DDL)
- `res.cmd_tuples` — rows affected by INSERT/UPDATE/DELETE (`nil` when the
  command reports no count), `res.cmd_status` — the command tag
- `res.error_message` — the full server error message of a result
  (`Exception#message` cannot read it on these data-wrapped objects)
