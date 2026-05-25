cmake_minimum_required(VERSION 3.30)

project(palindrome VERSION 0.1 LANGUAGES CXX)

option(PALINDROME_TESTS "Build and run tests" ON)
option(PALINDROME_CLI "Build the palindrome command-line tool" ON)
option(PALINDROME_PREFER_SYSTEM_DEPS "Prefer system-installed dependencies over fetching them" OFF)

set(CMAKE_COLOR_DIAGNOSTICS ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CPM)
include(dependency)
include(catch2)
include(pedantic)

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
    add_subdirectory(cli)
endif ()
