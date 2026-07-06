if (PALINDROME_TESTS)
    add_subdirectory(test)
endif ()

# JSON parsing for the SigMF metadata loader. CPM marks fetched includes as
# SYSTEM, so nlohmann_json's headers don't trip our -Werror warning set.
CPMAddPackage("gh:nlohmann/json@3.11.3")

# PNG output for rendered frames (single-file encoder, compiled SYSTEM-clean).
ensure_lodepng()

add_library(palindrome)
add_library(palindrome::palindrome ALIAS palindrome)

target_sources(palindrome PRIVATE sigmf.cpp demod.cpp fft.cpp fir.cpp mixer.cpp biquad.cpp dc_blocker.cpp wav.cpp image.cpp
        gaussian.cpp agc.cpp sync_separator.cpp horizontal_sweep.cpp vertical_sync.cpp chroma_decoder.cpp screen.cpp
        splat.cpp decoder.cpp)

target_sources(palindrome
        PUBLIC
        FILE_SET interface
        TYPE HEADERS
        BASE_DIRS include
        FILES
        include/palindrome/buffer.hpp
        include/palindrome/cmul.hpp
        include/palindrome/phase.hpp
        include/palindrome/phasor.hpp
        include/palindrome/pow01.hpp
        include/palindrome/gaussian.hpp
        include/palindrome/pipeline_run.hpp
        include/palindrome/restrict_ptr.hpp
        include/palindrome/sigmf.hpp
        include/palindrome/demod.hpp
        include/palindrome/fft.hpp
        include/palindrome/fir.hpp
        include/palindrome/mixer.hpp
        include/palindrome/biquad.hpp
        include/palindrome/dc_blocker.hpp
        include/palindrome/wav.hpp
        include/palindrome/image.hpp
        include/palindrome/video_types.hpp
        include/palindrome/agc.hpp
        include/palindrome/sync_separator.hpp
        include/palindrome/horizontal_sweep.hpp
        include/palindrome/vertical_sync.hpp
        include/palindrome/chroma_decoder.hpp
        include/palindrome/screen.hpp
        include/palindrome/decoder.hpp)

target_link_libraries(palindrome PUBLIC opt::cxx_std nlohmann_json::nlohmann_json STDEXEC::stdexec PRIVATE opt::pedantic opt::arch lodepng::lodepng)
