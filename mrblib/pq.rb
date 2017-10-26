unless Object.const_defined?("IOError")
  class IOError < StandardError; end
end

class Pq
  class Error < StandardError; end
  class ConnectionError < Error; end

  class Result
    class Error < Pq::Error; end
    class InvalidOid < Error; end
    attr_reader :status

    def values
      fnames = []
      column = 0
      while column < nfields
        fnames << fname(column)
        column += 1
      end
      rows = []
      row = 0
      while row < ntuples
        line = {}
        column = 0
        while column < nfields
          line[fnames[column]] = getvalue(row, column)
          column += 1
        end
        rows << line
        row += 1
      end
      rows
    end

    def copy_in?
      status == COPY_IN
    end

    def copy_out?
      status == COPY_OUT
    end

    def single_tuple?
      status == SINGLE_TUPLE
    end
  end # class Result

  def prepare(stmt_name, query)
    _prepare(stmt_name, query)
    Stmt.new(self, stmt_name)
  end

  class Stmt
    def initialize(conn, stmt_name)
      @conn, @stmt_name = conn, stmt_name
    end

    def exec(*args)
      @conn.exec_prepared(@stmt_name, *args)
    end
  end
end
