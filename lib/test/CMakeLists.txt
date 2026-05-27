ensure_catch2()

add_executable(palindrome_test SigmfTest.cpp DemodTest.cpp FirTest.cpp BiquadTest.cpp DcBlockerTest.cpp WavTest.cpp)
target_link_libraries(palindrome_test PRIVATE palindrome Catch2::Catch2WithMain opt::pedantic)

# Point the corpus-loading test at the real committed .sigmf-meta files.
target_compile_definitions(palindrome_test PRIVATE PALINDROME_CORPUS_DIR="${CMAKE_SOURCE_DIR}/corpus")

add_test(NAME "palindrome unit tests" COMMAND palindrome_test)

# DSP micro-benchmarks. A separate executable so the unit-test run stays fast;
# run it by hand (ideally a release build): ./lib/test/palindrome_bench
add_executable(palindrome_bench DemodBench.cpp)
target_link_libraries(palindrome_bench PRIVATE palindrome Catch2::Catch2WithMain opt::pedantic)
