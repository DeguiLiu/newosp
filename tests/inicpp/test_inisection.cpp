/*
 * test_inisection.cpp
 *
 * Tests for osp::ini::IniSection and osp::ini::IniFile section operations.
 */

#include "osp/inicpp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <sstream>

TEST_CASE("IniFile multiple sections", "IniSection") {
  std::istringstream ss("[Sec1]\na=1\n[Sec2]\nb=2\n[Sec3]\nc=3");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 3);
  REQUIRE(inif["Sec1"]["a"].As<int>() == 1);
  REQUIRE(inif["Sec2"]["b"].As<int>() == 2);
  REQUIRE(inif["Sec3"]["c"].As<int>() == 3);
}

TEST_CASE("IniFile multiple fields in one section", "IniSection") {
  std::istringstream ss("[Config]\nhost=localhost\nport=8080\nverbose=true\ntimeout=30.5");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Config"].size() == 4);
  REQUIRE(inif["Config"]["host"].As<std::string>() == "localhost");
  REQUIRE(inif["Config"]["port"].As<int>() == 8080);
  REQUIRE(inif["Config"]["verbose"].As<bool>() == true);
  REQUIRE(inif["Config"]["timeout"].As<double>() == Approx(30.5));
}

TEST_CASE("IniFile section with special characters in name", "IniSection") {
  std::istringstream ss("[Section With Spaces]\nkey=val\n[section-with-dashes]\nk=v\n[UPPERCASE]\nx=y");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 3);
  REQUIRE(inif.find("Section With Spaces") != inif.end());
  REQUIRE(inif.find("section-with-dashes") != inif.end());
  REQUIRE(inif.find("UPPERCASE") != inif.end());
}

TEST_CASE("IniFile create sections programmatically", "IniSection") {
  osp::ini::IniFile inif;
  inif["NewSection"]["key1"] = "value1";
  inif["NewSection"]["key2"] = 42;
  inif["Another"]["flag"] = true;

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["NewSection"]["key1"].As<std::string>() == "value1");
  REQUIRE(inif["NewSection"]["key2"].As<int>() == 42);
  REQUIRE(inif["Another"]["flag"].As<bool>() == true);
}

TEST_CASE("IniFile encode and decode roundtrip", "IniSection") {
  osp::ini::IniFile original;
  original["Database"]["host"] = "127.0.0.1";
  original["Database"]["port"] = 5432;
  original["Database"]["name"] = "mydb";
  original["Logging"]["level"] = "debug";
  original["Logging"]["enabled"] = true;

  std::string encoded = original.Encode();

  osp::ini::IniFile decoded;
  decoded.Decode(encoded);

  REQUIRE(decoded["Database"]["host"].As<std::string>() == "127.0.0.1");
  REQUIRE(decoded["Database"]["port"].As<int>() == 5432);
  REQUIRE(decoded["Database"]["name"].As<std::string>() == "mydb");
  REQUIRE(decoded["Logging"]["level"].As<std::string>() == "debug");
  REQUIRE(decoded["Logging"]["enabled"].As<bool>() == true);
}

TEST_CASE("IniFile section iteration", "IniSection") {
  std::istringstream ss("[A]\nx=1\n[B]\ny=2\n[C]\nz=3");
  osp::ini::IniFile inif(ss);

  int count = 0;
  for (const auto& section : inif) {
    REQUIRE(section.second.size() == 1);
    ++count;
  }
  REQUIRE(count == 3);
}

TEST_CASE("IniFile field iteration within section", "IniSection") {
  std::istringstream ss("[Data]\na=1\nb=2\nc=3\nd=4\ne=5");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Data"].size() == 5);

  int count = 0;
  for (const auto& field : inif["Data"]) {
    REQUIRE_FALSE(field.first.empty());
    ++count;
  }
  REQUIRE(count == 5);
}

TEST_CASE("IniFile find returns end for non-existent section", "IniSection") {
  std::istringstream ss("[Exists]\nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("Exists") != inif.end());
  REQUIRE(inif.find("DoesNotExist") == inif.end());
}

TEST_CASE("IniSection find returns end for non-existent field", "IniSection") {
  std::istringstream ss("[Sec]\nfoo=bar");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"].find("foo") != inif["Sec"].end());
  REQUIRE(inif["Sec"].find("baz") == inif["Sec"].end());
}

TEST_CASE("IniFile case insensitive sections and fields", "IniSection") {
  std::istringstream ss("[MySection]\nmyKey=myValue");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif.find("MYSECTION") != inif.end());
  REQUIRE(inif.find("mysection") != inif.end());
  REQUIRE(inif.find("MySection") != inif.end());

  REQUIRE(inif["mysection"].find("MYKEY") != inif["mysection"].end());
  REQUIRE(inif["mysection"]["mykey"].As<std::string>() == "myValue");
}

TEST_CASE("IniFile empty section followed by populated section", "IniSection") {
  std::istringstream ss("[Empty]\n[HasData]\nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["Empty"].size() == 0);
  REQUIRE(inif["HasData"].size() == 1);
}
