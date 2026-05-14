MRuby::Gem::Specification.new('mruby-postgresql') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'Postgresql adapter for mruby'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-symbol-ext'
  spec.add_dependency 'mruby-metaprog'

  # Used by the async-API tests in test/pq.rb for IO.select on the libpq socket.
  spec.add_test_dependency 'mruby-io'

  unless spec.search_package('libpq')
    # pkg-config either isn't installed or doesn't know about libpq.
    # Two cases to handle differently:
    #
    #   * Cross-build (spec.build.is_a?(MRuby::CrossBuild)) — host
    #     filesystem ≠ target filesystem, so probing /usr/include/postgresql
    #     etc. would point at the *build host's* libpq even if a copy
    #     exists. The configured host_target ("x86_64-w64-mingw32",
    #     "aarch64-linux-gnu", …) is the real target triple; surface it
    #     in the error so the user knows which sysroot needs libpq.pc.
    #
    #   * Native build — host == target, so probing is fine. Trust the
    #     toolchain (catches MSVC unambiguously) and ask gcc/clang for
    #     its target via -dumpmachine for everything else.
    if spec.build.is_a?(MRuby::CrossBuild)
      raise "mruby-postgresql: cross-build to '#{spec.build.host_target}' " \
            "couldn't find libpq via pkg-config. Install libpq.pc into your " \
            "target sysroot (so cross-pkg-config picks it up) or point " \
            "PKG_CONFIG_PATH / PKG_CONFIG_SYSROOT_DIR at it. Probing the " \
            "build host for libpq would link the wrong binary."
    end

    candidates = []
    libname    = 'pq'

    if spec.build.toolchain == :visualcpp
      # MSVC: only one realistic install layout — EDB's Windows installer
      # puts headers and import libs under C:\Program Files\PostgreSQL\<ver>\.
      # Newest version wins.
      libname = 'libpq'
      Dir.glob('C:/Program Files/PostgreSQL/*').sort.reverse.each do |root|
        candidates << ["#{root}/include", "#{root}/lib"]
      end
    else
      # GCC / Clang / MinGW: ask the compiler what target it builds for.
      triple = `#{spec.build.cc.command} -dumpmachine 2>&1`.strip
      case triple
      when /darwin/
        # Homebrew keeps libpq keg-only — its .pc file isn't on PKG_CONFIG_PATH
        # by default, which is why search_package missed it.
        candidates.concat [
          ['/opt/homebrew/opt/libpq/include', '/opt/homebrew/opt/libpq/lib'],   # Apple Silicon
          ['/usr/local/opt/libpq/include',    '/usr/local/opt/libpq/lib'],      # Intel
        ]
      when /mingw|cygwin/
        # Native MinGW build against an EDB-installed Postgres.
        Dir.glob('C:/Program Files/PostgreSQL/*').sort.reverse.each do |root|
          candidates << ["#{root}/include", "#{root}/lib"]
        end
      else
        # Linux / BSD: libpq-dev usually puts the header in the standard
        # place even on systems with no pkg-config installed; FreeBSD
        # ports drops it under /usr/local.
        candidates.concat [
          ['/usr/include/postgresql',       '/usr/lib'],
          ['/usr/include',                  '/usr/lib'],
          ['/usr/local/include/postgresql', '/usr/local/lib'],
        ]
      end
    end

    inc, lib = candidates.find { |i, _| File.exist?("#{i}/libpq-fe.h") }
    if inc
      spec.cc.include_paths     << inc
      spec.linker.library_paths << lib if File.directory?(lib)
      spec.linker.libraries     << libname
    else
      raise "mruby-postgresql: cannot find libpq. " \
            "Tried pkg-config 'libpq' and platform-specific install locations. " \
            "Install libpq-dev (Linux), `brew install libpq` (macOS), " \
            "or the PostgreSQL Windows installer."
    end
  end
end
