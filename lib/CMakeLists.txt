if (PALINDROME_TESTS)
    add_subdirectory(test)
endif ()

# JSON parsing for the SigMF metadata loader. nlohmann_json marks its includes
# as SYSTEM when consumed this way, so it doesn't trip our -Werror warning set.
palindrome_dependency(nlohmann_json "gh:nlohmann/json@3.11.3")

add_library(palindrome)
add_library(palindrome::palindrome ALIAS palindrome)

target_sources(palindrome PRIVATE sigmf.cpp demod.cpp fir.cpp mixer.cpp biquad.cpp dc_blocker.cpp wav.cpp)

target_sources(palindrome
        PUBLIC
        FILE_SET interface
        TYPE HEADERS
        BASE_DIRS include
        FILES
        include/palindrome/buffer.hpp
        include/palindrome/pipeline.hpp
        include/palindrome/restrict_ptr.hpp
        include/palindrome/sigmf.hpp
        include/palindrome/demod.hpp
        include/palindrome/fir.hpp
        include/palindrome/mixer.hpp
        include/palindrome/biquad.hpp
        include/palindrome/dc_blocker.hpp
        include/palindrome/wav.hpp)

target_link_libraries(palindrome PUBLIC opt::cxx_std nlohmann_json::nlohmann_json PRIVATE opt::pedantic opt::arch)
