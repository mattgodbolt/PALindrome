#include "convert_command.hpp"

#include <iostream>
#include <print>
#include <string_view>

namespace palindrome::cli {

namespace {
struct Names {
  std::string_view verb;
  std::string_view help;
};

Names names_for(ConvertCommand::Direction direction) {
  if (direction == ConvertCommand::Direction::Encode)
    return {"encode", "Encode an input into a PAL signal"};
  return {"decode", "Decode a PAL signal back to an image"};
}
} // namespace

void ConvertCommand::add_to(lyra::cli &cli, std::function<int()> &action) {
  const auto names = names_for(direction_);
  cli.add_argument(lyra::command(
      std::string{names.verb}, [this, &action](const lyra::group &) { action = [this] { return run(); }; })
          .help(std::string{names.help})
          .add_argument(lyra::opt(output_, "file")["-o"]["--output"]("Output file (defaults to stdout)"))
          .add_argument(lyra::arg(input_, "input")("Input file (defaults to stdin)")));
}

int ConvertCommand::run() const {
  // The real PAL encode/decode surface isn't implemented yet; report intent.
  std::println("{}: not yet implemented", names_for(direction_).verb);
  std::println(
      "  {} -> {}", input_.empty() ? "<stdin>" : input_.string(), output_.empty() ? "<stdout>" : output_.string());
  return 0;
}

} // namespace palindrome::cli
