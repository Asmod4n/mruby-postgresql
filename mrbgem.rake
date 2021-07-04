MRuby::Gem::Specification.new('mruby-postgresql') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'Postgresql adapter for mruby'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-symbol-ext'
  spec.add_dependency 'mruby-metaprog'

  unless spec.search_package('libpq')
    raise "mruby-postgresql: cannot find libpq development headers and libraries, please install it"
  end
end
