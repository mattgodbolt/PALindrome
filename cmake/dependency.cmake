# Fetch a dependency, preferring a system-installed copy if the user asked for
# one (PALINDROME_PREFER_SYSTEM_DEPS), otherwise pulling it in via CPM. Any
# trailing args are forwarded to CPMAddPackage, so callers can use either the
# shorthand spec or the full multi-arg form (e.g. for DOWNLOAD_ONLY).
#
#   palindrome_dependency(Catch2 "gh:catchorg/Catch2@3.8.0")
#   palindrome_dependency(lodepng NAME lodepng GITHUB_REPOSITORY lvandeve/lodepng
#                                 GIT_TAG <sha> DOWNLOAD_ONLY YES)
#
# NAME the find_package() name; everything after is the CPM spec.
macro(palindrome_dependency NAME)
    set(PALINDROME_HAS_${NAME} OFF)

    if (PALINDROME_PREFER_SYSTEM_DEPS)
        find_package(${NAME} QUIET)
        if (${NAME}_FOUND)
            set(PALINDROME_HAS_${NAME} ON)
        endif ()
    endif ()

    if (NOT PALINDROME_HAS_${NAME})
        CPMAddPackage(${ARGN})
    endif ()
endmacro()
