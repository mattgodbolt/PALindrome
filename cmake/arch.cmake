# An interface library targets can link against to opt in to architecture-tuned
# codegen:
#   opt::arch  - passes -march so the auto-vectoriser can use the host's full
#                instruction set (AVX2, FMA, ...).
#
# PALINDROME_ARCH defaults to "native" (tune for the build machine). Set it to a
# portable feature level such as x86-64-v3 for distributable binaries, or empty
# to disable. Skipped for MSVC and Emscripten, which don't take -march.
set(PALINDROME_ARCH "native" CACHE STRING "Value passed to -march (empty to disable)")

add_library(opt_arch INTERFACE)
add_library(opt::arch ALIAS opt_arch)
if (PALINDROME_ARCH AND NOT MSVC AND NOT EMSCRIPTEN)
    target_compile_options(opt_arch INTERFACE "-march=${PALINDROME_ARCH}")
endif ()
