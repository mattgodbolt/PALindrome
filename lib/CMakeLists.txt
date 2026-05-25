if (PALINDROME_TESTS)
    add_subdirectory(test)
endif ()

add_library(palindrome)
add_library(palindrome::palindrome ALIAS palindrome)

target_sources(palindrome PRIVATE palindrome.cpp)

target_sources(palindrome
        PUBLIC
        FILE_SET interface
        TYPE HEADERS
        BASE_DIRS include
        FILES include/palindrome/palindrome.hpp)

target_link_libraries(palindrome PUBLIC opt::cxx_std PRIVATE opt::pedantic)
