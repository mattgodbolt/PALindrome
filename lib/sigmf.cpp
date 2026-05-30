#include "palindrome/sigmf.hpp"

#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace palindrome::sigmf {

namespace {

// Read an optional T at `key` in object `j`: nullopt if missing or JSON null,
// the value otherwise. Throws ParseError on a type mismatch.
template<typename T>
std::optional<T> optional_field(const nlohmann::json &j, const char *key) {
  const auto it = j.find(key);
  if (it == j.end() || it->is_null())
    return std::nullopt;
  try {
    return it->template get<T>();
  }
  catch (const nlohmann::json::exception &e) {
    throw ParseError{std::format("field '{}' has the wrong type: {}", key, e.what())};
  }
}

// Read a required T at `key`, throwing ParseError if it's absent or null.
template<typename T>
T required_field(const nlohmann::json &j, const char *key) {
  if (auto value = optional_field<T>(j, key))
    return std::move(*value);
  throw ParseError{std::format("missing required field '{}'", key)};
}

} // namespace

DataType DataType::parse(std::string_view spec) {
  const auto fail = [&] { throw ParseError{std::format("invalid core:datatype: '{}'", spec)}; };

  DataType dt;
  std::string_view s = spec;
  if (s.empty())
    fail();
  if (s.front() == 'c')
    dt.complex = true;
  else if (s.front() == 'r')
    dt.complex = false;
  else
    fail();
  s.remove_prefix(1);

  // Format + width, matching the SigMF grammar. Ordered so no token is a prefix
  // of an earlier one (so starts_with picks the right entry).
  struct Entry {
    std::string_view token;
    Format format;
    unsigned bits;
  };
  static constexpr std::array entries{
      Entry{"f64", Format::Float, 64u},
      Entry{"f32", Format::Float, 32u},
      Entry{"i32", Format::SignedInt, 32u},
      Entry{"i16", Format::SignedInt, 16u},
      Entry{"u32", Format::UnsignedInt, 32u},
      Entry{"u16", Format::UnsignedInt, 16u},
      Entry{"i8", Format::SignedInt, 8u},
      Entry{"u8", Format::UnsignedInt, 8u},
  };
  bool matched = false;
  for (const auto &e: entries) {
    if (s.starts_with(e.token)) {
      dt.format = e.format;
      dt.bits = e.bits;
      s.remove_prefix(e.token.size());
      matched = true;
      break;
    }
  }
  if (!matched)
    fail();

  // Endianness suffix: the SigMF grammar requires it for widths > 8 bits and
  // forbids it for 8-bit types (a single byte has no byte order).
  if (dt.bits <= 8) {
    if (!s.empty())
      fail();
    dt.endianness = Endianness::Little; // irrelevant; pick a deterministic value
  }
  else if (s == "_le") {
    dt.endianness = Endianness::Little;
  }
  else if (s == "_be") {
    dt.endianness = Endianness::Big;
  }
  else {
    fail();
  }

  return dt;
}

Metadata parse(std::string_view json) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(json);
  }
  catch (const nlohmann::json::parse_error &e) {
    throw ParseError{std::format("invalid JSON: {}", e.what())};
  }
  if (!doc.is_object() || !doc.contains("global"))
    throw ParseError{"not a SigMF document: missing 'global' object"};

  const auto &g = doc.at("global");
  if (!g.is_object())
    throw ParseError{"'global' is not an object"};

  Metadata meta;
  meta.global.datatype = required_field<std::string>(g, "core:datatype");
  meta.global.parsed_datatype = DataType::parse(meta.global.datatype);
  meta.global.version = required_field<std::string>(g, "core:version");
  meta.global.sample_rate = optional_field<double>(g, "core:sample_rate");
  meta.global.description = optional_field<std::string>(g, "core:description");
  meta.global.author = optional_field<std::string>(g, "core:author");
  meta.global.recorder = optional_field<std::string>(g, "core:recorder");
  meta.global.hardware = optional_field<std::string>(g, "core:hw");
  meta.global.num_channels = optional_field<std::uint32_t>(g, "core:num_channels");

  if (const auto exts = g.find("core:extensions"); exts != g.end() && !exts->is_null()) {
    if (!exts->is_array())
      throw ParseError{"'core:extensions' is not an array"};
    for (const auto &e: *exts)
      meta.global.extensions.push_back(Extension{
          .name = required_field<std::string>(e, "name"),
          .version = required_field<std::string>(e, "version"),
          .optional = required_field<bool>(e, "optional"),
      });
  }

  // captures / annotations are required arrays per the spec, but we read
  // leniently: absent means empty.
  if (const auto caps = doc.find("captures"); caps != doc.end() && !caps->is_null()) {
    if (!caps->is_array())
      throw ParseError{"'captures' is not an array"};
    for (const auto &c: *caps)
      meta.captures.push_back(Capture{
          .sample_start = required_field<std::uint64_t>(c, "core:sample_start"),
          .frequency = optional_field<double>(c, "core:frequency"),
          .datetime = optional_field<std::string>(c, "core:datetime"),
      });
  }

  if (const auto anns = doc.find("annotations"); anns != doc.end() && !anns->is_null()) {
    if (!anns->is_array())
      throw ParseError{"'annotations' is not an array"};
    for (const auto &a: *anns)
      meta.annotations.push_back(Annotation{
          .sample_start = required_field<std::uint64_t>(a, "core:sample_start"),
          .sample_count = optional_field<std::uint64_t>(a, "core:sample_count"),
          .label = optional_field<std::string>(a, "core:label"),
          .description = optional_field<std::string>(a, "core:description"),
          .freq_lower_edge = optional_field<double>(a, "core:freq_lower_edge"),
          .freq_upper_edge = optional_field<double>(a, "core:freq_upper_edge"),
      });
  }

  meta.raw = std::move(doc);
  return meta;
}

Metadata load(const std::filesystem::path &meta_path) {
  std::ifstream in{meta_path, std::ios::binary};
  if (!in)
    throw ParseError{std::format("could not open SigMF metadata file: {}", meta_path.string())};
  const std::string contents{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  try {
    return parse(contents);
  }
  catch (const ParseError &e) {
    throw ParseError{std::format("{}: {}", meta_path.string(), e.what())};
  }
}

std::filesystem::path data_path_for(const std::filesystem::path &meta_path) {
  auto data = meta_path;
  data.replace_extension(".sigmf-data");
  return data;
}

} // namespace palindrome::sigmf
