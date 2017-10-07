# mruby-postgresql
mruby binding for the libbq from postgresql

This is just a simple wrapper around libpq from postgresql.


Connection
----------
Returns a connection object
```ruby
conn = Pq.new("postgresql://localhost/postgres")
```

Executing queries
-----------------
```ruby
res = conn.exec("select * from pg_database")
res.nfields.times do |i|
  print res.fname(i) + " "
end
puts
res.ntuples.times do |i|
  res.nfields.times do |j|
    print res.getvalue(i, j)
    print " "
  end
  puts
end
```
