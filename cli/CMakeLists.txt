add_executable(palindrome_cli
        main.cpp
        cli_util.cpp
        info_command.cpp
        demod_command.cpp
        render_command.cpp
        convert_command.cpp)

# The CMake target must be unique, but we want the binary on disk to just be
# `palindrome`.
set_target_properties(palindrome_cli PROPERTIES OUTPUT_NAME palindrome)

target_link_libraries(palindrome_cli PRIVATE palindrome lyra opt::pedantic)
