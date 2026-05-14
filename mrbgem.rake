MRuby::Gem::Specification.new('mruby-postgresql') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'Postgresql adapter for mruby'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-symbol-ext'
  spec.add_dependency 'mruby-metaprog'

  # Used by the async-API tests in test/pq.rb for IO.select on the libpq socket.
  spec.add_test_dependency 'mruby-io', core: 'mruby-io'

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
    # spec.build.toolchain is an Array of toolchain names (strings), not a
    # single symbol — `== :visualcpp` is silently always false. Use .include?.
    is_msvc    = spec.build.toolchain.include?('visualcpp')

    # On Windows, the EDB installer writes each install's base directory
    # to HKLM\SOFTWARE\PostgreSQL\Installations\postgresql-x64-<ver>.
    # Querying that is more reliable than globbing C:\Program Files —
    # picks up installs on other drives, gives us authoritative version
    # ordering, and explicitly tells us "no Postgres installed" rather
    # than guessing from the absence of a directory. Falls back to the
    # glob for installs that skipped the registry (rare: manual extract,
    # portable copy, third-party packager).
    require 'open3'
    windows_pg_roots = lambda do
      roots = []

      out, status = Open3.capture2(
        'reg', 'query',
        'HKLM\SOFTWARE\PostgreSQL\Installations',
        '/reg:64'
      )

      if status.success?
        installs = []
        out.lines.each do |line|
          line = line.strip
          # Subkey names look like ...\postgresql-x64-18. Capture the version
          # for ordering — registry enumeration order isn't guaranteed.
          next unless line =~ %r{\\postgresql-x64-(\d+)$}
          version = $1.to_i

          detail, dstatus = Open3.capture2(
            'reg', 'query', line, '/v', 'Base Directory', '/reg:64'
          )
          next unless dstatus.success?

          if detail =~ /Base Directory\s+REG_SZ\s+(.+)/i
            installs << [version, $1.strip.tr('\\', '/')]
          end
        end
        roots.concat(installs.sort_by { |v, _| -v }.map { |_, r| r })
      end

      # Supplement with glob; default installs show up in both places, but
      # the include?-guard keeps the list deduped.
      Dir.glob('C:/Program Files/PostgreSQL/*').sort.reverse.each do |root|
        roots << root unless roots.include?(root)
      end

      roots
    end

    if is_msvc
      # MSVC always means EDB-installed Postgres on Windows; the import
      # library is called libpq.lib rather than libpq.a.
      libname = 'libpq'
      windows_pg_roots.call.each do |root|
        candidates << ["#{root}/include", "#{root}/lib"]
      end
    else
      # GCC / Clang / MinGW: ask the compiler what target it builds for.
      # `.b` (force binary) avoids encoding-tag crashes on Windows where
      # backtick output is tagged with the system code page, not UTF-8.
      # Triples are ASCII so regex matching against the bytes is fine.
      triple = `#{spec.build.cc.command} -dumpmachine 2>&1`.b.strip
      case triple
      when /darwin/
        # Homebrew keeps libpq keg-only — its .pc file isn't on PKG_CONFIG_PATH
        # by default, which is why search_package missed it.
        candidates.concat [
          ['/opt/homebrew/opt/libpq/include', '/opt/homebrew/opt/libpq/lib'],   # Apple Silicon
          ['/usr/local/opt/libpq/include',    '/usr/local/opt/libpq/lib'],      # Intel
        ]
      when /mingw|cygwin/
        # Native MinGW build against an EDB-installed Postgres — same
        # discovery as MSVC, since the install layout is identical.
        windows_pg_roots.call.each do |root|
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
      spec.cc.include_paths << inc
      if is_msvc
        # mruby's visualcpp toolchain has issues both with `library_paths`
        # (no quoting, splits at the space in "Program Files") and with
        # whatever currently makes filename() see a non-string in the
        # libraries/dependencies map. Sidestep both by passing the full,
        # quoted path to libpq.lib as a single positional linker arg —
        # the MSVC linker recognises .lib files directly as input, no
        # /LIBPATH: needed, no library-name lookup needed.
        spec.linker.flags_before_libraries << %Q{"#{File.join(lib, 'libpq.lib')}"}

        # On Windows, PostgreSQL's DLLs aren't on the system PATH by
        # default — the EDB installer doesn't add
        # `C:\Program Files\PostgreSQL\<ver>\bin`. Anything linked
        # against libpq.lib then fails to start at run time with
        # "libpq.dll could not be found" because the imports are
        # resolved at load. Rake `file` tasks declared in mrbgem.rake
        # don't get wired into mruby's build graph, so stage the DLLs
        # eagerly here at gem-setup time — by the time the linker runs
        # and the test driver invokes mrbtest.exe, the DLLs are already
        # next to it. Idempotent: re-copies only when the source is newer.
        pg_bin     = File.expand_path('../bin', lib)
        target_bin = "#{spec.build.build_dir}/bin"
        FileUtils.mkdir_p(target_bin)
        Dir.glob("#{pg_bin}/*.dll").each do |src|
          dst = "#{target_bin}/#{File.basename(src)}"
          next if File.exist?(dst) && File.mtime(dst) >= File.mtime(src)
          FileUtils.cp(src, dst)
        end
      else
        spec.linker.library_paths << lib if File.directory?(lib)
        spec.linker.libraries     << libname
      end
    else
      raise "mruby-postgresql: cannot find libpq. " \
            "Tried pkg-config 'libpq' and platform-specific install locations. " \
            "Install libpq-dev (Linux), `brew install libpq` (macOS), " \
            "or the PostgreSQL Windows installer."
    end
  end
end
