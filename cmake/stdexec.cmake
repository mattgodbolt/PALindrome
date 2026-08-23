# Pull in NVIDIA stdexec: the std::execution (P2300) reference impl, backing
# the threaded stage pipeline in lib (palindrome/pipeline_run.hpp). SYSTEM so
# its headers aren't held to our -Werror set. Linked PUBLIC by lib, so the CLI
# inherits it.
#
# stdexec's top-level CMakeLists bootstraps itself with two *unchecked*
# file(DOWNLOAD)s from raw.githubusercontent.com: RAPIDS.cmake and execution.bs
# (parsed for its version number). A failed download leaves an empty file, which
# passes the if(NOT EXISTS ...) guard, so one rate-limited fetch poisons the
# build dir and flakes CI (issue #72). Pre-seed both files, with status and
# non-empty checks plus retries, into the binary dir stdexec will configure in,
# so its own guards find good copies and it never downloads anything itself.

function(palindrome_seed_download url dest)
    if (EXISTS "${dest}")
        file(SIZE "${dest}" existing_size)
        if (existing_size GREATER 0)
            return()
        endif ()
    endif ()
    get_filename_component(dest_dir "${dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${dest_dir}")
    foreach (attempt RANGE 1 3)
        file(DOWNLOAD "${url}" "${dest}" STATUS status TIMEOUT 60)
        list(GET status 0 status_code)
        if (status_code EQUAL 0)
            file(SIZE "${dest}" size)
            if (size GREATER 0)
                return()
            endif ()
        endif ()
        # Never leave a bad file behind: an empty one would satisfy the
        # if(NOT EXISTS) guards and poison every subsequent configure.
        file(REMOVE "${dest}")
        message(STATUS "Download of ${url} failed (attempt ${attempt}: ${status}); retrying")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
    endforeach ()
    message(FATAL_ERROR "Could not download ${url} after 3 attempts")
endfunction()

macro(ensure_stdexec)
    if (DEFINED FETCHCONTENT_BASE_DIR)
        set(stdexec_binary_dir "${FETCHCONTENT_BASE_DIR}/stdexec-build")
    else ()
        set(stdexec_binary_dir "${CMAKE_BINARY_DIR}/_deps/stdexec-build")
    endif ()
    palindrome_seed_download(
            "https://raw.githubusercontent.com/rapidsai/rapids-cmake/branch-24.02/RAPIDS.cmake"
            "${stdexec_binary_dir}/RAPIDS.cmake")
    palindrome_seed_download(
            "https://raw.githubusercontent.com/cplusplus/sender-receiver/main/execution.bs"
            "${stdexec_binary_dir}/execution.bs")
    CPMAddPackage(
            NAME stdexec
            GITHUB_REPOSITORY NVIDIA/stdexec
            GIT_TAG 02d671da624daafc63dc42f60bfba40f97161400
            OPTIONS "STDEXEC_BUILD_TESTS OFF" "STDEXEC_BUILD_EXAMPLES OFF"
            SYSTEM YES)
endmacro()
