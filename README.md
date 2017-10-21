# mruby-postgresql
mruby binding for libpq from postgresql


Connection
----------
Connecting to a postgresql Server
```ruby
conn = Pq.new("postgresql://localhost/postgres")
```

Disconnecting
```ruby
conn.close
```
Any IO Operation afterwards raises a IOError


Executing queries
-----------------
Without arguments
```ruby
res = conn.exec("select * from pg_database")
puts res.values
```

With arguments
```ruby
res = conn.exec("select * from pg_type where typname = $1", "bool")
puts res.values
```
Passed arguments are automatically escaped to prevent SQL-injection. The first argument is $1, the second $2 and so on.

Prepared statements
-------------------
Creating a prepared statement
```ruby
statement = conn.prepare("mystatement", "select * from pg_type where typname = $1")
```
The statement name can be empty (not nil) so it defines the default statement

Executing a prepared statement
```ruby
res = statement.exec("bool")
puts res.values
```
