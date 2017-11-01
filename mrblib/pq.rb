unless Object.const_defined?("IOError")
  class IOError < StandardError; end
end

class Pq
  class Result
    constants.each do |const|
      define_method("#{const.downcase}?") do
        status == self.class.const_get(const)
      end
    end

    class Error < Pq::Error
      constants.each do |const|
        define_method(const.downcase) do
          field(self.class.const_get(const))
        end
      end
    end
    class EmptyQueryError < Error; end
    class BadResponseError < Error; end
    class NonFatalError < Error; end
    class FatalError < Error; end
    class InvalidOid < Error; end

    attr_reader :status

    def to_ary
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

    def describe
      @conn.describe_prepared(@stmt_name)
    end
  end
end
