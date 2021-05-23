assert("ConnectDisconnect") do
    conn = Pq.new("postgresql://localhost/postgres")
    conn.close
    assert_raise(IOError) { conn.exec("select * from pg_database") }
end