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
puts res.to_ary
```

With arguments
```ruby
res = conn.exec("select * from pg_type where typname = $1", "bool")
puts res.to_ary
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
puts res.to_ary
```

Retrieving Results Row-by-Row
-----------------------------
```ruby
conn.exec("select * from pg_database") do |row|
  puts row.getvalue(0, 0)
end
```
The block gets called for every row of the answer, if you want to cancel it while its running call ```conn.cancel```, the still awaiting results are freed then. If your block raises a exception all remaining results are freed too.
Error results from the answer are Exception objects but aren't raised, you have to handle them yourself, all result Errors are a subclass of Pq::Result::Error.

SQL NULL value
--------------
The SQL NULL value is returned as the symbol :NULL

Error Handling
--------------
Exceptions are only raised when the connection has issues or you are trying to use functions which need a higher protocol version.
Errors in Result Objects are Exceptions, but aren't raised.
Each Result Error has several fields which describe the Error, take a look at the ```PQresultErrorField``` function from https://www.postgresql.org/docs/current/static/libpq-exec.html#libpq-exec-main, the PG_DIAG constants are mapped as ruby methods, e.g. PG_DIAG_SEVERITY is mapped as error.severity.
The error.sqlstate method returns error codes (as strings), they are explained here: https://www.postgresql.org/docs/current/static/errcodes-appendix.html
```ruby
res = conn.exec ("i am a syn;tax error")
res.is_a? Pq::Result::FatalError
res.severity == "ERROR"
res.sqlstate == "42601"
res.message_primary == "syntax error at or near \"i\""
```


Getting more info about a Result
--------------------------------
res.ntuples # Returns the number of rows (tuples) in the query result.

res.nfields # Returns the number of columns (fields) in each row of the query result.

res.fname(column_number) # Returns the column name associated with the given column number. Column numbers start at 0.

res.fnumber(column_name) # Returns the column number associated with the given column name.

res.ftablecol(column_number) # Returns the column number (within its table) of the column making up the specified query result column. Query-result column numbers start at 0, but table columns have nonzero numbers.

res.ftype # Returns the data type associated with the given column number. The integer returned is the internal OID number of the type. Column numbers start at 0.

res.getvalue(row_number, column_number) # Returns a single field value of one row of a PGresult. Row and column numbers start at 0.

res.getisnull(row_number, column_number) # Tests a field for a null value. Row and column numbers start at 0.
