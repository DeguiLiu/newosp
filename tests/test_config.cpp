/**
 * @file test_config.cpp
 * @brief Tests for config.hpp
 */

#include "osp/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>

#ifdef OSP_CONFIG_INI_ENABLED

TEST_CASE("Config LoadBuffer INI basic", "[config][ini]") {
  const char* ini_data =
      "[network]\n"
      "port = 5090\n"
      "host = 0.0.0.0\n"
      "[log]\n"
      "level = INFO\n";

  osp::Config cfg;
  auto result = cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                               osp::ConfigFormat::kIni);
  REQUIRE(result.has_value());

  REQUIRE(cfg.GetInt("network", "port", 0) == 5090);
  REQUIRE(std::strcmp(cfg.GetString("network", "host"), "0.0.0.0") == 0);
  REQUIRE(std::strcmp(cfg.GetString("log", "level"), "INFO") == 0);
}

TEST_CASE("Config GetString default", "[config]") {
  osp::Config cfg;
  REQUIRE(std::strcmp(cfg.GetString("x", "y", "default"), "default") == 0);
}

TEST_CASE("Config GetInt default", "[config]") {
  osp::Config cfg;
  REQUIRE(cfg.GetInt("x", "y", 42) == 42);
}

TEST_CASE("Config GetBool", "[config][ini]") {
  const char* ini_data =
      "[flags]\n"
      "debug = true\n"
      "verbose = 1\n"
      "quiet = false\n"
      "enabled = yes\n"
      "active = on\n";

  osp::Config cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetBool("flags", "debug") == true);
  REQUIRE(cfg.GetBool("flags", "verbose") == true);
  REQUIRE(cfg.GetBool("flags", "quiet") == false);
  REQUIRE(cfg.GetBool("flags", "enabled") == true);
  REQUIRE(cfg.GetBool("flags", "active") == true);
}

TEST_CASE("Config GetPort", "[config][ini]") {
  const char* ini_data = "[net]\nport = 8080\n";
  osp::Config cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetPort("net", "port") == 8080);
  REQUIRE(cfg.GetPort("net", "missing", 3000) == 3000);
}

TEST_CASE("Config GetDouble", "[config][ini]") {
  const char* ini_data = "[math]\npi = 3.14159\n";
  osp::Config cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.GetDouble("math", "pi") > 3.14);
  REQUIRE(cfg.GetDouble("math", "pi") < 3.15);
}

TEST_CASE("Config HasSection and HasKey", "[config][ini]") {
  const char* ini_data = "[net]\nport = 80\n";
  osp::Config cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  REQUIRE(cfg.HasSection("net"));
  REQUIRE(!cfg.HasSection("missing"));
  REQUIRE(cfg.HasKey("net", "port"));
  REQUIRE(!cfg.HasKey("net", "missing"));
}

TEST_CASE("Config FindInt optional", "[config][ini]") {
  const char* ini_data = "[data]\ncount = 100\n";
  osp::Config cfg;
  cfg.LoadBuffer(ini_data, static_cast<uint32_t>(std::strlen(ini_data)),
                 osp::ConfigFormat::kIni);

  auto found = cfg.FindInt("data", "count");
  REQUIRE(found.has_value());
  REQUIRE(found.value() == 100);

  auto missing = cfg.FindInt("data", "nope");
  REQUIRE(!missing.has_value());
}

TEST_CASE("Config override on reload", "[config][ini]") {
  const char* ini1 = "[s]\nk = v1\n";
  const char* ini2 = "[s]\nk = v2\n";
  osp::Config cfg;
  cfg.LoadBuffer(ini1, static_cast<uint32_t>(std::strlen(ini1)),
                 osp::ConfigFormat::kIni);
  cfg.LoadBuffer(ini2, static_cast<uint32_t>(std::strlen(ini2)),
                 osp::ConfigFormat::kIni);

  REQUIRE(std::strcmp(cfg.GetString("s", "k"), "v2") == 0);
}

TEST_CASE("Config LoadFile nonexistent", "[config][ini]") {
  osp::Config cfg;
  auto result = cfg.LoadFile("/tmp/__nonexistent_file__.ini");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

TEST_CASE("Config format not supported", "[config]") {
  osp::Config cfg;
  auto result = cfg.LoadBuffer("{}", 2, osp::ConfigFormat::kJson);
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFormatNotSupported);
}

TEST_CASE("Config DetectFormat auto INI", "[config][ini]") {
  osp::Config cfg;
  // This file doesn't exist but we can at least verify that auto-detection
  // routes to INI for .ini extension (will fail with kFileNotFound)
  auto result = cfg.LoadFile("/tmp/__nonexistent__.ini");
  REQUIRE(!result.has_value());
  REQUIRE(result.get_error() == osp::ConfigError::kFileNotFound);
}

#else

TEST_CASE("Config INI disabled", "[config]") {
  osp::Config cfg;
  auto result = cfg.LoadBuffer("", 0, osp::ConfigFormat::kIni);
  REQUIRE(!result.has_value());
}

#endif
