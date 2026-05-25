# Fetch a dependency, preferring a system-installed copy if the user asked for
# one (PALINDROME_PREFER_SYSTEM_DEPS), otherwise pulling it in via CPM.
#
#   palindrome_dependency(Catch2 "gh:catchorg/Catch2@3.8.0")
#
# NAME        the find_package() / CPM package name.
# CPM_PACKAGE the CPM package spec to fetch when not found on the system.
macro(palindrome_dependency NAME CPM_PACKAGE)
    set(PALINDROME_HAS_${NAME} OFF)

    if (PALINDROME_PREFER_SYSTEM_DEPS)
        find_package(${NAME} QUIET)
        if (${NAME}_FOUND)
            set(PALINDROME_HAS_${NAME} ON)
        endif ()
    endif ()

    if (NOT PALINDROME_HAS_${NAME})
        CPMAddPackage(${CPM_PACKAGE})
    endif ()
endmacro()
