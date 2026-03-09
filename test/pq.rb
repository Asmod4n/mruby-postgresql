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

assert("Integer int4 boundary: 2147483647 (INT32_MAX)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int4 = 2147483647", 2147483647)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer int4 boundary: -2147483648 (INT32_MIN)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int4 = -2147483648", -2147483648)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer int8 boundary: 2147483648 (INT32_MAX+1)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int8 = 2147483648", 2147483648)
  assert_equal [[true]], res.to_ary
  conn.close
end

assert("Integer int8 boundary: -2147483649 (INT32_MIN-1)") do
  conn = Pq.new("postgresql://localhost/postgres")
  res = conn.exec("select $1::int8 = -2147483649", -2147483649)
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
