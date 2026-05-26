#include "palindrome/sigmf.hpp"

#include <filesystem>
#include <format>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_view_literals;
namespace sigmf = palindrome::sigmf;

namespace {

// A compact but complete document exercising every modelled field.
constexpr auto kSample = R"({
  "global": {
    "core:datatype": "ri16_le",
    "core:version": "1.2.0",
    "core:sample_rate": 32000000,
    "core:description": "test clip",
    "core:author": "Tester",
    "core:hw": "RX888 mk2",
    "core:extensions": [
      { "name": "rx888", "version": "0.0.1", "optional": true }
    ],
    "rx888:vision_if_hz": 3568848
  },
  "captures": [
    { "core:sample_start": 0, "core:frequency": 590200000, "core:datetime": "2026-05-26T00:26:31.842396Z" }
  ],
  "annotations": [
    { "core:sample_start": 0, "core:sample_count": 15360000, "core:label": "PAL composite at IF" }
  ]
})"sv;

} // namespace

TEST_CASE("DataType::parse decodes the SigMF grammar") {
  SECTION("real signed 16-bit little-endian") {
    const auto dt = sigmf::DataType::parse("ri16_le");
    CHECK_FALSE(dt.complex);
    CHECK(dt.format == sigmf::DataType::Format::SignedInt);
    CHECK(dt.bits == 16u);
    CHECK(dt.endianness == sigmf::DataType::Endianness::Little);
    CHECK(dt.bytes_per_sample() == 2u);
  }

  SECTION("complex float doubles the per-sample size") {
    const auto dt = sigmf::DataType::parse("cf32_le");
    CHECK(dt.complex);
    CHECK(dt.format == sigmf::DataType::Format::Float);
    CHECK(dt.bits == 32u);
    CHECK(dt.bytes_per_sample() == 8u);
  }

  SECTION("big-endian suffix") {
    CHECK(sigmf::DataType::parse("ru32_be").endianness == sigmf::DataType::Endianness::Big);
  }

  SECTION("8-bit takes no endianness suffix") {
    const auto dt = sigmf::DataType::parse("cu8");
    CHECK(dt.bits == 8u);
    CHECK(dt.bytes_per_sample() == 2u);
    // ...and the grammar forbids one on 8-bit types.
    CHECK_THROWS_AS(sigmf::DataType::parse("cu8_le"), sigmf::ParseError);
  }

  SECTION("a width > 8 bits requires an endianness suffix") {
    CHECK_THROWS_AS(sigmf::DataType::parse("ri16"), sigmf::ParseError);
  }

  SECTION("garbage is rejected") {
    CHECK_THROWS_AS(sigmf::DataType::parse(""), sigmf::ParseError);
    CHECK_THROWS_AS(sigmf::DataType::parse("xi16_le"), sigmf::ParseError);
    CHECK_THROWS_AS(sigmf::DataType::parse("ri17_le"), sigmf::ParseError);
    CHECK_THROWS_AS(sigmf::DataType::parse("ri16_xx"), sigmf::ParseError);
  }
}

TEST_CASE("DataType is comparable and formattable") {
  CHECK(sigmf::DataType::parse("ri16_le") == sigmf::DataType::parse("ri16_le"));
  CHECK(sigmf::DataType::parse("ri16_le") != sigmf::DataType::parse("ri16_be"));

  CHECK(std::format("{}", sigmf::DataType::parse("ri16_le")) == "real int16 LE");
  CHECK(std::format("{}", sigmf::DataType::parse("cf32_be")) == "complex float32 BE");
  CHECK(std::format("{}", sigmf::DataType::parse("cu8")) == "complex uint8");
  // The format spec (here a width) is honoured via the inherited string formatter.
  CHECK(std::format("{:>15}", sigmf::DataType::parse("cu8")) == "  complex uint8");
}

TEST_CASE("parse reads the global object") {
  const auto meta = sigmf::parse(kSample);
  CHECK(meta.global.datatype == "ri16_le");
  CHECK(meta.global.parsed_datatype.bits == 16u);
  CHECK(meta.global.version == "1.2.0");
  REQUIRE(meta.global.sample_rate.has_value());
  CHECK(meta.global.sample_rate.value() == 32000000.0);
  CHECK(meta.global.description == "test clip");
  CHECK(meta.global.author == "Tester");
  CHECK(meta.global.hardware == "RX888 mk2");
  CHECK_FALSE(meta.global.num_channels.has_value());
}

TEST_CASE("parse reads extensions") {
  const auto meta = sigmf::parse(kSample);
  REQUIRE(meta.global.extensions.size() == 1);
  CHECK(meta.global.extensions.front().name == "rx888");
  CHECK(meta.global.extensions.front().version == "0.0.1");
  CHECK(meta.global.extensions.front().optional);
}

TEST_CASE("parse reads captures and annotations") {
  const auto meta = sigmf::parse(kSample);

  REQUIRE(meta.captures.size() == 1);
  const auto &cap = meta.captures.front();
  CHECK(cap.sample_start == 0u);
  REQUIRE(cap.frequency.has_value());
  CHECK(cap.frequency.value() == 590200000.0);
  CHECK(cap.datetime == "2026-05-26T00:26:31.842396Z");

  REQUIRE(meta.annotations.size() == 1);
  const auto &ann = meta.annotations.front();
  CHECK(ann.sample_start == 0u);
  REQUIRE(ann.sample_count.has_value());
  CHECK(ann.sample_count.value() == 15360000ULL);
  CHECK(ann.label == "PAL composite at IF");
}

TEST_CASE("field() reaches extension-namespaced keys") {
  const auto meta = sigmf::parse(kSample);
  CHECK(meta.field<std::int64_t>("rx888:vision_if_hz") == 3568848);
  CHECK_FALSE(meta.field<std::int64_t>("rx888:not_present").has_value());
  // A present-but-wrong-type field surfaces as ParseError, not a raw json throw.
  CHECK_THROWS_AS(meta.field<std::int64_t>("core:description"), sigmf::ParseError);
}

TEST_CASE("parse is lenient about absent captures and annotations") {
  const auto meta = sigmf::parse(R"({"global": {"core:datatype": "ri16_le", "core:version": "1.2.0"}})");
  CHECK(meta.captures.empty());
  CHECK(meta.annotations.empty());
}

TEST_CASE("parse rejects structurally wrong fields") {
  CHECK_THROWS_AS(sigmf::parse(R"({"global": []})"), sigmf::ParseError);
  CHECK_THROWS_AS(sigmf::parse(R"({"global": {"core:datatype": "ri16_le", "core:version": "1.2.0"}, "captures": {}})"),
      sigmf::ParseError);
  // Wrong scalar type for a modelled field.
  CHECK_THROWS_AS(
      sigmf::parse(R"({"global": {"core:datatype": "ri16_le", "core:version": "1.2.0", "core:sample_rate": "fast"}})"),
      sigmf::ParseError);
}

TEST_CASE("parse rejects malformed documents") {
  CHECK_THROWS_AS(sigmf::parse("not json"), sigmf::ParseError);
  CHECK_THROWS_AS(sigmf::parse("[]"), sigmf::ParseError);
  CHECK_THROWS_AS(sigmf::parse(R"({"global": {"core:version": "1.2.0"}})"), sigmf::ParseError);
  CHECK_THROWS_AS(sigmf::parse(R"({"global": {"core:datatype": "ri16_le"}})"), sigmf::ParseError);
  CHECK_THROWS_AS(sigmf::parse(R"({"global": {"core:datatype": 5, "core:version": "1.2.0"}})"), sigmf::ParseError);
}

TEST_CASE("data_path_for swaps the extension") {
  CHECK(sigmf::data_path_for("corpus/wb3.sigmf-meta") == std::filesystem::path{"corpus/wb3.sigmf-data"});
}

TEST_CASE("load reports a missing file as a ParseError naming the path") {
  CHECK_THROWS_AS(sigmf::load("does/not/exist.sigmf-meta"), sigmf::ParseError);
}

#ifdef PALINDROME_CORPUS_DIR
TEST_CASE("the committed corpus metadata loads and validates") {
  const std::filesystem::path corpus{PALINDROME_CORPUS_DIR};
  for (const auto *name: {"alex_kidd.sigmf-meta", "alex_kidd_title.sigmf-meta", "wb3.sigmf-meta"}) {
    CAPTURE(name);
    const auto meta = sigmf::load(corpus / name);

    // The format the capture tool writes: real int16 LE at 32 MSps.
    CHECK(meta.global.datatype == "ri16_le");
    CHECK(meta.global.parsed_datatype.bytes_per_sample() == 2u);
    REQUIRE(meta.global.sample_rate.has_value());
    CHECK(meta.global.sample_rate.value() == 32000000.0);

    // The rx888 extension is declared, and its carrier fields are reachable.
    REQUIRE(meta.global.extensions.size() == 1);
    CHECK(meta.global.extensions.front().name == "rx888");
    CHECK(meta.field<std::int64_t>("rx888:vision_if_hz").value() > 3000000);

    // One capture, one annotation covering a whole number of frames.
    REQUIRE(meta.captures.size() == 1);
    CHECK(meta.captures.front().frequency.value() == 590200000.0);
    REQUIRE(meta.annotations.size() == 1);
    CHECK(meta.annotations.front().sample_count.value() == 15360000ULL);
  }
}
#endif
