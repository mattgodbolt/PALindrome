#include "convert_command.hpp"
#include "demod_command.hpp"
#include "info_command.hpp"

#include <functional>
#include <iostream>
#include <print>

#include <lyra/lyra.hpp>

int main(int argc, const char **argv) {
  using palindrome::cli::ConvertCommand;

  bool show_help{};
  std::function<int()> action; // set by whichever subcommand is selected

  // Each command owns its option state; it must outlive cli.parse().
  ConvertCommand encode{ConvertCommand::Direction::Encode};
  ConvertCommand decode{ConvertCommand::Direction::Decode};
  palindrome::cli::InfoCommand info;
  palindrome::cli::DemodCommand demod;

  auto cli = lyra::cli();
  cli.add_argument(lyra::help(show_help));
  encode.add_to(cli, action);
  decode.add_to(cli, action);
  info.add_to(cli, action);
  demod.add_to(cli, action);

  const auto result = cli.parse({argc, argv});
  if (show_help) {
    std::cout << cli << '\n';
    return 0;
  }
  if (!result) {
    std::println(std::cerr, "Error in command line: {}", result.message());
    return 1;
  }
  if (!action) {
    // No subcommand given: show usage rather than doing nothing.
    std::cout << cli << '\n';
    return 1;
  }
  return action();
}
