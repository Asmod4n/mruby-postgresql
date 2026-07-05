unless Object.const_defined?(:IOError)
  class IOError < StandardError; end
end

class Pq
  # PostgreSQL datetime text -> Time. The connection always runs with
  # TimeZone=UTC (set at connect, like the UTF-8 client encoding), so
  # timestamptz text invariably ends in "+00" and one Time.gm call is the
  # whole decode. Anything else — BC dates, infinity, a foreign offset on
  # a connection that changed its TimeZone — returns nil and the C
  # decoder hands the user the canonical text instead.
  def self.decode_datetime(str)
    date, clock, bc = str.chomp("+00").split(" ")
    y, mo, d = date.split("-")
    return nil if bc || d.nil? || (clock && (!clock.include?(":") || clock.include?("+") || clock.include?("-")))
    h, mi, s = clock.to_s.split(":")
    s, frac = s.to_s.split(".")
    Time.gm(y.to_i, mo.to_i, d.to_i, h.to_s.to_i, mi.to_s.to_i, s.to_s.to_i, "#{frac}000000"[0, 6].to_i)
  rescue
    nil # out of time_t's range: the raw text says it better
  end

  # Rational -> decimal text for a query parameter: the inverse of the
  # numeric decoding. Exact whenever a finite decimal exists (denominator
  # 2^a * 5^b); otherwise (1/3, ...) — where PostgreSQL's numeric cannot
  # be exact either — the same 20 fractional digits PostgreSQL's own
  # numeric division produces, rounded half away from zero, so $1 with
  # Rational(1, 3) equals 1::numeric / 3 on the server.
  def self.encode_rational(r)
    den = r.denominator
    two = five = 0
    while den % 2 == 0; den /= 2; two += 1; end
    while den % 5 == 0; den /= 5; five += 1; end
    if den == 1
      exp = two > five ? two : five
      scale = 10 ** exp
      mantissa = r.numerator * scale / r.denominator
    else
      exp = 20
      scale = 10 ** exp
      mantissa = (r.numerator.abs * scale + r.denominator / 2) / r.denominator
      mantissa = -mantissa if r.numerator < 0
    end
    sign = mantissa < 0 ? "-" : ""
    whole, frac = mantissa.abs.divmod(scale)
    exp == 0 ? "#{sign}#{whole}" : "#{sign}#{whole}.#{(scale + frac).to_s[1, exp]}"
  end

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
  end # class Stmt

  class Notify
    attr_reader :relname, :be_pid, :extra
    def initialize(relname, be_pid, extra)
      @relname = relname
      @be_pid  = be_pid
      @extra   = extra
    end
  end

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
    # unlike its siblings this is never returned wrapping a PGresult — it
    # is raised (by ftable/ftablecol), so it must not inherit the
    # MRB_TT_DATA instance type of Result::Error (mruby cannot raise
    # TT_DATA objects); a plain Pq::Error subclass is raisable
    class InvalidOid < Pq::Error; end

    attr_reader :status

    def to_ary
      rows = []
      row = 0
      while row < ntuples
        line = []
        column = 0
        while column < nfields
          line << getvalue(row, column)
          column += 1
        end
        rows << line
        row += 1
      end
      rows
    end

    alias_method :values, :to_ary

    def names
      fnames = []
      column = 0
      while column < nfields
        fnames << fname(column)
        column += 1
      end
      fnames
    end
  end # class Result
end # class Pq
