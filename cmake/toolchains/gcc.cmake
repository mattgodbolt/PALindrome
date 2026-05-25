# Pin a specific GCC so devs and CI build with the same compiler.
set(CMAKE_C_COMPILER gcc-15 CACHE STRING "C compiler")
set(CMAKE_CXX_COMPILER g++-15 CACHE STRING "C++ compiler")
