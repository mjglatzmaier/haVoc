function(havoc_enable_sanitizers target)
    if(NOT HAVOC_ENABLE_SANITIZERS)
        return()
    endif()

    # PUBLIC, not PRIVATE. Instrumented code emits calls to the sanitizer
    # runtime, so every binary that links this target has to link that runtime
    # too. With PRIVATE link options the runtime reached the engine executable
    # (which is asked for them directly) but not the test executable (which only
    # links havoc_core), so the sanitizer build failed at link with several
    # hundred undefined references to __asan_*/__ubsan_*. Nothing caught it
    # because the CI job that builds this configuration never ran -- see
    # .github/workflows/ci.yml.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PUBLIC
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            $<$<CONFIG:Debug>:-fno-sanitize-recover=all>
        )
        target_link_options(${target} PUBLIC
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>
        )
    elseif(MSVC)
        target_compile_options(${target} PUBLIC
            $<$<CONFIG:Debug>:/fsanitize=address>
        )
    endif()
endfunction()
