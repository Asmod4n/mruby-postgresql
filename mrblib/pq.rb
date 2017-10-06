class Pq
  class Error < StandardError; end
  class Result
    class InvalidOid < Pq::Error; end
  end
end
