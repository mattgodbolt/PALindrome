# Pull in lodepng (single-file PNG encoder/decoder) for image I/O. lodepng ships
# no releases and no CMake of its own, so we fetch the source (pinned by commit)
# and compile lodepng.cpp into a small static library here. Call ensure_lodepng()
# then link against lodepng::lodepng. Goes through palindrome_dependency() so
# PALINDROME_PREFER_SYSTEM_DEPS finds a packaged lodepng if there is one.
#
# Its include dir is marked SYSTEM and it is deliberately *not* linked against
# opt::pedantic, so lodepng's own source isn't held to our -Werror warning set.
macro(ensure_lodepng)
    if (NOT TARGET lodepng::lodepng)
        palindrome_dependency(lodepng
                NAME lodepng
                GITHUB_REPOSITORY lvandeve/lodepng
                GIT_TAG 22561883dd63fd1850f18e1f6adac321e4f609b0
                DOWNLOAD_ONLY YES)
        if (NOT TARGET lodepng::lodepng)
            # CPM downloaded the raw source; lodepng has no CMakeLists, so wrap
            # lodepng.cpp ourselves. (A system find_package that exposes
            # lodepng::lodepng directly would short-circuit this block.)
            add_library(lodepng STATIC "${lodepng_SOURCE_DIR}/lodepng.cpp")
            add_library(lodepng::lodepng ALIAS lodepng)
            target_include_directories(lodepng SYSTEM PUBLIC "${lodepng_SOURCE_DIR}")
            target_link_libraries(lodepng PRIVATE opt::cxx_std)
        endif ()
    endif ()
endmacro()
