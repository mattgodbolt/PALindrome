# Interface libraries that targets can link against to opt in to:
#   opt::pedantic  - a strict warning set, warnings-as-errors
#   opt::cxx_std   - the project's C++ standard (C++26)
#
# Keeping these as INTERFACE libraries (rather than global flags) means tests
# and third-party code we pull in aren't forced to compile warning-clean.

add_library(opt_pedantic INTERFACE)
add_library(opt::pedantic ALIAS opt_pedantic)

if (MSVC)
    target_compile_options(opt_pedantic INTERFACE "/W4" "/WX")
else ()
    target_compile_options(opt_pedantic INTERFACE
            "-Wall" "-Wextra" "-pedantic" "-Wshadow" "-Wconversion" "-Werror"
            "-Wnull-dereference" "-Wnon-virtual-dtor" "-Wsuggest-override"
            # Mechanises the float/double rule in CLAUDE.md: data must not drift
            # up to double implicitly — widen with an explicit cast or not at all.
            "-Wdouble-promotion")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(opt_pedantic INTERFACE
                "-Wduplicated-cond" "-Wduplicated-branches" "-Wlogical-op")
    endif ()
endif ()

add_library(opt_cxx_std INTERFACE)
add_library(opt::cxx_std ALIAS opt_cxx_std)
target_compile_features(opt_cxx_std INTERFACE cxx_std_26)
