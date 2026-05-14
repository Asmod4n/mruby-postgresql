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

`connect_poll` returns `:reading`, `:writing`, `:ok`, or `:failed`. (libpq
also defines a deprecated `:active` state; the gem exposes it for
completeness but it never appears on modern libpq.)

`status` returns symbols matching the libpq `CONNECTION_*` names in
lowercase: `:ok`, `:bad`, `:started`, `:made`, `:awaiting_response`,
`:auth_ok`, `:setenv`, `:ssl_startup`, `:needed`. Newer libpq states fall
through to the underlying integer.