# mruby-postgresql
mruby binding for libbq from postgresql


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
puts res.values
```
