/*
 * test_file_io.cpp
 *
 * Tests for file load/save operations.
 */

#include "osp/inicpp.h"

#include <cstdio>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <fstream>
#include <string>

static const char* kTempFile = "test_inifile_tmp.ini";

static void WriteTempFile(const std::string& content) {
  std::ofstream ofs(kTempFile);
  ofs << content;
}

static void RemoveTempFile() {
  std::remove(kTempFile);
}

TEST_CASE("load ini file from disk", "FileIO") {
  WriteTempFile("[Server]\nhost=localhost\nport=9090\n");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Server"]["host"].As<std::string>() == "localhost");
  REQUIRE(inif["Server"]["port"].As<int>() == 9090);

  RemoveTempFile();
}

TEST_CASE("save ini file to disk", "FileIO") {
  osp::ini::IniFile inif;
  inif["App"]["name"] = "test_app";
  inif["App"]["version"] = 2;
  inif.Save(kTempFile);

  // reload and verify
  osp::ini::IniFile loaded;
  loaded.Load(kTempFile);
  REQUIRE(loaded["App"]["name"].As<std::string>() == "test_app");
  REQUIRE(loaded["App"]["version"].As<int>() == 2);

  RemoveTempFile();
}

TEST_CASE("save and load roundtrip with multiple sections", "FileIO") {
  osp::ini::IniFile inif;
  inif["DB"]["host"] = "db.example.com";
  inif["DB"]["port"] = 3306;
  inif["DB"]["user"] = "admin";
  inif["Cache"]["enabled"] = true;
  inif["Cache"]["ttl"] = 300;
  inif.Save(kTempFile);

  osp::ini::IniFile loaded;
  loaded.Load(kTempFile);

  REQUIRE(loaded["DB"]["host"].As<std::string>() == "db.example.com");
  REQUIRE(loaded["DB"]["port"].As<int>() == 3306);
  REQUIRE(loaded["DB"]["user"].As<std::string>() == "admin");
  REQUIRE(loaded["Cache"]["enabled"].As<bool>() == true);
  REQUIRE(loaded["Cache"]["ttl"].As<int>() == 300);

  RemoveTempFile();
}

TEST_CASE("load from file constructor", "FileIO") {
  WriteTempFile("[Test]\nval=42\n");

  osp::ini::IniFile inif(kTempFile);
  REQUIRE(inif["Test"]["val"].As<int>() == 42);

  RemoveTempFile();
}

TEST_CASE("load empty file", "FileIO") {
  WriteTempFile("");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.size() == 0);

  RemoveTempFile();
}

TEST_CASE("load file with only comments", "FileIO") {
  WriteTempFile("# this is a comment\n; another comment\n# more comments\n");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.size() == 0);

  RemoveTempFile();
}

TEST_CASE("load file with BOM-like leading whitespace", "FileIO") {
  WriteTempFile("  [Section]\n  key=value\n");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.find("Section") != inif.end());
  REQUIRE(inif["Section"]["key"].As<std::string>() == "value");

  RemoveTempFile();
}

TEST_CASE("save overwrites existing file", "FileIO") {
  // first write
  {
    osp::ini::IniFile inif;
    inif["Old"]["data"] = "old_value";
    inif.Save(kTempFile);
  }

  // second write (overwrite)
  {
    osp::ini::IniFile inif;
    inif["New"]["data"] = "new_value";
    inif.Save(kTempFile);
  }

  // verify only new content exists
  osp::ini::IniFile loaded;
  loaded.Load(kTempFile);
  REQUIRE(loaded.find("Old") == loaded.end());
  REQUIRE(loaded["New"]["data"].As<std::string>() == "new_value");

  RemoveTempFile();
}

TEST_CASE("load multiple times clears previous content", "FileIO") {
  WriteTempFile("[A]\nx=1\n");
  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.size() == 1);

  WriteTempFile("[B]\ny=2\n[C]\nz=3\n");
  inif.Load(kTempFile);
  REQUIRE(inif.size() == 2);
  REQUIRE(inif.find("A") == inif.end());
  REQUIRE(inif["B"]["y"].As<int>() == 2);

  RemoveTempFile();
}
