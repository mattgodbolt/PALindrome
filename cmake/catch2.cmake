# Pull in Catch2 for unit tests. Call ensure_catch2() from a test subdirectory
# before defining a test executable that links Catch2::Catch2WithMain.
macro(ensure_catch2)
    add_compile_definitions(CATCH_CONFIG_ENABLE_ALL_STRINGMAKERS)
    CPMAddPackage("gh:catchorg/Catch2@3.8.0")
endmacro()
