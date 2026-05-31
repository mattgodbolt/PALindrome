cmake_minimum_required(VERSION 3.30)

project(palindrome VERSION 0.1 LANGUAGES CXX)

option(PALINDROME_TESTS "Build and run tests" ON)
option(PALINDROME_CLI "Build the palindrome command-line tool" ON)

set(CMAKE_COLOR_DIAGNOSTICS ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
# CPM is the only dependency mechanism we use. To prefer system-installed
# packages (find_package first, fall back to fetch), configure with
# -DCPM_USE_LOCAL_PACKAGES=ON (or set the env var) -- it's CPM's own switch.
include(CPM)
include(catch2)
include(lodepng)
include(pedantic)
include(arch)

# Use the faster lld linker when the compiler can find it.
try_compile(CAN_USE_LLD SOURCE_FROM_CONTENT "test.cpp" "int main() { }" LINK_OPTIONS "-fuse-ld=lld")
if (CAN_USE_LLD)
    add_link_options("-fuse-ld=lld")
endif ()

# ccache speeds up rebuilds if it's installed; harmless otherwise.
find_program(CCACHE ccache)
if (CCACHE)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE}")
endif ()

# Link-time optimisation for optimised builds only (it slows debug builds down
# for no benefit).
if (NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif ()
if (NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
endif ()

if (PALINDROME_TESTS)
    enable_testing()
endif ()

add_subdirectory(lib)

if (PALINDROME_CLI)
    CPMAddPackage("gh:bfgroup/Lyra@1.7.0#5a4ef1abaf7139ef00204992a2627127ad394499")
    # NVIDIA stdexec: the std::execution (P2300) reference impl, for the threaded
    # render pipeline. SYSTEM so its headers aren't held to our -Werror set.
    CPMAddPackage(
            NAME stdexec
            GITHUB_REPOSITORY NVIDIA/stdexec
            GIT_TAG 02d671da624daafc63dc42f60bfba40f97161400
            OPTIONS "STDEXEC_BUILD_TESTS OFF" "STDEXEC_BUILD_EXAMPLES OFF"
            SYSTEM YES)
    add_subdirectory(cli)
endif ()
