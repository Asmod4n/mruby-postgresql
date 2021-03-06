MRuby::Gem::Specification.new('mruby-postgresql') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'Postgresql adapter for mruby'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-symbol-ext'

  next if spec.respond_to?(:search_package) && spec.search_package('libpq')

  create_build_dir_cmd = "mkdir -p #{spec.build_dir}/build && cd #{spec.build_dir}/build"
  configure_cmd = "#{spec.dir}/deps/postgresql/configure CC=#{spec.cc.command} CFLAGS=\"#{spec.cc.flags.join(' ')}\" LDFLAGS=\"#{spec.linker.flags.join(' ')}\" CXX=#{spec.cxx.command} CXXFLAGS=\"#{spec.cxx.flags.join(' ')}\" --prefix=#{spec.build_dir}"
  build_cmd = "cd src/interfaces/libpq && make -j4 && make install && cd #{spec.build_dir}/build/src/backend && make -j4 generated-headers && cd #{spec.build_dir}/build/src/include && make install"

  if build.is_a?(MRuby::CrossBuild) && build.host_target && build.build_target
    unless File.exists?("#{spec.build_dir}/lib/libpq.a")
      sh "#{create_build_dir_cmd} && #{configure_cmd} --host=#{build.host_target} --build=#{build.build_target} && #{build_cmd}"
    end
    spec.linker.flags_before_libraries << "\"#{spec.build_dir}/lib/libpq.a\""
    spec.cc.include_paths << "#{spec.build_dir}/include"
    build.cc.include_paths << "#{spec.build_dir}/include"
  elsif spec.cc.search_header_path 'libpq-fe.h'
    spec.linker.libraries << 'pq'
  else
    unless File.exists?("#{spec.build_dir}/lib/libpq.a")
      if spec.cc.search_header_path('openssl/crypto.h') && spec.cc.search_header_path('openssl/ssl.h')
        configure_cmd << " --with-openssl"
      end
      sh "#{create_build_dir_cmd} && #{configure_cmd} && #{build_cmd}"
    end
    spec.linker.flags_before_libraries << "\"#{spec.build_dir}/lib/libpq.a\""
    spec.cc.include_paths << "#{spec.build_dir}/include"
    build.cc.include_paths << "#{spec.build_dir}/include"
  end
end
