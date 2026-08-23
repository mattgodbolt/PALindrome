add_executable(palindrome_cli
        main.cpp
        cli_util.cpp
        info_command.cpp
        demod_command.cpp
        profile_args.cpp
        render_command.cpp
        sync_command.cpp
        levels_command.cpp
        convert_command.cpp)

# The CMake target must be unique, but we want the binary on disk to just be
# `palindrome`.
set_target_properties(palindrome_cli PROPERTIES OUTPUT_NAME palindrome)

# nlohmann_json arrives transitively through palindrome, but profile_args.cpp
# names it directly, so link it directly too.
target_link_libraries(palindrome_cli PRIVATE palindrome lyra nlohmann_json::nlohmann_json opt::pedantic)
