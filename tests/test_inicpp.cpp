/**
 * @file test_inicpp.cpp
 * @brief Comprehensive tests for inicpp library (embedded ini parser).
 *
 * Tests cover:
 * - IniField operations and type conversions
 * - IniSection operations
 * - IniFile parsing and encoding
 * - File I/O operations
 * - Edge cases and error handling
 */

#include "osp/inicpp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using Catch::Approx;

// ============================================================================
// IniField Tests
// ============================================================================

TEST_CASE("IniField default constructor creates empty field", "[inicpp][field]") {
  osp::ini::IniField field;
  REQUIRE(field.As<std::string>() == "");
}

TEST_CASE("IniField string constructor and assignment", "[inicpp][field]") {
  osp::ini::IniField field("hello");
  REQUIRE(field.As<std::string>() == "hello");
  
  field = std::string("world");
  REQUIRE(field.As<std::string>() == "world");
}

TEST_CASE("IniField type conversions", "[inicpp][field]") {
  osp::ini::IniField field;

  // Integer
  field = 42;
  REQUIRE(field.As<int>() == 42);
  REQUIRE(field.As<long>() == 42L);

  // Boolean
  field = true;
  REQUIRE(field.As<bool>() == true);

  // Floating point
  field = 3.14;
  REQUIRE(field.As<double>() == Approx(3.14).margin(1e-3));
  
  field = 2.5f;
  REQUIRE(field.As<float>() == Approx(2.5f).margin(1e-3f));
}

// ============================================================================
// IniSection Tests
// ============================================================================

TEST_CASE("IniSection basic operations", "[inicpp][section]") {
  osp::ini::IniSection section;
  
  // Add fields
  section["name"] = "test";
  section["count"] = 10;
  section["enabled"] = true;
  
  REQUIRE(section.size() == 3);
  REQUIRE(section["name"].As<std::string>() == "test");
  REQUIRE(section["count"].As<int>() == 10);
  REQUIRE(section["enabled"].As<bool>() == true);
}

TEST_CASE("IniSection field access", "[inicpp][section]") {
  osp::ini::IniSection section;
  section["key"] = "value";
  
  REQUIRE(section.find("key") != section.end());
  REQUIRE(section.find("missing") == section.end());
}

// ============================================================================
// IniFile Decoding Tests
// ============================================================================

TEST_CASE("IniFile decode simple content", "[inicpp][decode]") {
  std::string content = "[Foo]\nhello=world\nnum=123\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini.size() == 1);
  REQUIRE(ini["Foo"]["hello"].As<std::string>() == "world");
  REQUIRE(ini["Foo"]["num"].As<int>() == 123);
}

TEST_CASE("IniFile decode multiple sections", "[inicpp][decode]") {
  std::string content = 
      "[Server]\n"
      "host=localhost\n"
      "port=8080\n"
      "[Database]\n"
      "name=mydb\n"
      "user=admin\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini.size() == 2);
  REQUIRE(ini["Server"]["host"].As<std::string>() == "localhost");
  REQUIRE(ini["Server"]["port"].As<int>() == 8080);
  REQUIRE(ini["Database"]["name"].As<std::string>() == "mydb");
  REQUIRE(ini["Database"]["user"].As<std::string>() == "admin");
}

TEST_CASE("IniFile decode with comments", "[inicpp][decode]") {
  std::string content = 
      "; This is a comment\n"
      "[Section]\n"
      "key1=value1 ; inline comment\n"
      "# Another comment style\n"
      "key2=value2\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini["Section"]["key1"].As<std::string>() == "value1");
  REQUIRE(ini["Section"]["key2"].As<std::string>() == "value2");
}

TEST_CASE("IniFile decode boolean values", "[inicpp][decode]") {
  std::string content = 
      "[Flags]\n"
      "t1=TRUE\n"
      "t2=true\n"
      "f1=FALSE\n"
      "f2=false\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini["Flags"]["t1"].As<bool>() == true);
  REQUIRE(ini["Flags"]["t2"].As<bool>() == true);
  REQUIRE(ini["Flags"]["f1"].As<bool>() == false);
  REQUIRE(ini["Flags"]["f2"].As<bool>() == false);
}

TEST_CASE("IniFile decode numeric values", "[inicpp][decode]") {
  std::string content = 
      "[Numbers]\n"
      "int_val=42\n"
      "negative=-100\n"
      "float_val=3.14159\n"
      "scientific=1.5e10\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini["Numbers"]["int_val"].As<int>() == 42);
  REQUIRE(ini["Numbers"]["negative"].As<int>() == -100);
  REQUIRE(ini["Numbers"]["float_val"].As<double>() == Approx(3.14159).margin(1e-5));
}

TEST_CASE("IniFile decode with whitespace", "[inicpp][decode]") {
  std::string content = 
      "[Section]\n"
      "  key1  =  value1  \n"
      "key2=value2\n"
      "  key3=value3\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini["Section"]["key1"].As<std::string>() == "value1");
  REQUIRE(ini["Section"]["key2"].As<std::string>() == "value2");
  REQUIRE(ini["Section"]["key3"].As<std::string>() == "value3");
}

TEST_CASE("IniFile decode empty sections", "[inicpp][decode]") {
  std::string content = "[Empty]\n[NonEmpty]\nkey=value\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini.size() == 2);
  REQUIRE(ini["Empty"].size() == 0);
  REQUIRE(ini["NonEmpty"].size() == 1);
}

// ============================================================================
// IniFile Encoding Tests
// ============================================================================

TEST_CASE("IniFile encode simple content", "[inicpp][encode]") {
  osp::ini::IniFile ini;
  ini["Section"]["key"] = "value";
  ini["Section"]["num"] = 42;
  
  std::string encoded = ini.Encode();
  
  REQUIRE(encoded.find("[Section]") != std::string::npos);
  REQUIRE(encoded.find("key=value") != std::string::npos);
  REQUIRE(encoded.find("num=42") != std::string::npos);
}

TEST_CASE("IniFile encode various types", "[inicpp][encode]") {
  osp::ini::IniFile ini;
  ini["Types"]["string"] = "text";
  ini["Types"]["integer"] = 123;
  ini["Types"]["float"] = 1.5f;
  ini["Types"]["boolean"] = true;
  
  std::string encoded = ini.Encode();
  
  // Decode back and verify
  osp::ini::IniFile decoded;
  decoded.Decode(encoded);
  
  REQUIRE(decoded["Types"]["string"].As<std::string>() == "text");
  REQUIRE(decoded["Types"]["integer"].As<int>() == 123);
  REQUIRE(decoded["Types"]["float"].As<float>() == Approx(1.5f).margin(1e-3f));
  REQUIRE(decoded["Types"]["boolean"].As<bool>() == true);
}

// ============================================================================
// File I/O Tests
// ============================================================================

static const char* kTempFile = "/tmp/test_inicpp_tmp.ini";

static void RemoveTempFile() {
  std::remove(kTempFile);
}

TEST_CASE("IniFile save and load", "[inicpp][fileio]") {
  // Create and save
  osp::ini::IniFile ini;
  ini["App"]["name"] = "TestApp";
  ini["App"]["version"] = 100;
  ini.Save(kTempFile);
  
  // Load and verify
  osp::ini::IniFile loaded;
  loaded.Load(kTempFile);
  
  REQUIRE(loaded["App"]["name"].As<std::string>() == "TestApp");
  REQUIRE(loaded["App"]["version"].As<int>() == 100);
  
  RemoveTempFile();
}

TEST_CASE("IniFile load from disk", "[inicpp][fileio]") {
  // Write test file
  std::ofstream ofs(kTempFile);
  ofs << "[Server]\nhost=localhost\nport=9090\n";
  ofs.close();
  
  // Load and verify
  osp::ini::IniFile ini;
  ini.Load(kTempFile);
  
  REQUIRE(ini.size() == 1);
  REQUIRE(ini["Server"]["host"].As<std::string>() == "localhost");
  REQUIRE(ini["Server"]["port"].As<int>() == 9090);
  
  RemoveTempFile();
}

TEST_CASE("IniFile round-trip save/load", "[inicpp][fileio]") {
  osp::ini::IniFile original;
  original["Config"]["timeout"] = 30.5;
  original["Config"]["retries"] = 3;
  original["Config"]["debug"] = false;
  
  // Save
  original.Save(kTempFile);
  
  // Load
  osp::ini::IniFile loaded;
  loaded.Load(kTempFile);
  
  // Verify
  REQUIRE(loaded["Config"]["timeout"].As<double>() == Approx(30.5));
  REQUIRE(loaded["Config"]["retries"].As<int>() == 3);
  REQUIRE(loaded["Config"]["debug"].As<bool>() == false);
  
  RemoveTempFile();
}

// ============================================================================
// Edge Cases and Error Handling Tests
// ============================================================================

TEST_CASE("IniFile decode duplicate fields overwrites by default", "[inicpp][edge]") {
  std::string content = 
      "[Section]\n"
      "key=value1\n"
      "key=value2\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  // Last value should win
  REQUIRE(ini["Section"]["key"].As<std::string>() == "value2");
}

TEST_CASE("IniFile decode empty values", "[inicpp][edge]") {
  std::string content = "[Section]\nkey=\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini["Section"]["key"].As<std::string>() == "");
}

TEST_CASE("IniFile decode special characters in values", "[inicpp][edge]") {
  std::string content = 
      "[Paths]\n"
      "path1=/usr/local/bin\n"
      "path2=C:\\\\Program Files\\\\App\n"
      "url=https://example.com:8080/path?key=value\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  REQUIRE(ini["Paths"]["path1"].As<std::string>() == "/usr/local/bin");
  REQUIRE(ini["Paths"]["url"].As<std::string>() == "https://example.com:8080/path?key=value");
}

TEST_CASE("IniFile decode hex and octal numbers", "[inicpp][edge]") {
  std::string content = 
      "[Numbers]\n"
      "hex=0xFF\n"
      "octal=0755\n"
      "decimal=255\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  // inicpp supports hex (0xFF = 255) and octal (0755 = 493) parsing
  REQUIRE(ini["Numbers"]["hex"].As<int>() == 255);
  REQUIRE(ini["Numbers"]["octal"].As<int>() == 493);
  REQUIRE(ini["Numbers"]["decimal"].As<int>() == 255);
}

TEST_CASE("IniFile case sensitivity", "[inicpp][edge]") {
  std::string content = 
      "[Section]\n"
      "Key=value1\n"
      "[section]\n"
      "key=value2\n";
  
  osp::ini::IniFile ini;
  ini.Decode(content);
  
  // Case-sensitive by default
  REQUIRE(ini.size() == 2);
  REQUIRE(ini.find("Section") != ini.end());
  REQUIRE(ini.find("section") != ini.end());
}

TEST_CASE("IniFile section and field iteration", "[inicpp][iteration]") {
  osp::ini::IniFile ini;
  ini["S1"]["k1"] = "v1";
  ini["S1"]["k2"] = "v2";
  ini["S2"]["k3"] = "v3";
  
  int sectionCount = 0;
  int fieldCount = 0;
  
  for (const auto& sectionPair : ini) {
    sectionCount++;
    for (const auto& fieldPair : sectionPair.second) {
      fieldCount++;
    }
  }
  
  REQUIRE(sectionCount == 2);
  REQUIRE(fieldCount == 3);
}

TEST_CASE("IniFile conversion error handling", "[inicpp][edge]") {
  osp::ini::IniFile ini;
  ini["Data"]["not_a_number"] = "abc";
  
  // Conversion to int should throw or return default
  REQUIRE_THROWS(ini["Data"]["not_a_number"].As<int>());
}

// ============================================================================
// Type Conversion Tests
// ============================================================================

TEST_CASE("IniField convert integers", "[inicpp][convert]") {
  osp::ini::IniField field;
  
  field = 42;
  REQUIRE(field.As<int>() == 42);
  REQUIRE(field.As<short>() == 42);
  REQUIRE(field.As<long>() == 42);
  
  field = -100;
  REQUIRE(field.As<int>() == -100);
}

TEST_CASE("IniField convert unsigned integers", "[inicpp][convert]") {
  osp::ini::IniField field;
  
  field = 255;
  REQUIRE(field.As<unsigned int>() == 255U);
  REQUIRE(field.As<unsigned short>() == 255U);
  REQUIRE(field.As<unsigned long>() == 255UL);
}

TEST_CASE("IniField convert floating point", "[inicpp][convert]") {
  osp::ini::IniField field;
  
  field = 3.14159;
  REQUIRE(field.As<double>() == Approx(3.14159).margin(1e-5));
  
  field = -0.001;
  REQUIRE(field.As<double>() == Approx(-0.001).margin(1e-6));
  
  field = 2.5f;
  REQUIRE(field.As<float>() == Approx(2.5f).margin(1e-3f));
}

TEST_CASE("IniField convert characters", "[inicpp][convert]") {
  osp::ini::IniField field;
  
  field = 'a';
  REQUIRE(field.As<char>() == 'a');
  
  field = 'Z';
  REQUIRE(field.As<unsigned char>() == static_cast<unsigned char>('Z'));
}

TEST_CASE("IniField convert strings", "[inicpp][convert]") {
  osp::ini::IniField field;
  
  field = std::string("hello world");
  REQUIRE(field.As<std::string>() == "hello world");
  
  field = "test";
  REQUIRE(field.As<std::string>() == "test");
}
