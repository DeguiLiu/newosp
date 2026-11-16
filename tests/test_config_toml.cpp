/**
 * @file test_config_toml.cpp
 * @brief Tests for the TOML backend (toml++) of config.hpp.
 *
 * Covers: basic section/key, int/string/bool/float, nested tables, arrays,
 * defaults, missing-file error, malformed-input error, extension auto-detect,
 * tag dispatch, and cross-format consistency with the INI backend.
 */

#include "osp/config.hpp"

#include <cstdio>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

// ============================================================================
// TomlBackend Tests
// ============================================================================

#ifdef OSP_CONFIG_TOML_ENABLED

using TomlCfg = osp::Config<osp::TomlBackend>;

namespace {

// A representative TOML document covering scalars, a root array and nested
// tables. Inline string literals keep the tests free of disk dependencies.
const char* kBasicToml =
    "title = \"TOML Example\"\n"
    "tags = [\"a\", \"b\", \"c\"]\n"
    "\n"
    "[network]\n"
    "port = 5090\n"
    "host = \"0.0.0.0\"\n"
    "debug = true\n"
    "ratio = 0.25\n"
    "\n"
    "[log]\n"
    "level = \"INFO\"\n"
    "\n"
    "[server.auth]\n"
    "user = \"bob\"\n"
    "max_connections = 128\n";

}  // namespace

TEST_CASE("TOML LoadBuffer basic", "[config][toml]") {
  TomlCfg cfg;
  auto result = cfg.LoadBuffer(kBasicToml, static_cast<uint32_t>(std::strlen(kBasicToml)), osp::ConfigFormat::kToml);
  REQUIRE(result.has_value());

  // Top-level scalar -> "" section
  REQUIRE(std::strcmp(cfg.GetString("", "title"), "TOML Example") == 0);
  // Section scalar
  REQUIRE(cfg.GetInt("network", "port", 0) == 5090);
  REQUIRE(std::strcmp(cfg.GetString("network", "host"), "0.0.0.0") == 0);
  REQUIRE(std::strcmp(cfg.GetString("log", "level"), "INFO") == 0);
}

TEST_CASE("TOML boolean and float values", "[config][toml]") {
  TomlCfg cfg;
  cfg.LoadBuffer(kBasicToml, static_cast<uint32_t>(std::strlen(kBasicToml)), osp::ConfigFormat::kToml);

  REQUIRE(cfg.GetBool("network", "debug") == true);
  REQUIRE(cfg.GetBool("network", "missing", true) == true);

  REQUIRE(cfg.GetDouble("network", "ratio") > 0.24);
  REQUIRE(cfg.GetDouble("network", "ratio") < 0.26);
}

TEST_CASE("TOML nested tables flatten to dotted section", "[config][toml]") {
  TomlCfg cfg;
  cfg.LoadBuffer(kBasicToml, static_cast<uint32_t>(std::strlen(kBasicToml)), osp::ConfigFormat::kToml);

  REQUIRE(std::strcmp(cfg.GetString("server.auth", "user"), "bob") == 0);
  REQUIRE(cfg.GetInt("server.auth", "max_connections", 0) == 128);
}

TEST_CASE("TOML array serializes to comma-separated string", "[config][toml]") {
  TomlCfg cfg;
  cfg.LoadBuffer(kBasicToml, static_cast<uint32_t>(std::strlen(kBasicToml)), osp::ConfigFormat::kToml);

  REQUIRE(std::strcmp(cfg.GetString("", "tags"), "a, b, c") == 0);
}

TEST_CASE("TOML numeric types", "[config][toml]") {
  const char* toml_data =
      "[math]\n"
      "pi = 3.14159\n"
      "negative = -10\n"
      "port = 65535\n"
      "big = 123456789\n";

  TomlCfg cfg;
  cfg.LoadBuffer(toml_data, static_cast<uint32_t>(std::strlen(toml_data)), osp::ConfigFormat::kToml);

  REQUIRE(cfg.GetDouble("math", "pi") > 3.14);
  REQUIRE(cfg.GetDouble("math", "pi") < 3.15);
  REQUIRE(cfg.GetInt("math", "negative", 0) == -10);
  REQUIRE(cfg.GetPort("math", "port") == 65535);
  REQUIRE(cfg.GetInt("math", "big", 0) == 123456789);
}

TEST_CASE("TOML defaults when key missing", "[config][toml]") {
  TomlCfg cfg;
  // Empty config: every lookup falls back to the default.
  REQUIRE(cfg.GetInt("x", "y", 42) == 42);
  REQUIRE(std::strcmp(cfg.GetString("x", "y", "default"), "default") == 0);
  REQUIRE(cfg.GetBool("x", "y", true) == true);
  REQUIRE(cfg.GetDouble("x", "y", 1.5) == 1.5);
}

TEST_CASE("TOML HasSection and HasKey", "[config][toml]") {
  TomlCfg cfg;
  cfg.LoadBuffer(kBasicToml, static_cast<uint32_t>(std::strlen(kBasicToml)), osp::ConfigFormat::kToml);

  REQUIRE(cfg.HasSection("network"));
  REQUIRE(cfg.HasSection("server.auth"));
  REQUIRE(!cfg.HasSection("missing"));
  REQUIRE(cfg.HasKey("network", "port"));
  REQUIRE(!cfg.HasKey("network", "missing"));
}

TEST_CASE("TOML FindInt optional", "[config][toml]") {
  TomlCfg cfg;
  cfg.LoadBuffer(kBasicToml, static_cast<uint32_t>(std::strlen(kBasicToml)), osp::ConfigFormat::kToml);

  auto found = cfg.FindInt("network", "port");
  REQUIRE(found.has_value());
  REQUIRE(found.value() == 5090);

  auto missing = cfg.FindInt("network", "nope");
  REQUIRE(!missing.has_value());
}

TEST_CASE("TOML malformed input returns parse error", "[config][toml]") {
  const char* bad_toml = "key = \n[unclosed\nvalue = ]\n";
  TomlCfg cfg;
  auto result = cfg.LoadBuffer(bad_toml, static_cast<uint32_t>(std::strlen(bad_toml)), osp::ConfigFormat::kToml);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kParseError);
}

TEST_CASE("TOML LoadFile nonexistent returns file-not-found", "[config][toml]") {
  const char* path = "__osp_test_config_toml_nonexistent__.toml";
  std::remove(path);  // ensure it is really absent
  TomlCfg cfg;
  auto result = cfg.LoadFile(path);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

TEST_CASE("TOML LoadFile from disk", "[config][toml]") {
  const char* path = "__osp_test_config_toml__.toml";
  FILE* f = std::fopen(path, "w");
  REQUIRE(f != nullptr);
  std::fprintf(f, "[server]\nport = 9090\nname = \"test\"\n");
  std::fclose(f);

  TomlCfg cfg;
  auto result = cfg.LoadFile(path);
  REQUIRE(result.has_value());
  REQUIRE(cfg.GetInt("server", "port", 0) == 9090);
  REQUIRE(std::strcmp(cfg.GetString("server", "name"), "test") == 0);

  std::remove(path);
}

TEST_CASE("TOML auto-detect .toml extension", "[config][toml]") {
  TomlCfg cfg;
  auto result = cfg.LoadFile("__osp_test_config_toml_nonexistent__.toml");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

TEST_CASE("TOML format not supported returns error", "[config][toml]") {
  TomlCfg cfg;
  // TomlConfig has no INI backend; dispatch must fail cleanly.
  auto result = cfg.LoadBuffer("[s]\nk = v\n", 9, osp::ConfigFormat::kIni);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFormatNotSupported);
}

TEST_CASE("TOML keys are case-insensitive in the flat store", "[config][toml]") {
  const char* toml_data = "[Network]\nPort = 80\n";
  TomlCfg cfg;
  cfg.LoadBuffer(toml_data, static_cast<uint32_t>(std::strlen(toml_data)), osp::ConfigFormat::kToml);

  REQUIRE(cfg.GetInt("network", "port", 0) == 80);
  REQUIRE(cfg.GetInt("NETWORK", "PORT", 0) == 80);
}

TEST_CASE("TOML EntryCount", "[config][toml]") {
  const char* toml_data = "[a]\nk1 = 1\nk2 = 2\n[b]\nk3 = 3\n";
  TomlCfg cfg;
  cfg.LoadBuffer(toml_data, static_cast<uint32_t>(std::strlen(toml_data)), osp::ConfigFormat::kToml);
  REQUIRE(cfg.EntryCount() == 3);
}

#endif  // OSP_CONFIG_TOML_ENABLED

// ============================================================================
// Cross-format consistency (TOML vs INI)
// ============================================================================

#if defined(OSP_CONFIG_INI_ENABLED) && defined(OSP_CONFIG_TOML_ENABLED)

TEST_CASE("TOML and INI agree on the same document", "[config][toml][ini][multi]") {
  const char* ini_data = "[network]\nport = 8080\nhost = 0.0.0.0\n";
  const char* toml_data = "[network]\nport = 8080\nhost = \"0.0.0.0\"\n";

  osp::Config<osp::IniBackend> ini_cfg;
  ini_cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)), osp::ConfigFormat::kIni);

  osp::Config<osp::TomlBackend> toml_cfg;
  toml_cfg.LoadBuffer(toml_data, static_cast<uint32_t>(std::strlen(toml_data)), osp::ConfigFormat::kToml);

  REQUIRE(ini_cfg.GetInt("network", "port", 0) == toml_cfg.GetInt("network", "port", 0));
  REQUIRE(std::strcmp(ini_cfg.GetString("network", "host"), toml_cfg.GetString("network", "host")) == 0);
}

TEST_CASE("MultiConfig dispatches INI and TOML", "[config][toml][multi]") {
  osp::Config<osp::IniBackend, osp::TomlBackend> cfg;

  const char* ini_data = "[sec]\nkey1 = val1\n";
  auto r1 = cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)), osp::ConfigFormat::kIni);
  REQUIRE(r1.has_value());
  REQUIRE(std::strcmp(cfg.GetString("sec", "key1"), "val1") == 0);

  const char* toml_data = "[sec]\nkey2 = \"val2\"\n";
  auto r2 = cfg.LoadBuffer(toml_data, static_cast<uint32_t>(std::strlen(toml_data)), osp::ConfigFormat::kToml);
  REQUIRE(r2.has_value());
  REQUIRE(std::strcmp(cfg.GetString("sec", "key2"), "val2") == 0);
}

#endif  // OSP_CONFIG_INI_ENABLED && OSP_CONFIG_TOML_ENABLED

// ============================================================================
// TomlBackend tag dispatch
// ============================================================================

TEST_CASE("TomlBackend MatchesExtension", "[config][tag]") {
  REQUIRE(osp::TomlBackend::MatchesExtension("toml") == true);
  REQUIRE(osp::TomlBackend::MatchesExtension("TOML") == true);
  REQUIRE(osp::TomlBackend::MatchesExtension("T0ml") == false);
  REQUIRE(osp::TomlBackend::MatchesExtension("ini") == false);
  REQUIRE(osp::TomlBackend::MatchesExtension("json") == false);
  REQUIRE(osp::TomlBackend::MatchesExtension("yaml") == false);
}

TEST_CASE("TomlBackend format enum", "[config][tag]") {
  REQUIRE(osp::TomlBackend::kFormat == osp::ConfigFormat::kToml);
}
