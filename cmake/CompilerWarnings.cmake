function(havoc_set_warnings target)
    # -Wsign-conversion is deliberately absent.
    #
    # It fired at roughly a hundred sites, every one of them a container
    # subscripted by a non-negative engine index -- a Square, a Piece, a rank, a
    # file, a move count. All of them are correct by construction, so the
    # signal-to-noise ratio was zero, and a hundred warnings in a build log is
    # how a real one goes unread.
    #
    # It also does not catch the failure it looks like it catches. The only
    # genuine out-of-bounds access in this codebase -- pinned() reading
    # bitboards::battks[65] when a position has no king -- is a raw C array
    # indexed by an enum, which produces no sign-conversion warning at all. ASan
    # and UBSan found it in the first second of the first run. That build is now
    # part of CI, which is the tripwire that actually works.
    #
    # -Wconversion stays: it caught three real narrowing conversions (U16 to U8,
    # int to U16, U64 to double) that were silently truncating.
    set(GCC_CLANG_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Woverloaded-virtual
        -Wconversion
        -Wnull-dereference
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wdouble-promotion
    )

    # Some flags are GCC-only; filter for Clang
    set(CLANG_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Woverloaded-virtual
        -Wconversion
        # Clang folds -Wsign-conversion into -Wconversion; GCC does not. Opt out
        # explicitly so both compilers apply the same policy (see above).
        -Wno-sign-conversion
        -Wnull-dereference
        -Wformat=2
        -Wimplicit-fallthrough
        -Wdouble-promotion
    )

    set(MSVC_WARNINGS
        /W4
        /permissive-
        # Do not apply any of the above to headers reached through <angle
        # brackets>, i.e. the standard library. /w14242 below is on by intent
        # for our code, but MSVC's own <xutility> trips it repeatedly inside
        # std::fill, and with /WX that fails the build over code we do not own
        # and cannot fix. GCC and Clang exempt system headers by default; MSVC
        # has to be told.
        /external:anglebrackets
        /external:W0
        /w14242  # conversion, possible loss of data
        /w14254  # operator conversion, possible loss of data
        /w14263  # function does not override base class virtual
        /w14265  # class has virtual functions but destructor is not virtual
        /w14287  # unsigned/negative constant mismatch
        /w14296  # expression is always true/false
        /w14311  # pointer truncation
        /w14545  # ill-formed comma expression
        /w14546  # function call before comma missing argument list
        /w14547  # operator before comma has no effect
        /w14549  # operator before comma has no effect
        /w14555  # expression has no effect
        /w14619  # pragma warning: nonexistent warning number
        /w14640  # thread-unsafe static member initialization
        /w14826  # conversion is sign-extended
        /w14905  # wide string literal cast to LPSTR
        /w14906  # string literal cast to LPWSTR
        /w14928  # illegal copy-initialization
    )

    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:${GCC_CLANG_WARNINGS}>
        $<$<CXX_COMPILER_ID:Clang>:${CLANG_WARNINGS}>
        $<$<CXX_COMPILER_ID:AppleClang>:${CLANG_WARNINGS}>
        $<$<CXX_COMPILER_ID:MSVC>:${MSVC_WARNINGS}>
    )

    # Off by default so a local build is never blocked by a warning, on in CI so
    # the tree cannot drift back to a build log nobody reads.
    if(HAVOC_WERROR)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/WX>
        )
    endif()
endfunction()
