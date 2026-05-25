ensure_catch2()

add_executable(palindrome_test PalindromeTest.cpp)
target_link_libraries(palindrome_test PRIVATE palindrome Catch2::Catch2WithMain opt::pedantic)

add_test(NAME "palindrome unit tests" COMMAND palindrome_test)
