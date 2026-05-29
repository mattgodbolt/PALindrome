#include "convert_command.hpp"
#include "demod_command.hpp"
#include "info_command.hpp"
#include "render_command.hpp"

#include <exception>
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
  palindrome::cli::RenderCommand render;

  auto cli = lyra::cli();
  cli.add_argument(lyra::help(show_help));
  encode.add_to(cli, action);
  decode.add_to(cli, action);
  info.add_to(cli, action);
  demod.add_to(cli, action);
  render.add_to(cli, action);

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
  // Last-resort handler for anything the subcommands didn't catch (e.g. a bad
  // --cutoff that trips AmEnvelope's std::invalid_argument). Commands still
  // print their own prefixed diagnostics for the cases they know about; this
  // just stops an escaped exception from terminating without a useful message.
  try {
    return action();
  }
  catch (const std::exception &e) {
    std::println(std::cerr, "palindrome: {}", e.what());
    return 1;
  }
  catch (...) {
    std::println(std::cerr, "palindrome: unknown error");
    return 1;
  }
}
