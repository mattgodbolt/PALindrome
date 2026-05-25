#include "palindrome/palindrome.hpp"

#include <filesystem>
#include <iostream>
#include <print>

#include <lyra/lyra.hpp>

int main(int argc, const char **argv) {
  bool show_help{};
  bool decode{};
  std::filesystem::path input;
  std::filesystem::path output;

  const auto cli = lyra::cli() //
      | lyra::help(show_help) //
      | lyra::opt(decode)["-d"]["--decode"]("Decode from PAL rather than encoding to it") //
      | lyra::opt(output, "file")["-o"]["--output"]("Output file (defaults to stdout)") //
      | lyra::arg(input, "input")("Input file to convert (defaults to stdin)");

  if (const auto result = cli.parse({argc, argv}); !result) {
    std::print(std::cerr, "Error in command line: {}\n", result.message());
    return 1;
  }
  if (show_help) {
    std::cout << cli << '\n';
    return 0;
  }

  std::println("{}", palindrome::greeting());
  std::println("{} {} -> {}", decode ? "decode" : "encode", input.empty() ? "<stdin>" : input.string(),
      output.empty() ? "<stdout>" : output.string());
  return 0;
}
