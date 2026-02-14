/**
 * @file test_inicpp.cpp
 * @brief Comprehensive tests for inicpp library (embedded ini parser).
 *
 * Ported from DeguiLiu/inifile-cpp test suite.
 * Original: https://github.com/DeguiLiu/inifile-cpp
 * Namespace adapted: ini:: -> osp::ini::
 */

#include "osp/inicpp.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using Catch::Approx;

// ============================================================================
// IniField Tests (from test_inifield.cpp)
// ============================================================================

TEST_CASE("IniField default constructor creates empty field", "[IniField]") {
  osp::ini::IniField field;
  REQUIRE(field.As<std::string>() == "");
}

TEST_CASE("IniField string constructor", "[IniField]") {
  osp::ini::IniField field("hello");
  REQUIRE(field.As<std::string>() == "hello");
}

TEST_CASE("IniField copy constructor", "[IniField]") {
  osp::ini::IniField original("world");
  osp::ini::IniField copy(original);
  REQUIRE(copy.As<std::string>() == "world");
}

TEST_CASE("IniField assignment from various types", "[IniField]") {
  osp::ini::IniField field;

  field = 42;
  REQUIRE(field.As<int>() == 42);

  field = true;
  REQUIRE(field.As<bool>() == true);

  field = 3.14;
  REQUIRE(field.As<double>() == Approx(3.14).margin(1e-3));

  field = std::string("test");
  REQUIRE(field.As<std::string>() == "test");

  field = "literal";
  REQUIRE(field.As<std::string>() == "literal");
}

TEST_CASE("IniField copy assignment", "[IniField]") {
  osp::ini::IniField a("source");
  osp::ini::IniField b;
  b = a;
  REQUIRE(b.As<std::string>() == "source");
}

TEST_CASE("IniField as<T> type conversion", "[IniField]") {
  osp::ini::IniField intField("123");
  REQUIRE(intField.As<int>() == 123);
  REQUIRE(intField.As<long>() == 123L);
  REQUIRE(intField.As<unsigned int>() == 123u);
  REQUIRE(intField.As<double>() == Approx(123.0));
  REQUIRE(intField.As<float>() == Approx(123.0f));

  osp::ini::IniField boolField("true");
  REQUIRE(boolField.As<bool>() == true);

  osp::ini::IniField charField("x");
  REQUIRE(charField.As<char>() == 'x');
}

TEST_CASE("IniField as<T> throws on invalid conversion", "[IniField]") {
  osp::ini::IniField field("not_a_number");
  REQUIRE_THROWS(field.As<int>());
  REQUIRE_THROWS(field.As<double>());
  REQUIRE_THROWS(field.As<bool>());
}

TEST_CASE("IniField overwrite preserves latest value", "[IniField]") {
  osp::ini::IniField field;
  field = 10;
  REQUIRE(field.As<int>() == 10);

  field = 20;
  REQUIRE(field.As<int>() == 20);

  field = "now a string";
  REQUIRE(field.As<std::string>() == "now a string");
}

// ============================================================================
// IniSection Tests (from test_inisection.cpp)
// ============================================================================

TEST_CASE("IniFile multiple sections", "[IniSection]") {
  std::istringstream ss("[Sec1]\na=1\n[Sec2]\nb=2\n[Sec3]\nc=3");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 3);
  REQUIRE(inif["Sec1"]["a"].As<int>() == 1);
  REQUIRE(inif["Sec2"]["b"].As<int>() == 2);
  REQUIRE(inif["Sec3"]["c"].As<int>() == 3);
}

TEST_CASE("IniFile multiple fields in one section", "[IniSection]") {
  std::istringstream ss("[Config]\nhost=localhost\nport=8080\nverbose=true\ntimeout=30.5");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Config"].size() == 4);
  REQUIRE(inif["Config"]["host"].As<std::string>() == "localhost");
  REQUIRE(inif["Config"]["port"].As<int>() == 8080);
  REQUIRE(inif["Config"]["verbose"].As<bool>() == true);
  REQUIRE(inif["Config"]["timeout"].As<double>() == Approx(30.5));
}

TEST_CASE("IniFile section with special characters in name", "[IniSection]") {
  std::istringstream ss("[Section With Spaces]\nkey=val\n[section-with-dashes]\nk=v\n[UPPERCASE]\nx=y");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 3);
  REQUIRE(inif.find("Section With Spaces") != inif.end());
  REQUIRE(inif.find("section-with-dashes") != inif.end());
  REQUIRE(inif.find("UPPERCASE") != inif.end());
}

TEST_CASE("IniFile create sections programmatically", "[IniSection]") {
  osp::ini::IniFile inif;
  inif["NewSection"]["key1"] = "value1";
  inif["NewSection"]["key2"] = 42;
  inif["Another"]["flag"] = true;

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["NewSection"]["key1"].As<std::string>() == "value1");
  REQUIRE(inif["NewSection"]["key2"].As<int>() == 42);
  REQUIRE(inif["Another"]["flag"].As<bool>() == true);
}

TEST_CASE("IniFile encode and decode roundtrip", "[IniSection]") {
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

TEST_CASE("IniFile section iteration", "[IniSection]") {
  std::istringstream ss("[A]\nx=1\n[B]\ny=2\n[C]\nz=3");
  osp::ini::IniFile inif(ss);

  int count = 0;
  for (const auto& section : inif) {
    REQUIRE(section.second.size() == 1);
    ++count;
  }
  REQUIRE(count == 3);
}

TEST_CASE("IniFile field iteration within section", "[IniSection]") {
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

TEST_CASE("IniFile find returns end for non-existent section", "[IniSection]") {
  std::istringstream ss("[Exists]\nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("Exists") != inif.end());
  REQUIRE(inif.find("DoesNotExist") == inif.end());
}

TEST_CASE("IniSection find returns end for non-existent field", "[IniSection]") {
  std::istringstream ss("[Sec]\nfoo=bar");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"].find("foo") != inif["Sec"].end());
  REQUIRE(inif["Sec"].find("baz") == inif["Sec"].end());
}

TEST_CASE("IniFile case insensitive sections and fields", "[IniSection]") {
  std::istringstream ss("[MySection]\nmyKey=myValue");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif.find("MYSECTION") != inif.end());
  REQUIRE(inif.find("mysection") != inif.end());
  REQUIRE(inif.find("MySection") != inif.end());

  REQUIRE(inif["mysection"].find("MYKEY") != inif["mysection"].end());
  REQUIRE(inif["mysection"]["mykey"].As<std::string>() == "myValue");
}

TEST_CASE("IniFile empty section followed by populated section", "[IniSection]") {
  std::istringstream ss("[Empty]\n[HasData]\nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["Empty"].size() == 0);
  REQUIRE(inif["HasData"].size() == 1);
}

// ============================================================================
// IniFile Tests (from test_inifile.cpp) - Part 1
// ============================================================================

TEST_CASE("parse ini file", "[IniFile]") {
  std::istringstream ss(("[Foo]\nbar=hello world\n[Test]"));
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello world");
  REQUIRE(inif["Test"].size() == 0);
}

TEST_CASE("parse empty file", "[IniFile]") {
  std::istringstream ss("");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 0);
}

TEST_CASE("parse comment only file", "[IniFile]") {
  std::istringstream ss("# this is a comment");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 0);
}

TEST_CASE("parse empty section", "[IniFile]") {
  std::istringstream ss("[Foo]");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 0);
}

TEST_CASE("parse empty field", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar=");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "");
}

TEST_CASE("parse section with duplicate field", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar=hello\nbar=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "world");
}

TEST_CASE("parse section with duplicate field and overwriteDuplicateFields_ set to true", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(true);
  inif.Decode("[Foo]\nbar=hello\nbar=world");

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "world");
}

TEST_CASE("parse field as bool", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=true\nbar2=false\nbar3=tRuE");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<bool>());
  REQUIRE_FALSE(inif["Foo"]["bar2"].As<bool>());
  REQUIRE(inif["Foo"]["bar3"].As<bool>());
}

TEST_CASE("parse field as char", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=c\nbar2=q");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<char>() == 'c');
  REQUIRE(inif["Foo"]["bar2"].As<char>() == 'q');
}

TEST_CASE("parse field as unsigned char", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=c\nbar2=q");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned char>() == 'c');
  REQUIRE(inif["Foo"]["bar2"].As<unsigned char>() == 'q');
}

TEST_CASE("parse field as short", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=-2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<short>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<short>() == -2);
}

TEST_CASE("parse field as unsigned short", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=13");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned short>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<unsigned short>() == 13);
}

TEST_CASE("parse field as int", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=-2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<int>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<int>() == -2);
}

TEST_CASE("parse field as unsigned int", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=13");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned int>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<unsigned int>() == 13);
}

TEST_CASE("parse field as long", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=-2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<long>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<long>() == -2);
}

TEST_CASE("parse field as unsigned long", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=13");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned long>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<unsigned long>() == 13);
}

TEST_CASE("parse field as double", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1.2\nbar2=1\nbar3=-2.4");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<double>() == Approx(1.2).margin(1e-3));
  REQUIRE(inif["Foo"]["bar2"].As<double>() == Approx(1.0).margin(1e-3));
  REQUIRE(inif["Foo"]["bar3"].As<double>() == Approx(-2.4).margin(1e-3));
}

TEST_CASE("parse field as float", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=1.2\nbar2=1\nbar3=-2.4");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<float>() == Approx(1.2f).margin(1e-3f));
  REQUIRE(inif["Foo"]["bar2"].As<float>() == Approx(1.0f).margin(1e-3f));
  REQUIRE(inif["Foo"]["bar3"].As<float>() == Approx(-2.4f).margin(1e-3f));
}

TEST_CASE("parse field as std::string", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=hello\nbar2=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<std::string>() == "hello");
  REQUIRE(inif["Foo"]["bar2"].As<std::string>() == "world");
}

TEST_CASE("parse field as const char*", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=hello\nbar2=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(std::strcmp(inif["Foo"]["bar1"].As<const char*>(), "hello") == 0);
  REQUIRE(std::strcmp(inif["Foo"]["bar2"].As<const char*>(), "world") == 0);
}

#ifdef __cpp_lib_string_view
TEST_CASE("parse field as std::string_view", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1=hello\nbar2=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<std::string_view>() == "hello");
  REQUIRE(inif["Foo"]["bar2"].As<std::string_view>() == "world");
}
#endif

TEST_CASE("parse field with custom field sep", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar1:true\nbar2:false\nbar3:tRuE");
  osp::ini::IniFile inif;

  inif.SetFieldSep(':');
  inif.Decode(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<bool>());
  REQUIRE_FALSE(inif["Foo"]["bar2"].As<bool>());
  REQUIRE(inif["Foo"]["bar3"].As<bool>());
}

TEST_CASE("parse with comment", "[IniFile]") {
  std::istringstream ss("[Foo]\n# this is a test\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("parse with custom comment char prefix", "[IniFile]") {
  std::istringstream ss("[Foo]\n$ this is a test\nbar=bla");
  osp::ini::IniFile inif;

  inif.SetFieldSep('=');
  inif.SetCommentChar('$');
  inif.Decode(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("parse with multi char comment prefix", "[IniFile]") {
  std::istringstream ss("[Foo]\nREM this is a test\nbar=bla");
  osp::ini::IniFile inif(ss, '=', {"REM"});

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("parse with multiple multi char comment prefixes", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "REM this is a comment\n"
      "#Also a comment\n"
      "//Even this is a comment\n"
      "bar=bla");
  osp::ini::IniFile inif(ss, '=', {"REM", "#", "//"});

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("comment prefixes can be set after construction", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "REM this is a comment\n"
      "#Also a comment\n"
      "//Even this is a comment\n"
      "bar=bla");
  osp::ini::IniFile inif;
  inif.SetCommentPrefixes({"REM", "#", "//"});
  inif.Decode(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("comments are allowed after escaped comments", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "hello=world \\## this is a comment\n"
      "more=of this \\# \\#\n");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["hello"].As<std::string>() == "world #");
  REQUIRE(inif["Foo"]["more"].As<std::string>() == "of this # #");
}

TEST_CASE("escape char right before a comment prefix escapes all the comment prefix", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "weird1=note \\### this is not a comment\n"
      "weird2=but \\#### this is a comment");
  osp::ini::IniFile inif(ss, '=', {"##"});

  REQUIRE(inif["Foo"]["weird1"].As<std::string>() == "note ### this is not a comment");
  REQUIRE(inif["Foo"]["weird2"].As<std::string>() == "but ##");
}

TEST_CASE("escape comment when writing", "[IniFile]") {
  osp::ini::IniFile inif('=', {"#"});

  inif["Fo#o"] = osp::ini::IniSection();
  inif["Fo#o"]["he#llo"] = "world";
  inif["Fo#o"]["world"] = "he#llo";

  std::string str = inif.Encode();

  REQUIRE(str ==
          "[Fo\\#o]\n"
          "he\\#llo=world\n"
          "world=he\\#llo\n\n");
}

TEST_CASE("decode what we encoded", "[IniFile]") {
  std::string content =
      "[Fo\\#o]\n"
      "he\\REMllo=worl\\REMd\n"
      "world=he\\//llo\n\n";

  osp::ini::IniFile inif('=', {"#", "REM", "//"});

  // decode the string
  inif.Decode(content);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif.find("Fo#o") != inif.end());
  REQUIRE(inif["Fo#o"].size() == 2);
  REQUIRE(inif["Fo#o"].find("heREMllo") != inif["Fo#o"].end());
  REQUIRE(inif["Fo#o"].find("world") != inif["Fo#o"].end());
  REQUIRE(inif["Fo#o"]["heREMllo"].As<std::string>() == "worlREMd");
  REQUIRE(inif["Fo#o"]["world"].As<std::string>() == "he//llo");

  auto actual = inif.Encode();

  REQUIRE(content == actual);

  inif.Decode(actual);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif.find("Fo#o") != inif.end());
  REQUIRE(inif["Fo#o"].size() == 2);
  REQUIRE(inif["Fo#o"].find("heREMllo") != inif["Fo#o"].end());
  REQUIRE(inif["Fo#o"].find("world") != inif["Fo#o"].end());
  REQUIRE(inif["Fo#o"]["heREMllo"].As<std::string>() == "worlREMd");
  REQUIRE(inif["Fo#o"]["world"].As<std::string>() == "he//llo");

  auto actual2 = inif.Encode();

  REQUIRE(content == actual2);
}

TEST_CASE("save with bool fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = true;
  inif["Foo"]["bar2"] = false;

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=true\nbar2=false\n\n");
}

TEST_CASE("save with char fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<char>('c');
  inif["Foo"]["bar2"] = static_cast<char>('q');

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=c\nbar2=q\n\n");
}

TEST_CASE("save with unsigned char fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned char>('c');
  inif["Foo"]["bar2"] = static_cast<unsigned char>('q');

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=c\nbar2=q\n\n");
}

TEST_CASE("save with short fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<short>(1);
  inif["Foo"]["bar2"] = static_cast<short>(-2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=-2\n\n");
}

TEST_CASE("save with unsigned short fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned short>(1);
  inif["Foo"]["bar2"] = static_cast<unsigned short>(13);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=13\n\n");
}

TEST_CASE("save with int fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<int>(1);
  inif["Foo"]["bar2"] = static_cast<int>(-2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=-2\n\n");
}

TEST_CASE("save with unsigned int fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned int>(1);
  inif["Foo"]["bar2"] = static_cast<unsigned int>(13);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=13\n\n");
}

TEST_CASE("save with long fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<long>(1);
  inif["Foo"]["bar2"] = static_cast<long>(-2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=-2\n\n");
}

TEST_CASE("save with unsigned long fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned long>(1);
  inif["Foo"]["bar2"] = static_cast<unsigned long>(13);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=13\n\n");
}

TEST_CASE("save with double fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<double>(1.2);
  inif["Foo"]["bar2"] = static_cast<double>(-2.4);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1.2\nbar2=-2.4\n\n");
}

TEST_CASE("save with float fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<float>(1.2f);
  inif["Foo"]["bar2"] = static_cast<float>(-2.4f);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1.2\nbar2=-2.4\n\n");
}

TEST_CASE("save with std::string fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<std::string>("hello");
  inif["Foo"]["bar2"] = static_cast<std::string>("world");

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

TEST_CASE("save with const char* fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<const char*>("hello");
  inif["Foo"]["bar2"] = static_cast<const char*>("world");

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

TEST_CASE("save with char* fields", "[IniFile]") {
  osp::ini::IniFile inif;
  char bar1[6] = "hello";
  char bar2[6] = "world";
  inif["Foo"]["bar1"] = static_cast<char*>(bar1);
  inif["Foo"]["bar2"] = static_cast<char*>(bar2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

TEST_CASE("save with string literal fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = "hello";
  inif["Foo"]["bar2"] = "world";

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

#ifdef __cpp_lib_string_view
TEST_CASE("save with std::string_view fields", "[IniFile]") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = std::string_view("hello");
  inif["Foo"]["bar2"] = std::string_view("world");

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}
#endif

TEST_CASE("save with custom field sep", "[IniFile]") {
  osp::ini::IniFile inif(':', '#');
  inif["Foo"]["bar1"] = true;
  inif["Foo"]["bar2"] = false;

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1:true\nbar2:false\n\n");
}

TEST_CASE("inline comments in sections are discarded", "[IniFile]") {
  std::istringstream ss("[Foo] # This is an inline comment\nbar=Hello world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("Foo") != inif.end());
}

TEST_CASE("inline comments in fields are discarded", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello #world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello");
}

TEST_CASE("inline comments can be escaped", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello \\#world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello #world!");
}

TEST_CASE("escape characters are kept if not before a comment prefix", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello \\world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello \\world!");
}

TEST_CASE("multi-line values are read correctly with space indents", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello\n"
      "    world!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values are read correctly with tab indents", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello\n"
      "\tworld!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values discard end-of-line comments", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello ; everyone\n"
      "    world! ; comment");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values discard interspersed comment lines", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello\n"
      "; everyone\n"
      "    world!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values should not be parsed when disabled", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "    bar=Hello\n"
      "    baz=world!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(false);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello");
  REQUIRE(inif["Foo"]["baz"].As<std::string>() == "world!");
}

TEST_CASE("multi-line values should be parsed when enabled, even when the continuation contains =", "[IniFile]") {
  std::istringstream ss(
      "[Foo]\n"
      "    bar=Hello\n"
      "    baz=world!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nbaz=world!");
  REQUIRE(inif["Foo"]["baz"].As<std::string>() == "");
}

TEST_CASE("when multi-line values are enabled, write newlines as multi-line value continuations", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);

  inif["Foo"] = osp::ini::IniSection();
  inif["Foo"]["bar"] = "Hello\nworld!";

  std::string str = inif.Encode();

  REQUIRE(str ==
          "[Foo]\n"
          "bar=Hello\n"
          "\tworld!\n\n");
}

TEST_CASE(
    "stringInsensitiveLess operator() returns true if and only if first parameter is less than the second "
    "ignoring sensitivity",
    "[StringInsensitiveLessFunctor]") {
  osp::ini::StringInsensitiveLess cc;

  REQUIRE(cc("a", "b"));
  REQUIRE(cc("a", "B"));
  REQUIRE(cc("aaa", "aaB"));
}

TEST_CASE("stringInsensitiveLess operator() returns false when words differs only in case",
          "[StringInsensitiveLessFunctor]") {
  osp::ini::StringInsensitiveLess cc;

  REQUIRE(cc("AA", "aa") == false);
}

TEST_CASE("stringInsensitiveLess operator() has a case insensitive strict weak ordering policy",
          "[StringInsensitiveLessFunctor]") {
  osp::ini::StringInsensitiveLess cc;

  REQUIRE(cc("B", "a") == false);
}

TEST_CASE("default inifile parser is case sensitive", "[IniFile]") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("foo") == inif.end());
  REQUIRE(inif["FOO"].find("BAR") == inif["FOO"].end());
}

TEST_CASE("case insensitive inifile ignores case of section", "[IniFile]") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif.find("foo") != inif.end());
  REQUIRE(inif.find("FOO") != inif.end());
}

TEST_CASE("case insensitive inifile ignores case of field", "[IniFile]") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif["FOO"].find("BAR") != inif["FOO"].end());
}

TEST_CASE(".As<>() works with IniFileCaseInsensitive", "[IniFile]") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif["FOO"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("trim() works with empty strings", "[TrimFunction]") {
  std::string example1 = "";
  std::string example2 = "  \t\n  ";

  osp::ini::Trim(example1);
  osp::ini::Trim(example2);

  REQUIRE(example1.size() == 0);
  REQUIRE(example2.size() == 0);
}

TEST_CASE("trim() works with already trimmed strings", "[TrimFunction]") {
  std::string example1 = "example_text";
  std::string example2 = "example  \t\n  text";

  osp::ini::Trim(example1);
  osp::ini::Trim(example2);

  REQUIRE(example1 == "example_text");
  REQUIRE(example2 == "example  \t\n  text");
}

TEST_CASE("trim() works with untrimmed strings", "[TrimFunction]") {
  std::string example1 = "example text      ";
  std::string example2 = "      example text";
  std::string example3 = "      example text      ";
  std::string example4 = "  \t\n  example  \t\n  text  \t\n  ";

  osp::ini::Trim(example1);
  osp::ini::Trim(example2);
  osp::ini::Trim(example3);
  osp::ini::Trim(example4);

  REQUIRE(example1 == "example text");
  REQUIRE(example2 == "example text");
  REQUIRE(example3 == "example text");
  REQUIRE(example4 == "example  \t\n  text");
}

// ============================================================================
// IniFile Tests - Failing Tests
// ============================================================================

TEST_CASE("fail to load unclosed section", "[IniFile]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Foo\nbar=bla"));
}

TEST_CASE("fail to load field without equal", "[IniFile]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar"));
}

TEST_CASE("fail to parse a multi-line field without indentation (when enabled)", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar=Hello\nworld!"));
}

TEST_CASE("fail to parse a multi-line field without indentation (when disabled)", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(false);
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar=Hello\nworld!"));
}

TEST_CASE("fail to continue multi-line field without start (when enabled)", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  REQUIRE_THROWS(inif.Decode("[Foo]\n    world!\nbar=Hello"));
}

TEST_CASE("fail to continue multi-line field without start (when disabled)", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(false);
  REQUIRE_THROWS(inif.Decode("[Foo]\n    world!\nbar=Hello"));
}

TEST_CASE("fail to parse as bool", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE_THROWS(inif["Foo"]["bar"].As<bool>());
}

TEST_CASE("fail to parse as int", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE_THROWS(inif["Foo"]["bar"].As<int>());
}

TEST_CASE("fail to parse as double", "[IniFile]") {
  std::istringstream ss("[Foo]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE_THROWS(inif["Foo"]["bar"].As<double>());
}

TEST_CASE("fail to parse field without section", "[IniFile]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("bar=bla"));
}

TEST_CASE("spaces are not taken into account in field names", "[IniFile]") {
  std::istringstream ss(("[Foo]\n  \t  bar  \t  =hello world"));
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"].find("bar") != inif["Foo"].end());
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello world");
}

TEST_CASE("spaces are not taken into account in field values", "[IniFile]") {
  std::istringstream ss(("[Foo]\nbar=  \t  hello world  \t  "));
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello world");
}

TEST_CASE("spaces are not taken into account in sections", "[IniFile]") {
  std::istringstream ss("  \t  [Foo]  \t  \nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("Foo") != inif.end());
}

TEST_CASE("parse section with duplicate field and overwriteDuplicateFields_ set to false", "[IniFile]") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(false);
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar=hello\nbar=world"));
}

// ============================================================================
// Convert Tests (from test_convert.cpp)
// ============================================================================

TEST_CASE("convert bool decode true variants", "[Convert]") {
  osp::ini::Convert<bool> conv;
  bool result = false;

  conv.Decode("true", result);
  REQUIRE(result == true);

  conv.Decode("TRUE", result);
  REQUIRE(result == true);

  conv.Decode("True", result);
  REQUIRE(result == true);

  conv.Decode("tRuE", result);
  REQUIRE(result == true);
}

TEST_CASE("convert bool decode false variants", "[Convert]") {
  osp::ini::Convert<bool> conv;
  bool result = true;

  conv.Decode("false", result);
  REQUIRE(result == false);

  conv.Decode("FALSE", result);
  REQUIRE(result == false);

  conv.Decode("False", result);
  REQUIRE(result == false);
}

TEST_CASE("convert bool decode invalid throws", "[Convert]") {
  osp::ini::Convert<bool> conv;
  bool result;

  REQUIRE_THROWS_AS(conv.Decode("yes", result), std::invalid_argument);
  REQUIRE_THROWS_AS(conv.Decode("no", result), std::invalid_argument);
  REQUIRE_THROWS_AS(conv.Decode("1", result), std::invalid_argument);
  REQUIRE_THROWS_AS(conv.Decode("0", result), std::invalid_argument);
  REQUIRE_THROWS_AS(conv.Decode("", result), std::invalid_argument);
}

TEST_CASE("convert bool encode", "[Convert]") {
  osp::ini::Convert<bool> conv;
  std::string result;

  conv.Encode(true, result);
  REQUIRE(result == "true");

  conv.Encode(false, result);
  REQUIRE(result == "false");
}

TEST_CASE("convert char decode", "[Convert]") {
  osp::ini::Convert<char> conv;
  char result;

  conv.Decode("a", result);
  REQUIRE(result == 'a');

  conv.Decode("Z", result);
  REQUIRE(result == 'Z');

  // multi-char string: takes first char
  conv.Decode("hello", result);
  REQUIRE(result == 'h');
}

TEST_CASE("convert char encode", "[Convert]") {
  osp::ini::Convert<char> conv;
  std::string result;

  conv.Encode('x', result);
  REQUIRE(result == "x");
}

TEST_CASE("convert unsigned char decode and encode", "[Convert]") {
  osp::ini::Convert<unsigned char> conv;
  unsigned char result;

  conv.Decode("A", result);
  REQUIRE(result == 'A');

  std::string encoded;
  conv.Encode('B', encoded);
  REQUIRE(encoded == "B");
}

TEST_CASE("convert int decode decimal", "[Convert]") {
  osp::ini::Convert<int> conv;
  int result;

  conv.Decode("42", result);
  REQUIRE(result == 42);

  conv.Decode("-100", result);
  REQUIRE(result == -100);

  conv.Decode("0", result);
  REQUIRE(result == 0);
}

TEST_CASE("convert int decode hex", "[Convert]") {
  osp::ini::Convert<int> conv;
  int result;

  conv.Decode("0xFF", result);
  REQUIRE(result == 255);

  conv.Decode("0x1A", result);
  REQUIRE(result == 26);
}

TEST_CASE("convert int decode octal prefix", "[Convert]") {
  osp::ini::Convert<int> conv;
  int result;

  // strToLong tries decimal first, so "010" parses as decimal 10
  conv.Decode("010", result);
  REQUIRE(result == 10);

  // "077" parses as decimal 77
  conv.Decode("077", result);
  REQUIRE(result == 77);
}

TEST_CASE("convert int decode invalid throws", "[Convert]") {
  osp::ini::Convert<int> conv;
  int result;

  // "abc" is valid hex (0xabc=2748), so it does NOT throw
  // "12.5" fails decimal/octal/hex due to the dot
  REQUIRE_THROWS_AS(conv.Decode("12.5", result), std::invalid_argument);
  REQUIRE_THROWS_AS(conv.Decode("xyz", result), std::invalid_argument);
}

TEST_CASE("convert int encode", "[Convert]") {
  osp::ini::Convert<int> conv;
  std::string result;

  conv.Encode(42, result);
  REQUIRE(result == "42");

  conv.Encode(-7, result);
  REQUIRE(result == "-7");
}

TEST_CASE("convert unsigned int decode and encode", "[Convert]") {
  osp::ini::Convert<unsigned int> conv;
  unsigned int result;

  conv.Decode("123", result);
  REQUIRE(result == 123);

  std::string encoded;
  conv.Encode(456u, encoded);
  REQUIRE(encoded == "456");
}

TEST_CASE("convert short decode and encode", "[Convert]") {
  osp::ini::Convert<short> conv;
  short result;

  conv.Decode("32000", result);
  REQUIRE(result == 32000);

  conv.Decode("-32000", result);
  REQUIRE(result == -32000);

  std::string encoded;
  conv.Encode(static_cast<short>(100), encoded);
  REQUIRE(encoded == "100");
}

TEST_CASE("convert unsigned short decode and encode", "[Convert]") {
  osp::ini::Convert<unsigned short> conv;
  unsigned short result;

  conv.Decode("65000", result);
  REQUIRE(result == 65000);

  std::string encoded;
  conv.Encode(static_cast<unsigned short>(200), encoded);
  REQUIRE(encoded == "200");
}

TEST_CASE("convert long decode and encode", "[Convert]") {
  osp::ini::Convert<long> conv;
  long result;

  conv.Decode("1000000", result);
  REQUIRE(result == 1000000L);

  conv.Decode("-999999", result);
  REQUIRE(result == -999999L);

  std::string encoded;
  conv.Encode(12345L, encoded);
  REQUIRE(encoded == "12345");
}

TEST_CASE("convert long decode invalid throws", "[Convert]") {
  osp::ini::Convert<long> conv;
  long result;

  REQUIRE_THROWS_AS(conv.Decode("not_a_number", result), std::invalid_argument);
}

TEST_CASE("convert unsigned long decode and encode", "[Convert]") {
  osp::ini::Convert<unsigned long> conv;
  unsigned long result;

  conv.Decode("4294967295", result);
  REQUIRE(result == 4294967295UL);

  std::string encoded;
  conv.Encode(9999UL, encoded);
  REQUIRE(encoded == "9999");
}

TEST_CASE("convert unsigned long decode invalid throws", "[Convert]") {
  osp::ini::Convert<unsigned long> conv;
  unsigned long result;

  REQUIRE_THROWS_AS(conv.Decode("xyz", result), std::invalid_argument);
}

TEST_CASE("convert double decode", "[Convert]") {
  osp::ini::Convert<double> conv;
  double result;

  conv.Decode("3.14159", result);
  REQUIRE(result == Approx(3.14159).margin(1e-5));

  conv.Decode("-0.001", result);
  REQUIRE(result == Approx(-0.001).margin(1e-6));

  conv.Decode("1e10", result);
  REQUIRE(result == Approx(1e10));

  conv.Decode("0", result);
  REQUIRE(result == Approx(0.0));
}

TEST_CASE("convert double decode invalid throws", "[Convert]") {
  osp::ini::Convert<double> conv;
  double result;

  REQUIRE_THROWS(conv.Decode("not_a_double", result));
}

TEST_CASE("convert double encode", "[Convert]") {
  osp::ini::Convert<double> conv;
  std::string result;

  conv.Encode(1.5, result);
  REQUIRE(result == "1.5");
}

TEST_CASE("convert float decode and encode", "[Convert]") {
  osp::ini::Convert<float> conv;
  float result;

  conv.Decode("2.5", result);
  REQUIRE(result == Approx(2.5f).margin(1e-3f));

  std::string encoded;
  conv.Encode(3.0f, encoded);
  REQUIRE(encoded == "3");
}

TEST_CASE("convert string decode and encode", "[Convert]") {
  osp::ini::Convert<std::string> conv;
  std::string result;

  conv.Decode("hello world", result);
  REQUIRE(result == "hello world");

  conv.Decode("", result);
  REQUIRE(result == "");

  std::string encoded;
  conv.Encode("test string", encoded);
  REQUIRE(encoded == "test string");
}

TEST_CASE("convert const char* decode and encode", "[Convert]") {
  osp::ini::Convert<const char*> conv;

  std::string encoded;
  conv.Encode("literal", encoded);
  REQUIRE(encoded == "literal");
}

#ifdef __cpp_lib_string_view
TEST_CASE("convert string_view decode and encode", "[Convert]") {
  osp::ini::Convert<std::string_view> conv;
  std::string_view result;
  std::string backing = "test_value";

  conv.Decode(backing, result);
  REQUIRE(result == "test_value");

  std::string encoded;
  conv.Encode(std::string_view("view_val"), encoded);
  REQUIRE(encoded == "view_val");
}
#endif

// ============================================================================
// Edge Cases Tests (from test_edge_cases.cpp)
// ============================================================================

TEST_CASE("parse field with equals sign in value", "[EdgeCase]") {
  std::istringstream ss("[Foo]\nurl=http://example.com?a=1&b=2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["url"].As<std::string>() == "http://example.com?a=1&b=2");
}

TEST_CASE("parse field with only equals sign as value", "[EdgeCase]") {
  std::istringstream ss("[Foo]\nsep==");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["sep"].As<std::string>() == "=");
}

TEST_CASE("parse field with multiple equals signs", "[EdgeCase]") {
  std::istringstream ss("[Foo]\nexpr=a=b=c=d");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["expr"].As<std::string>() == "a=b=c=d");
}

TEST_CASE("parse section with trailing content after bracket", "[EdgeCase]") {
  std::istringstream ss("[Foo] some trailing text\nbar=bla");
  osp::ini::IniFile inif(ss);

  // section name should be "Foo"
  REQUIRE(inif.find("Foo") != inif.end());
}

TEST_CASE("parse empty value field", "[EdgeCase]") {
  std::istringstream ss("[Sec]\nempty=\nnotempty=val");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"]["empty"].As<std::string>() == "");
  REQUIRE(inif["Sec"]["notempty"].As<std::string>() == "val");
}

TEST_CASE("parse value with leading and trailing spaces", "[EdgeCase]") {
  std::istringstream ss("[Sec]\nkey=  spaced value  ");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"]["key"].As<std::string>() == "spaced value");
}

TEST_CASE("parse key with leading and trailing spaces", "[EdgeCase]") {
  std::istringstream ss("[Sec]\n  spaced key  =value");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"].find("spaced key") != inif["Sec"].end());
  REQUIRE(inif["Sec"]["spaced key"].As<std::string>() == "value");
}

TEST_CASE("parse section name with leading and trailing spaces", "[EdgeCase]") {
  std::istringstream ss("  [MySection]  \nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("MySection") != inif.end());
}

TEST_CASE("parse windows-style line endings (CRLF)", "[EdgeCase]") {
  std::istringstream ss("[Foo]\r\nbar=hello\r\nbaz=world\r\n");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello");
  REQUIRE(inif["Foo"]["baz"].As<std::string>() == "world");
}

TEST_CASE("parse with blank lines between sections", "[EdgeCase]") {
  std::istringstream ss("[A]\nx=1\n\n\n[B]\ny=2\n\n");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["A"]["x"].As<int>() == 1);
  REQUIRE(inif["B"]["y"].As<int>() == 2);
}

TEST_CASE("parse with mixed comment styles", "[EdgeCase]") {
  std::istringstream ss("[Foo]\n# hash comment\n; semicolon comment\nbar=val");
  osp::ini::IniFile inif;
  inif.SetCommentPrefixes({"#", ";"});
  inif.Decode(ss);

  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "val");
}

TEST_CASE("numeric string as section name", "[EdgeCase]") {
  std::istringstream ss("[123]\nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("123") != inif.end());
}

TEST_CASE("very long field value", "[EdgeCase]") {
  std::string longVal(10000, 'x');
  std::string content = "[Sec]\nlong=" + longVal;
  std::istringstream ss(content);
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"]["long"].As<std::string>() == longVal);
}

TEST_CASE("many sections", "[EdgeCase]") {
  std::ostringstream builder;
  for (int i = 0; i < 100; ++i) {
    builder << "[Section" << i << "]\nkey" << i << "=val" << i << "\n";
  }

  osp::ini::IniFile inif;
  inif.Decode(builder.str());
  REQUIRE(inif.size() == 100);
  REQUIRE(inif["Section0"]["key0"].As<std::string>() == "val0");
  REQUIRE(inif["Section99"]["key99"].As<std::string>() == "val99");
}

TEST_CASE("fail to parse field before any section", "[EdgeCase]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("orphan=value"));
}

TEST_CASE("fail to parse unclosed section bracket", "[EdgeCase]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Unclosed\nk=v"));
}

TEST_CASE("fail to parse empty section name", "[EdgeCase]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[]\nk=v"));
}

TEST_CASE("fail to parse line without separator in section", "[EdgeCase]") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Sec]\nno_separator_here"));
}

TEST_CASE("duplicate field throws when disallowed", "[EdgeCase]") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(false);
  REQUIRE_THROWS(inif.Decode("[Sec]\nk=v1\nk=v2"));
}

TEST_CASE("duplicate field overwrites when allowed", "[EdgeCase]") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(true);
  inif.Decode("[Sec]\nk=v1\nk=v2");
  REQUIRE(inif["Sec"]["k"].As<std::string>() == "v2");
}

TEST_CASE("custom field separator colon", "[EdgeCase]") {
  std::istringstream ss("[Sec]\nkey:value");
  osp::ini::IniFile inif;
  inif.SetFieldSep(':');
  inif.Decode(ss);

  REQUIRE(inif["Sec"]["key"].As<std::string>() == "value");
}

TEST_CASE("custom field separator in constructor", "[EdgeCase]") {
  std::istringstream ss("[Sec]\nkey:value");
  osp::ini::IniFile inif(':', '#');
  inif.Decode(ss);

  REQUIRE(inif["Sec"]["key"].As<std::string>() == "value");
}

TEST_CASE("encode with custom field separator", "[EdgeCase]") {
  osp::ini::IniFile inif(':', '#');
  inif["S"]["k"] = "v";

  std::string result = inif.Encode();
  REQUIRE(result.find("k:v") != std::string::npos);
}

TEST_CASE("custom escape character", "[EdgeCase]") {
  osp::ini::IniFile inif;
  inif.SetEscapeChar('!');
  inif.SetCommentPrefixes({"#"});
  inif.Decode("[Sec]\nval=hello !# world");

  REQUIRE(inif["Sec"]["val"].As<std::string>() == "hello # world");
}

TEST_CASE("multi-line value with multiple continuation lines", "[EdgeCase]") {
  std::istringstream ss("[Sec]\ntext=line1\n\tline2\n\tline3\n\tline4");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Sec"]["text"].As<std::string>() == "line1\nline2\nline3\nline4");
}

TEST_CASE("multi-line value encode produces continuation with tab", "[EdgeCase]") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif["Sec"]["ml"] = "first\nsecond\nthird";

  std::string result = inif.Encode();
  REQUIRE(result.find("ml=first\n\tsecond\n\tthird") != std::string::npos);
}

TEST_CASE("multi-line roundtrip", "[EdgeCase]") {
  osp::ini::IniFile original;
  original.SetMultiLineValues(true);
  original["Config"]["desc"] = "line1\nline2\nline3";

  std::string encoded = original.Encode();

  osp::ini::IniFile decoded;
  decoded.SetMultiLineValues(true);
  decoded.Decode(encoded);

  REQUIRE(decoded["Config"]["desc"].As<std::string>() == "line1\nline2\nline3");
}

TEST_CASE("trim removes tabs and newlines", "[Trim]") {
  std::string s = "\t\n  hello  \n\t";
  osp::ini::Trim(s);
  REQUIRE(s == "hello");
}

TEST_CASE("trim preserves internal whitespace", "[Trim]") {
  std::string s = "  hello   world  ";
  osp::ini::Trim(s);
  REQUIRE(s == "hello   world");
}

TEST_CASE("trim on already trimmed string is no-op", "[Trim]") {
  std::string s = "clean";
  osp::ini::Trim(s);
  REQUIRE(s == "clean");
}

TEST_CASE("trim whitespace-only string becomes empty", "[Trim]") {
  std::string s = "   \t\n\r  ";
  osp::ini::Trim(s);
  REQUIRE(s.empty());
}

TEST_CASE("decode from string directly", "[EdgeCase]") {
  osp::ini::IniFile inif;
  inif.Decode("[Direct]\nk=v");
  REQUIRE(inif["Direct"]["k"].As<std::string>() == "v");
}

TEST_CASE("decode clears previous content", "[EdgeCase]") {
  osp::ini::IniFile inif;
  inif.Decode("[First]\na=1");
  REQUIRE(inif.size() == 1);

  inif.Decode("[Second]\nb=2\n[Third]\nc=3");
  REQUIRE(inif.size() == 2);
  REQUIRE(inif.find("First") == inif.end());
}

// ============================================================================
// File I/O Tests (from test_file_io.cpp)
// ============================================================================

static const char* kTempFile = "/tmp/test_inifile_tmp.ini";

static void WriteTempFile(const std::string& content) {
  std::ofstream ofs(kTempFile);
  ofs << content;
}

static void RemoveTempFile() {
  std::remove(kTempFile);
}

TEST_CASE("load ini file from disk", "[FileIO]") {
  WriteTempFile("[Server]\nhost=localhost\nport=9090\n");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Server"]["host"].As<std::string>() == "localhost");
  REQUIRE(inif["Server"]["port"].As<int>() == 9090);

  RemoveTempFile();
}

TEST_CASE("save ini file to disk", "[FileIO]") {
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

TEST_CASE("save and load roundtrip with multiple sections", "[FileIO]") {
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

TEST_CASE("load from file constructor", "[FileIO]") {
  WriteTempFile("[Test]\nval=42\n");

  osp::ini::IniFile inif(kTempFile);
  REQUIRE(inif["Test"]["val"].As<int>() == 42);

  RemoveTempFile();
}

TEST_CASE("load empty file", "[FileIO]") {
  WriteTempFile("");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.size() == 0);

  RemoveTempFile();
}

TEST_CASE("load file with only comments", "[FileIO]") {
  WriteTempFile("# this is a comment\n; another comment\n# more comments\n");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.size() == 0);

  RemoveTempFile();
}

TEST_CASE("load file with BOM-like leading whitespace", "[FileIO]") {
  WriteTempFile("  [Section]\n  key=value\n");

  osp::ini::IniFile inif;
  inif.Load(kTempFile);
  REQUIRE(inif.find("Section") != inif.end());
  REQUIRE(inif["Section"]["key"].As<std::string>() == "value");

  RemoveTempFile();
}

TEST_CASE("save overwrites existing file", "[FileIO]") {
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

TEST_CASE("load multiple times clears previous content", "[FileIO]") {
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
