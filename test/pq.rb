assert("ConnectDisconnect") do
  conn = Pq.new("postgresql://localhost/postgres")
  conn.close
  assert_raise(IOError) { conn.exec("select * from pg_database") }
end

assert("Exec without args") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select 1::text as n")
  assert_equal [["1"]], res.to_ary
  conn.close
end

assert("Exec with string arg") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::text as val", "hello")
  assert_equal [["hello"]], res.to_ary
  conn.close
end

assert("Exec returns bool true") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select true as b")
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Exec returns bool false") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select false as b")
  assert_equal [[false]], res.to_ary
  conn.close
end

assert("NULL value returned as :NULL") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select null as n")
  assert_equal [[:NULL]], res.to_ary
  conn.close
end

# Integer encoding — value-based dispatch
assert("Integer int2 boundary: 0") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int2 = 0", 0)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer int2 boundary: 32767 (INT16_MAX)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int2 = 32767", 32767)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer int2 boundary: -32768 (INT16_MIN)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int2 = -32768", -32768)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer int4 boundary: 32768 (INT16_MAX+1)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int4 = 32768", 32768)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer round-trip: negative int2") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int2", -1)
  assert_equal [[-1]], res.to_ary
  conn.close
end

assert("Integer round-trip: int4 via arithmetic") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int4 + $2::int4", 1000000, 2000000)
  assert_equal [[3000000]], res.to_ary
  conn.close
end

assert("Float round-trip") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::float8 > 3.0", 3.14)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Syntax error returns FatalError result") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("this is not sql")
  assert_true res.is_a?(Pq::Result::FatalError)
  assert_equal "42601", res.sqlstate
  conn.close
end

assert("Prepared statement exec") do
  conn = Pq.new("postgresql://localhost/postgres")
  stmt = conn.prepare("test_stmt", "select $1 * 2")
  res = stmt.exec(40000)
  assert_equal [[80000]], res.to_ary
  conn.close
end

assert("Result metadata: ntuples and nfields") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select 1 as a, 2 as b union all select 3, 4")
  assert_equal 2, res.ntuples
  assert_equal 2, res.nfields
  conn.close
end

assert("Result metadata: fname and fnumber") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select 1 as mycolumn")
  assert_equal "mycolumn", res.fname(0)
  assert_equal 0, res.fnumber("mycolumn")
  conn.close
end

assert("Block row-by-row iteration") do
  conn = Pq.new("postgresql://localhost/postgres")
  rows = []
  conn.exec("select generate_series(1,3) as n") do |row|
    rows << row.getvalue(0, 0)
  end
  assert_equal [1, 2, 3], rows
  conn.close
end

assert("Multiple params") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::text || $2::text", "foo", "bar")
  assert_equal [["foobar"]], res.to_ary
  conn.close
end

# ===========================================================================
# Async / event-loop API
# ===========================================================================
#
# These tests drive libpq's async API the way real event-loop code would:
# IO.select on the connection socket whenever connect_poll / flush /
# consume_input asks us to wait. Requires mruby-io (declared as a test
# dependency in mrbgem.rake).
#
# We wrap conn.socket in an IO purely so IO.select has something to take.
# The fd is owned by libpq, so we flip `io.autoclose = false` to stop the
# IO finalizer from closing libpq's socket behind its back.

ASYNC_TIMEOUT = 5  # seconds — generous; localhost replies in microseconds

def wrap_socket(conn)
  io = IO.for_fd(conn.socket)
  io.autoclose = false
  io
end

assert("Async: connect_start drives to :ok via IO.select") do
  conn = Pq.connect_start("postgresql://localhost/postgres")
  io = wrap_socket(conn)
  loop do
    case conn.connect_poll
    when :ok      then break
    when :failed  then raise "connect failed: #{conn.error_message}"
    when :reading then IO.select([io], nil, nil, ASYNC_TIMEOUT)
    when :writing then IO.select(nil, [io], nil, ASYNC_TIMEOUT)
    end
  end
  assert_equal :ok, conn.status
  assert_true conn.socket >= 0
  conn.close
end

assert("Async: connect_start raises on obviously bad conninfo") do
  assert_raise(Pq::ConnectionError) do
    Pq.connect_start("postgresql://localhost:1/postgres invalid_option=oops")
  end
end

assert("Async: nonblocking toggle") do
  conn = Pq.new("postgresql://localhost/postgres")
  assert_false conn.nonblocking?
  conn.nonblocking = true
  assert_true  conn.nonblocking?
  conn.nonblocking = false
  assert_false conn.nonblocking?
  conn.close
end

assert("Async: send_query + flush + consume_input + get_result") do
  conn = Pq.new("postgresql://localhost/postgres")
  io = wrap_socket(conn)
  conn.nonblocking = true
  conn.send_query("select 1::int4 as n")

  # Push outgoing data (on localhost flush returns 0 immediately)
  while (r = conn.flush) != 0
    IO.select(nil, [io], nil, ASYNC_TIMEOUT)
  end

  # Wait until the server has responded
  while conn.busy?
    IO.select([io], nil, nil, ASYNC_TIMEOUT)
    conn.consume_input
  end

  res = conn.get_result
  assert_kind_of Pq::Result, res
  assert_equal [[1]], res.to_ary
  # PQgetResult must be called until it returns nil to fully drain
  assert_nil conn.get_result
  conn.close
end

assert("Async: send_query with parameters") do
  conn = Pq.new("postgresql://localhost/postgres")
  io = wrap_socket(conn)
  conn.send_query("select $1::int4 + $2::int4", 100, 200)
  while (r = conn.flush) != 0
    IO.select(nil, [io], nil, ASYNC_TIMEOUT)
  end
  while conn.busy?
    IO.select([io], nil, nil, ASYNC_TIMEOUT)
    conn.consume_input
  end
  assert_equal [[300]], conn.get_result.to_ary
  assert_nil conn.get_result
  conn.close
end

assert("Async: send_prepare + send_query_prepared") do
  conn = Pq.new("postgresql://localhost/postgres")
  io = wrap_socket(conn)

  conn.send_prepare("async_stmt", "select $1::text || '!'")
  while (r = conn.flush) != 0
    IO.select(nil, [io], nil, ASYNC_TIMEOUT)
  end
  while conn.busy?
    IO.select([io], nil, nil, ASYNC_TIMEOUT)
    conn.consume_input
  end
  prep = conn.get_result
  assert_false prep.is_a?(Pq::Result::Error)
  assert_nil conn.get_result

  conn.send_query_prepared("async_stmt", "hi")
  while (r = conn.flush) != 0
    IO.select(nil, [io], nil, ASYNC_TIMEOUT)
  end
  while conn.busy?
    IO.select([io], nil, nil, ASYNC_TIMEOUT)
    conn.consume_input
  end
  assert_equal [["hi!"]], conn.get_result.to_ary
  assert_nil conn.get_result
  conn.close
end

assert("Async: set_single_row_mode streams one row at a time") do
  conn = Pq.new("postgresql://localhost/postgres")
  io = wrap_socket(conn)
  conn.send_query("select generate_series(1,3)::int4")
  assert_true conn.set_single_row_mode

  rows = []
  loop do
    while conn.busy?
      IO.select([io], nil, nil, ASYNC_TIMEOUT)
      conn.consume_input
    end
    res = conn.get_result
    break if res.nil?
    next if res.ntuples == 0   # the final TUPLES_OK marker after the rows
    rows << res.getvalue(0, 0)
  end
  assert_equal [1, 2, 3], rows
  conn.close
end

assert("Async: multi-statement send_query yields multiple results") do
  conn = Pq.new("postgresql://localhost/postgres")
  io = wrap_socket(conn)
  conn.send_query("select 1::int4; select 2::int4")
  while (r = conn.flush) != 0
    IO.select(nil, [io], nil, ASYNC_TIMEOUT)
  end

  results = []
  loop do
    while conn.busy?
      IO.select([io], nil, nil, ASYNC_TIMEOUT)
      conn.consume_input
    end
    res = conn.get_result
    break if res.nil?
    results << res.to_ary
  end
  assert_equal [[[1]], [[2]]], results
  conn.close
end

assert("Async: notifies returns nil when nothing pending") do
  conn = Pq.new("postgresql://localhost/postgres")
  assert_nil conn.notifies
  conn.close
end

assert("Async: notifies returns Pq::Notify after NOTIFY") do
  conn = Pq.new("postgresql://localhost/postgres")
  conn.exec("LISTEN mychan")
  conn.exec("NOTIFY mychan, 'payload'")
  # A round-trip query absorbs any pending notification onto our side
  conn.exec("select 1")
  n = conn.notifies
  assert_kind_of Pq::Notify, n
  assert_equal "mychan",  n.relname
  assert_equal "payload", n.extra
  assert_true n.be_pid > 0
  conn.close
end

assert("Async: error_message returns a String") do
  conn = Pq.new("postgresql://localhost/postgres")
  assert_kind_of String, conn.error_message
  conn.close
end

# ===========================================================================
# COPY (manual, row-by-row)
# ===========================================================================

assert("COPY: round-trip via put_copy_data / get_copy_data") do
  conn = Pq.new("postgresql://localhost/postgres")
  conn.exec("create temp table copy_test (id int4, word text)")

  res = conn.exec("COPY copy_test FROM STDIN")
  assert_true res.copy_in?
  assert_true conn.put_copy_data("1\tfoo\n")
  assert_true conn.put_copy_data("2\tbar\n")
  assert_true conn.put_copy_end
  final = conn.get_result
  assert_true final.command_ok?
  assert_nil conn.get_result

  res = conn.exec("COPY copy_test TO STDOUT")
  assert_true res.copy_out?
  rows = []
  while (row = conn.get_copy_data)
    rows << row
  end
  assert_equal ["1\tfoo\n", "2\tbar\n"], rows
  final = conn.get_result
  assert_true final.command_ok?
  assert_nil conn.get_result

  # the connection stays usable after a completed COPY
  assert_equal [[2]], conn.exec("select count(*)::int4 from copy_test").to_ary
  conn.close
end

assert("COPY: put_copy_end with message aborts, connection recovers") do
  conn = Pq.new("postgresql://localhost/postgres")
  conn.exec("create temp table copy_abort_test (id int4)")

  res = conn.exec("COPY copy_abort_test FROM STDIN")
  assert_true res.copy_in?
  assert_true conn.put_copy_data("1\n")
  assert_true conn.put_copy_end("aborted by test")
  final = conn.get_result
  assert_true final.is_a?(Pq::Result::FatalError)
  assert_nil conn.get_result

  # the abort discarded the pending row and the connection stays usable
  assert_equal [[0]], conn.exec("select count(*)::int4 from copy_abort_test").to_ary
  conn.close
end

# ===========================================================================
# Primitives for tooling (migration gems etc.)
# ===========================================================================

assert("transaction_status follows BEGIN/error/ROLLBACK") do
  conn = Pq.new("postgresql://localhost/postgres")
  assert_equal :idle, conn.transaction_status
  conn.exec("BEGIN")
  assert_equal :intrans, conn.transaction_status
  conn.exec("this is not sql")           # error result -> aborted transaction
  assert_equal :inerror, conn.transaction_status
  conn.exec("ROLLBACK")
  assert_equal :idle, conn.transaction_status
  conn.close
  assert_raise(IOError) { conn.transaction_status }
end

assert("escape_identifier quotes and doubles embedded quotes") do
  conn = Pq.new("postgresql://localhost/postgres")
  assert_equal "\"users\"", conn.escape_identifier("users")
  assert_equal "\"weird \"\"name\"\"\"", conn.escape_identifier("weird \"name\"")
  # usable in real SQL
  res = conn.exec("SELECT 1::int4 AS #{conn.escape_identifier("weird \"col\"")}")
  assert_equal "weird \"col\"", res.fname(0)
  conn.close
end

assert("escape_literal quotes string values") do
  conn = Pq.new("postgresql://localhost/postgres")
  assert_equal "'it''s'", conn.escape_literal("it's")
  res = conn.exec("SELECT #{conn.escape_literal("it's")}::text")
  assert_equal [["it's"]], res.to_ary
  conn.close
end

assert("cmd_status and cmd_tuples report command outcomes") do
  conn = Pq.new("postgresql://localhost/postgres")
  conn.exec("CREATE TEMP TABLE prim_t (id int4)")
  res = conn.exec("INSERT INTO prim_t VALUES (1), (2)")
  assert_equal "INSERT 0 2", res.cmd_status
  assert_equal 2, res.cmd_tuples
  assert_equal 2, conn.exec("UPDATE prim_t SET id = id + 1").cmd_tuples
  assert_equal 1, conn.exec("DELETE FROM prim_t WHERE id = 2").cmd_tuples
  res = conn.exec("CREATE TEMP TABLE prim_t2 (id int4)")
  assert_equal "CREATE TABLE", res.cmd_status
  assert_nil res.cmd_tuples               # no row count for DDL
  conn.close
end

assert("Result#error_message returns the full server message") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("this is not sql")
  assert_include res.error_message, "syntax error"
  assert_equal "", conn.exec("SELECT 1").error_message
  conn.close
end

assert("ftable_oid / ftablecol_num return libpq's raw answers, never raise") do
  conn = Pq.new("postgresql://localhost/postgres")
  conn.exec("create temp table ftable_raw_t (id int4)")
  res = conn.exec("select id, 1 as computed from ftable_raw_t")

  assert_true res.ftable_oid(0) > 0                  # a real table's oid
  assert_equal res.ftable_oid(0), res.ftable(0)      # agrees with the raising form
  assert_equal 1, res.ftablecol_num(0)               # first column of the table

  assert_equal Pq::InvalidOid, res.ftable_oid(1)     # computed: defined answer...
  assert_equal 0, res.ftablecol_num(1)
  assert_raise(Pq::InvalidOidError) { res.ftable(1) } # ...where ftable raises
  assert_raise(Pq::InvalidOidError) { res.ftablecol(1) }

  assert_equal 0, Pq::InvalidOid                     # libpq's sentinel value
  conn.close
end
