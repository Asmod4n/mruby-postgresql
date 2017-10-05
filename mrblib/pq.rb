class Pq
  class Error < StandardError; end
  class Result
    NULL = ("NULL").freeze
    class InvalidOid < Pq::Error; end
  end
end
