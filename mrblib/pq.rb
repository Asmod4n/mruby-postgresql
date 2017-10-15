class Pq
  class Error < StandardError; end
  class ConnectionError < Error; end
  class Result
    class Error < Pq::Error; end
    class InvalidOid < Error; end

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
  end
end
