#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// A small, read-focused model of the SigMF metadata format (the JSON in a
// `.sigmf-meta` file). See https://github.com/sigmf/SigMF. We model the
// SigMF-required and commonly-used core fields as strong types; anything else
// (notably extension-namespaced keys such as `rx888:*`) stays reachable via
// Metadata::raw / Metadata::field.
namespace palindrome::sigmf {

// Thrown when a document isn't valid SigMF: malformed JSON, a missing required
// field, or a field of the wrong type. The message says what went wrong.
class ParseError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// The decoded `core:datatype` string (e.g. "ri16_le"). SigMF datatypes match
// the grammar [c|r][f|i|u]<bits>[_le|_be]: a real or complex sample built from
// one (real) or two (complex) interleaved components, each a float / signed /
// unsigned integer of the given width, in the given byte order.
struct DataType {
  enum class Format { Float, SignedInt, UnsignedInt };
  enum class Endianness { Little, Big };

  bool complex{};
  Format format{};
  unsigned bits{};
  Endianness endianness{Endianness::Little};

  // Bytes occupied on disk by a single stored sample: the component width,
  // doubled for complex (interleaved I/Q) data. Zero only for a
  // default-constructed (un-parsed) DataType; any value from parse() is >= 1.
  [[nodiscard]] std::size_t bytes_per_sample() const { return (bits / 8u) * (complex ? 2u : 1u); }

  // Decode a SigMF datatype string. Throws ParseError if it doesn't match the
  // SigMF grammar.
  [[nodiscard]] static DataType parse(std::string_view spec);

  auto operator<=>(const DataType &) const = default;
};

// A declared SigMF extension namespace: one entry of `core:extensions`.
struct Extension {
  std::string name;
  std::string version;
  bool optional{true};

  auto operator<=>(const Extension &) const = default;
};

// The `global` object: properties of the recording as a whole. Only the
// required and frequently-used core fields are modelled here; reach anything
// else through Metadata::field.
struct Global {
  std::string datatype; // core:datatype, verbatim
  DataType parsed_datatype; // core:datatype, decoded
  std::string version; // core:version
  std::optional<double> sample_rate;
  std::optional<std::string> description;
  std::optional<std::string> author;
  std::optional<std::string> recorder;
  std::optional<std::string> hardware; // core:hw
  std::optional<std::uint32_t> num_channels;
  std::vector<Extension> extensions;
};

// One entry of the `captures` array: a run of samples sharing the same RF/clock
// configuration, starting at sample_start.
struct Capture {
  std::uint64_t sample_start{};
  std::optional<double> frequency; // core:frequency, Hz
  std::optional<std::string> datetime;
};

// One entry of the `annotations` array: a labelled region of samples. If
// sample_count is absent the region runs to the end of the capture.
struct Annotation {
  std::uint64_t sample_start{};
  std::optional<std::uint64_t> sample_count;
  std::optional<std::string> label;
  std::optional<std::string> description;
  std::optional<double> freq_lower_edge; // Hz
  std::optional<double> freq_upper_edge; // Hz
};

// A parsed SigMF metadata document.
struct Metadata {
  Global global;
  std::vector<Capture> captures;
  std::vector<Annotation> annotations;

  // The full underlying JSON, for reaching fields not modelled above. Prefer
  // field() for typed access to global keys.
  nlohmann::json raw;

  // Fetch a global field by its fully-qualified key (e.g. "rx888:vision_if_hz")
  // converted to T. Returns nullopt if the key is absent or JSON null; throws
  // ParseError if present but not convertible to T. Note conversions are
  // otherwise nlohmann's: a numeric value outside T's range is truncated /
  // wrapped, not rejected.
  template<typename T>
  [[nodiscard]] std::optional<T> field(std::string_view key) const {
    const auto &g = raw.at("global");
    const auto it = g.find(std::string{key});
    if (it == g.end() || it->is_null())
      return std::nullopt;
    try {
      return it->template get<T>();
    }
    catch (const nlohmann::json::exception &e) {
      throw ParseError{std::format("field '{}' has the wrong type: {}", key, e.what())};
    }
  }
};

// Parse SigMF metadata from an in-memory JSON string.
[[nodiscard]] Metadata parse(std::string_view json);

// Load and parse a `.sigmf-meta` file from disk.
[[nodiscard]] Metadata load(const std::filesystem::path &meta_path);

// Given a `.sigmf-meta` path, return the path to its paired `.sigmf-data` file
// (same stem, `.sigmf-data` extension).
[[nodiscard]] std::filesystem::path data_path_for(const std::filesystem::path &meta_path);

} // namespace palindrome::sigmf

// Format a DataType in human-readable form, e.g. "real int16 LE", "complex
// float32", "complex uint8" (no byte-order suffix for 8-bit). Honours the
// usual string format spec (fill/align/width).
template<>
struct std::formatter<palindrome::sigmf::DataType> : std::formatter<std::string> {
  auto format(const palindrome::sigmf::DataType &dt, std::format_context &ctx) const {
    using Format = palindrome::sigmf::DataType::Format;
    using Endianness = palindrome::sigmf::DataType::Endianness;
    const auto *type = dt.format == Format::Float ? "float" : dt.format == Format::SignedInt ? "int" : "uint";
    const auto *order = dt.bits <= 8 ? "" : dt.endianness == Endianness::Little ? " LE" : " BE";
    return std::formatter<std::string>::format(
        std::format("{} {}{}{}", dt.complex ? "complex" : "real", type, dt.bits, order), ctx);
  }
};
