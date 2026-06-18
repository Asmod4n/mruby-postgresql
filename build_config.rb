MRuby::Build.new do |conf|
  toolchain :gcc
  conf.enable_debug
  conf.enable_test
  conf.gembox 'default'
  conf.gem File.expand_path(File.dirname(__FILE__))
end
