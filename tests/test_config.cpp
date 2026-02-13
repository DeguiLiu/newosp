/**
 * @file test_config.cpp
 * @brief Tests for config.hpp - template-based multi-format config parser.
 */

#include "osp/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>

// ============================================================================
// INI Backend Tests
// ============================================================================

#ifdef OSP_CONFIG_INI_ENABLED

using IniCfg = osp::Config<osp::IniBackend>;

TEST_CASE("INI LoadBuffer basic", "[config][ini]") {
  const char* ini_data =
      "[network]\n"
      "port = 5090\n"
      "host = 0.0.0.0\n"
      "[log]\n"
      "level = INFO\n";

  IniCfg cfg;
  auto result = cfg.LoadBuffer(ini_data,
                               static_cast<uint32_t>(std::strlen(ini_data)),
                               osp::ConfigFormat::kIni);
  REQUIRE(result.has_value());

  REQUIRE(cfg.GetInt("network", "port", 0) == 5090);
  REQUIRE(std::strcmp(cfg.GetString("network", "host"), "0.0.0.0") == 0);
  REQUIRE(std::strcmp(cfg.GetString("log", "level"), "INFO") == 0);
}

TEST_CASE("INI GetString default", "[config][ini]") {
  IniCfg cfg;
  REQUIRE(std::strcmp(cfg.GetString("x", "y", "default"), "default") == 0);
}

TEST_CASE("INI GetInt default", "[config][ini]") {
  IniCfg cfg;
  REQUIRE(cfg.GetInt("x", "y", 42) == 42);
}

TEST_CASE("INI GetBool", "[config][ini]") {
  const char* ini_data =
      "[flags]\n"
      "debug = true\n"
      "verbose = 1\n"
      "quiet = false\n"
      "enabled = yes\n"
      "active = on\n";

  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetBool("flags", "debug") == true);
  REQUIRE(cfg.GetBool("flags", "verbose") == true);
  REQUIRE(cfg.GetBool("flags", "quiet") == false);
  REQUIRE(cfg.GetBool("flags", "enabled") == true);
  REQUIRE(cfg.GetBool("flags", "active") == true);
}

TEST_CASE("INI GetPort", "[config][ini]") {
  const char* ini_data = "[net]\nport = 8080\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetPort("net", "port") == 8080);
  REQUIRE(cfg.GetPort("net", "missing", 3000) == 3000);
}

TEST_CASE("INI GetDouble", "[config][ini]") {
  const char* ini_data = "[math]\npi = 3.14159\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetDouble("math", "pi") > 3.14);
  REQUIRE(cfg.GetDouble("math", "pi") < 3.15);
}

TEST_CASE("INI HasSection and HasKey", "[config][ini]") {
  const char* ini_data = "[net]\nport = 80\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.HasSection("net"));
  REQUIRE(!cfg.HasSection("missing"));
  REQUIRE(cfg.HasKey("net", "port"));
  REQUIRE(!cfg.HasKey("net", "missing"));
}

TEST_CASE("INI FindInt optional", "[config][ini]") {
  const char* ini_data = "[data]\ncount = 100\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  auto found = cfg.FindInt("data", "count");
  REQUIRE(found.has_value());
  REQUIRE(found.value() == 100);

  auto missing = cfg.FindInt("data", "nope");
  REQUIRE(!missing.has_value());
}

TEST_CASE("INI override on reload", "[config][ini]") {
  const char* ini1 = "[s]\nk = v1\n";
  const char* ini2 = "[s]\nk = v2\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini1, static_cast<uint32_t>(std::strlen(ini1)),
                 osp::ConfigFormat::kIni);
  cfg.LoadBuffer(ini2, static_cast<uint32_t>(std::strlen(ini2)),
                 osp::ConfigFormat::kIni);

  REQUIRE(std::strcmp(cfg.GetString("s", "k"), "v2") == 0);
}

TEST_CASE("INI LoadFile nonexistent", "[config][ini]") {
  IniCfg cfg;
  auto result = cfg.LoadFile("/tmp/__nonexistent_file__.ini");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

TEST_CASE("INI format not supported returns error", "[config][ini]") {
  IniCfg cfg;
  // IniConfig does not support JSON format
  auto result = cfg.LoadBuffer("{}", 2, osp::ConfigFormat::kJson);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFormatNotSupported);
}

TEST_CASE("INI auto-detect extension", "[config][ini]") {
  IniCfg cfg;
  // File doesn't exist but auto-detection routes to INI for .ini extension
  auto result = cfg.LoadFile("/tmp/__nonexistent__.ini");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

TEST_CASE("INI LoadFile from disk", "[config][ini]") {
  const char* path = "/tmp/__osp_test_config__.ini";
  FILE* f = std::fopen(path, "w");
  REQUIRE(f != nullptr);
  std::fprintf(f, "[server]\nport = 9090\nname = test\n");
  std::fclose(f);

  IniCfg cfg;
  auto result = cfg.LoadFile(path);
  REQUIRE(result.has_value());
  REQUIRE(cfg.GetInt("server", "port", 0) == 9090);
  REQUIRE(std::strcmp(cfg.GetString("server", "name"), "test") == 0);

  std::remove(path);
}

TEST_CASE("INI EntryCount", "[config][ini]") {
  const char* ini_data = "[a]\nk1 = v1\nk2 = v2\n[b]\nk3 = v3\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);
  REQUIRE(cfg.EntryCount() == 3);
}

TEST_CASE("INI case insensitive keys", "[config][ini]") {
  const char* ini_data = "[Network]\nPort = 80\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetInt("network", "port", 0) == 80);
  REQUIRE(cfg.GetInt("NETWORK", "PORT", 0) == 80);
}

#endif  // OSP_CONFIG_INI_ENABLED

// ============================================================================
// JSON Backend Tests
// ============================================================================

#ifdef OSP_CONFIG_JSON_ENABLED

using JsonCfg = osp::Config<osp::JsonBackend>;

TEST_CASE("JSON LoadBuffer basic", "[config][json]") {
  const char* json_data = R"({
    "network": {
      "port": 5090,
      "host": "0.0.0.0"
    },
    "log": {
      "level": "INFO"
    }
  })";

  JsonCfg cfg;
  auto result = cfg.LoadBuffer(json_data,
                                static_cast<uint32_t>(std::strlen(json_data)),
                                osp::ConfigFormat::kJson);
  REQUIRE(result.has_value());

  REQUIRE(cfg.GetInt("network", "port", 0) == 5090);
  REQUIRE(std::strcmp(cfg.GetString("network", "host"), "0.0.0.0") == 0);
  REQUIRE(std::strcmp(cfg.GetString("log", "level"), "INFO") == 0);
}

TEST_CASE("JSON flat keys (no section)", "[config][json]") {
  const char* json_data = R"({"name": "test", "count": 42})";

  JsonCfg cfg;
  auto result = cfg.LoadBuffer(json_data,
                                static_cast<uint32_t>(std::strlen(json_data)),
                                osp::ConfigFormat::kJson);
  REQUIRE(result.has_value());

  REQUIRE(std::strcmp(cfg.GetString("", "name"), "test") == 0);
  REQUIRE(cfg.GetInt("", "count", 0) == 42);
}

TEST_CASE("JSON boolean values", "[config][json]") {
  const char* json_data = R"({
    "flags": {
      "debug": true,
      "verbose": false
    }
  })";

  JsonCfg cfg;
  cfg.LoadBuffer(json_data, static_cast<uint32_t>(std::strlen(json_data)),
                 osp::ConfigFormat::kJson);

  REQUIRE(cfg.GetBool("flags", "debug") == true);
  REQUIRE(cfg.GetBool("flags", "verbose") == false);
}

TEST_CASE("JSON numeric values", "[config][json]") {
  const char* json_data = R"({
    "math": {
      "pi": 3.14159,
      "negative": -10,
      "port": 65535
    }
  })";

  JsonCfg cfg;
  cfg.LoadBuffer(json_data, static_cast<uint32_t>(std::strlen(json_data)),
                 osp::ConfigFormat::kJson);

  REQUIRE(cfg.GetDouble("math", "pi") > 3.14);
  REQUIRE(cfg.GetDouble("math", "pi") < 3.15);
  REQUIRE(cfg.GetInt("math", "negative", 0) == -10);
  REQUIRE(cfg.GetPort("math", "port") == 65535);
}

TEST_CASE("JSON parse error", "[config][json]") {
  const char* bad_json = "{ invalid json ]";
  JsonCfg cfg;
  auto result = cfg.LoadBuffer(bad_json,
                                static_cast<uint32_t>(std::strlen(bad_json)),
                                osp::ConfigFormat::kJson);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kParseError);
}

TEST_CASE("JSON LoadFile from disk", "[config][json]") {
  const char* path = "/tmp/__osp_test_config__.json";
  FILE* f = std::fopen(path, "w");
  REQUIRE(f != nullptr);
  std::fprintf(f, R"({"server": {"port": 9090, "name": "test"}})");
  std::fclose(f);

  JsonCfg cfg;
  auto result = cfg.LoadFile(path);
  REQUIRE(result.has_value());
  REQUIRE(cfg.GetInt("server", "port", 0) == 9090);
  REQUIRE(std::strcmp(cfg.GetString("server", "name"), "test") == 0);

  std::remove(path);
}

TEST_CASE("JSON auto-detect extension", "[config][json]") {
  JsonCfg cfg;
  auto result = cfg.LoadFile("/tmp/__nonexistent__.json");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

#endif  // OSP_CONFIG_JSON_ENABLED

// ============================================================================
// YAML Backend Tests
// ============================================================================

#ifdef OSP_CONFIG_YAML_ENABLED

using YamlCfg = osp::Config<osp::YamlBackend>;

TEST_CASE("YAML LoadBuffer basic", "[config][yaml]") {
  const char* yaml_data =
      "network:\n"
      "  port: 5090\n"
      "  host: \"0.0.0.0\"\n"
      "log:\n"
      "  level: INFO\n";

  YamlCfg cfg;
  auto result = cfg.LoadBuffer(yaml_data,
                                static_cast<uint32_t>(std::strlen(yaml_data)),
                                osp::ConfigFormat::kYaml);
  REQUIRE(result.has_value());

  REQUIRE(cfg.GetInt("network", "port", 0) == 5090);
  REQUIRE(std::strcmp(cfg.GetString("network", "host"), "0.0.0.0") == 0);
  REQUIRE(std::strcmp(cfg.GetString("log", "level"), "INFO") == 0);
}

TEST_CASE("YAML flat keys (no section)", "[config][yaml]") {
  const char* yaml_data =
      "name: test\n"
      "count: 42\n";

  YamlCfg cfg;
  auto result = cfg.LoadBuffer(yaml_data,
                                static_cast<uint32_t>(std::strlen(yaml_data)),
                                osp::ConfigFormat::kYaml);
  REQUIRE(result.has_value());

  REQUIRE(std::strcmp(cfg.GetString("", "name"), "test") == 0);
  REQUIRE(cfg.GetInt("", "count", 0) == 42);
}

TEST_CASE("YAML boolean values", "[config][yaml]") {
  const char* yaml_data =
      "flags:\n"
      "  debug: true\n"
      "  verbose: false\n";

  YamlCfg cfg;
  cfg.LoadBuffer(yaml_data, static_cast<uint32_t>(std::strlen(yaml_data)),
                 osp::ConfigFormat::kYaml);

  REQUIRE(cfg.GetBool("flags", "debug") == true);
  REQUIRE(cfg.GetBool("flags", "verbose") == false);
}

TEST_CASE("YAML numeric values", "[config][yaml]") {
  const char* yaml_data =
      "math:\n"
      "  pi: 3.14159\n"
      "  negative: -10\n"
      "  port: 65535\n";

  YamlCfg cfg;
  cfg.LoadBuffer(yaml_data, static_cast<uint32_t>(std::strlen(yaml_data)),
                 osp::ConfigFormat::kYaml);

  REQUIRE(cfg.GetDouble("math", "pi") > 3.14);
  REQUIRE(cfg.GetDouble("math", "pi") < 3.15);
  REQUIRE(cfg.GetInt("math", "negative", 0) == -10);
  REQUIRE(cfg.GetPort("math", "port") == 65535);
}

TEST_CASE("YAML LoadFile from disk", "[config][yaml]") {
  const char* path = "/tmp/__osp_test_config__.yaml";
  FILE* f = std::fopen(path, "w");
  REQUIRE(f != nullptr);
  std::fprintf(f, "server:\n  port: 9090\n  name: test\n");
  std::fclose(f);

  YamlCfg cfg;
  auto result = cfg.LoadFile(path);
  REQUIRE(result.has_value());
  REQUIRE(cfg.GetInt("server", "port", 0) == 9090);
  REQUIRE(std::strcmp(cfg.GetString("server", "name"), "test") == 0);

  std::remove(path);
}

TEST_CASE("YAML auto-detect extension yml", "[config][yaml]") {
  YamlCfg cfg;
  auto result = cfg.LoadFile("/tmp/__nonexistent__.yml");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

#endif  // OSP_CONFIG_YAML_ENABLED

// ============================================================================
// MultiConfig Tests (all enabled backends)
// ============================================================================

TEST_CASE("MultiConfig instantiation", "[config]") {
  osp::MultiConfig cfg;
  REQUIRE(cfg.EntryCount() == 0);
}

#if defined(OSP_CONFIG_INI_ENABLED) && defined(OSP_CONFIG_JSON_ENABLED)

TEST_CASE("MultiConfig dispatches INI and JSON", "[config][multi]") {
  osp::MultiConfig cfg;

  // Load INI
  const char* ini_data = "[sec]\nkey1 = val1\n";
  auto r1 = cfg.LoadBuffer(ini_data,
                            static_cast<uint32_t>(std::strlen(ini_data)),
                            osp::ConfigFormat::kIni);
  REQUIRE(r1.has_value());
  REQUIRE(std::strcmp(cfg.GetString("sec", "key1"), "val1") == 0);

  // Load JSON (additive - entries accumulate)
  osp::MultiConfig cfg2;
  const char* json_data = R"({"sec": {"key2": "val2"}})";
  auto r2 = cfg2.LoadBuffer(json_data,
                             static_cast<uint32_t>(std::strlen(json_data)),
                             osp::ConfigFormat::kJson);
  REQUIRE(r2.has_value());
  REQUIRE(std::strcmp(cfg2.GetString("sec", "key2"), "val2") == 0);
}

#endif

#if defined(OSP_CONFIG_INI_ENABLED) && defined(OSP_CONFIG_YAML_ENABLED)

TEST_CASE("MultiConfig dispatches INI and YAML", "[config][multi]") {
  osp::MultiConfig cfg;

  const char* ini_data = "[sec]\nkey1 = val1\n";
  auto r1 = cfg.LoadBuffer(ini_data,
                            static_cast<uint32_t>(std::strlen(ini_data)),
                            osp::ConfigFormat::kIni);
  REQUIRE(r1.has_value());

  osp::MultiConfig cfg2;
  const char* yaml_data = "sec:\n  key2: val2\n";
  auto r2 = cfg2.LoadBuffer(yaml_data,
                             static_cast<uint32_t>(std::strlen(yaml_data)),
                             osp::ConfigFormat::kYaml);
  REQUIRE(r2.has_value());
  REQUIRE(std::strcmp(cfg2.GetString("sec", "key2"), "val2") == 0);
}

#endif

// ============================================================================
// Backend tag dispatch tests
// ============================================================================

TEST_CASE("IniBackend MatchesExtension", "[config][tag]") {
  REQUIRE(osp::IniBackend::MatchesExtension("ini") == true);
  REQUIRE(osp::IniBackend::MatchesExtension("cfg") == true);
  REQUIRE(osp::IniBackend::MatchesExtension("conf") == true);
  REQUIRE(osp::IniBackend::MatchesExtension("INI") == true);
  REQUIRE(osp::IniBackend::MatchesExtension("json") == false);
  REQUIRE(osp::IniBackend::MatchesExtension("yaml") == false);
}

TEST_CASE("JsonBackend MatchesExtension", "[config][tag]") {
  REQUIRE(osp::JsonBackend::MatchesExtension("json") == true);
  REQUIRE(osp::JsonBackend::MatchesExtension("JSON") == true);
  REQUIRE(osp::JsonBackend::MatchesExtension("ini") == false);
  REQUIRE(osp::JsonBackend::MatchesExtension("yaml") == false);
}

TEST_CASE("YamlBackend MatchesExtension", "[config][tag]") {
  REQUIRE(osp::YamlBackend::MatchesExtension("yaml") == true);
  REQUIRE(osp::YamlBackend::MatchesExtension("yml") == true);
  REQUIRE(osp::YamlBackend::MatchesExtension("YAML") == true);
  REQUIRE(osp::YamlBackend::MatchesExtension("json") == false);
  REQUIRE(osp::YamlBackend::MatchesExtension("ini") == false);
}

TEST_CASE("ConfigFormat enum values", "[config][tag]") {
  REQUIRE(osp::IniBackend::kFormat == osp::ConfigFormat::kIni);
  REQUIRE(osp::JsonBackend::kFormat == osp::ConfigFormat::kJson);
  REQUIRE(osp::YamlBackend::kFormat == osp::ConfigFormat::kYaml);
}

// ============================================================================
// ConfigStore common behavior (no backend needed)
// ============================================================================

TEST_CASE("ConfigStore GetBool default", "[config]") {
  osp::MultiConfig cfg;
  REQUIRE(cfg.GetBool("x", "y") == false);
  REQUIRE(cfg.GetBool("x", "y", true) == true);
}

TEST_CASE("ConfigStore GetPort clamping", "[config][ini]") {
#ifdef OSP_CONFIG_INI_ENABLED
  const char* ini_data = "[net]\nhigh = 99999\nneg = -1\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetPort("net", "high") == 65535);
  REQUIRE(cfg.GetPort("net", "neg") == 0);
#endif
}

TEST_CASE("ConfigStore FindBool optional", "[config][ini]") {
#ifdef OSP_CONFIG_INI_ENABLED
  const char* ini_data = "[f]\nok = true\n";
  IniCfg cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  auto found = cfg.FindBool("f", "ok");
  REQUIRE(found.has_value());
  REQUIRE(found.value() == true);

  auto missing = cfg.FindBool("f", "nope");
  REQUIRE(!missing.has_value());
#endif
}
