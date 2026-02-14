/*
 * test_inifile.cpp
 *
 * Created on: 26 Dec 2015
 *     Author: Fabian Meyer
 *    License: MIT
 */

#include "osp/inicpp.h"

#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <sstream>

TEST_CASE("parse ini file", "IniFile") {
  std::istringstream ss(("[Foo]\nbar=hello world\n[Test]"));
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello world");
  REQUIRE(inif["Test"].size() == 0);
}

TEST_CASE("parse empty file", "IniFile") {
  std::istringstream ss("");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 0);
}

TEST_CASE("parse comment only file", "IniFile") {
  std::istringstream ss("# this is a comment");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 0);
}

TEST_CASE("parse empty section", "IniFile") {
  std::istringstream ss("[Foo]");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 0);
}

TEST_CASE("parse empty field", "IniFile") {
  std::istringstream ss("[Foo]\nbar=");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "");
}

TEST_CASE("parse section with duplicate field", "IniFile") {
  std::istringstream ss("[Foo]\nbar=hello\nbar=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "world");
}

TEST_CASE("parse section with duplicate field and overwriteDuplicateFields_ set to true", "IniFile") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(true);
  inif.Decode("[Foo]\nbar=hello\nbar=world");

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "world");
}

TEST_CASE("parse field as bool", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=true\nbar2=false\nbar3=tRuE");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<bool>());
  REQUIRE_FALSE(inif["Foo"]["bar2"].As<bool>());
  REQUIRE(inif["Foo"]["bar3"].As<bool>());
}

TEST_CASE("parse field as char", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=c\nbar2=q");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<char>() == 'c');
  REQUIRE(inif["Foo"]["bar2"].As<char>() == 'q');
}

TEST_CASE("parse field as unsigned char", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=c\nbar2=q");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned char>() == 'c');
  REQUIRE(inif["Foo"]["bar2"].As<unsigned char>() == 'q');
}

TEST_CASE("parse field as short", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=-2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<short>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<short>() == -2);
}

TEST_CASE("parse field as unsigned short", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=13");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned short>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<unsigned short>() == 13);
}

TEST_CASE("parse field as int", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=-2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<int>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<int>() == -2);
}

TEST_CASE("parse field as unsigned int", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=13");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned int>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<unsigned int>() == 13);
}

TEST_CASE("parse field as long", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=-2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<long>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<long>() == -2);
}

TEST_CASE("parse field as unsigned long", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1\nbar2=13");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<unsigned long>() == 1);
  REQUIRE(inif["Foo"]["bar2"].As<unsigned long>() == 13);
}

TEST_CASE("parse field as double", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1.2\nbar2=1\nbar3=-2.4");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<double>() == Approx(1.2).margin(1e-3));
  REQUIRE(inif["Foo"]["bar2"].As<double>() == Approx(1.0).margin(1e-3));
  REQUIRE(inif["Foo"]["bar3"].As<double>() == Approx(-2.4).margin(1e-3));
}

TEST_CASE("parse field as float", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=1.2\nbar2=1\nbar3=-2.4");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 3);
  REQUIRE(inif["Foo"]["bar1"].As<float>() == Approx(1.2f).margin(1e-3f));
  REQUIRE(inif["Foo"]["bar2"].As<float>() == Approx(1.0f).margin(1e-3f));
  REQUIRE(inif["Foo"]["bar3"].As<float>() == Approx(-2.4f).margin(1e-3f));
}

TEST_CASE("parse field as std::string", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=hello\nbar2=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<std::string>() == "hello");
  REQUIRE(inif["Foo"]["bar2"].As<std::string>() == "world");
}

TEST_CASE("parse field as const char*", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=hello\nbar2=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(std::strcmp(inif["Foo"]["bar1"].As<const char*>(), "hello") == 0);
  REQUIRE(std::strcmp(inif["Foo"]["bar2"].As<const char*>(), "world") == 0);
}

#ifdef __cpp_lib_string_view
TEST_CASE("parse field as std::string_view", "IniFile") {
  std::istringstream ss("[Foo]\nbar1=hello\nbar2=world");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 2);
  REQUIRE(inif["Foo"]["bar1"].As<std::string_view>() == "hello");
  REQUIRE(inif["Foo"]["bar2"].As<std::string_view>() == "world");
}
#endif

TEST_CASE("parse field with custom field sep", "IniFile") {
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

TEST_CASE("parse with comment", "IniFile") {
  std::istringstream ss("[Foo]\n# this is a test\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("parse with custom comment char prefix", "IniFile") {
  std::istringstream ss("[Foo]\n$ this is a test\nbar=bla");
  osp::ini::IniFile inif;

  inif.SetFieldSep('=');
  inif.SetCommentChar('$');
  inif.Decode(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("parse with multi char comment prefix", "IniFile") {
  std::istringstream ss("[Foo]\nREM this is a test\nbar=bla");
  osp::ini::IniFile inif(ss, '=', {"REM"});

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("parse with multiple multi char comment prefixes", "IniFile") {
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

TEST_CASE("comment prefixes can be set after construction", "IniFile") {
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

TEST_CASE("comments are allowed after escaped comments", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "hello=world \\## this is a comment\n"
      "more=of this \\# \\#\n");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["hello"].As<std::string>() == "world #");
  REQUIRE(inif["Foo"]["more"].As<std::string>() == "of this # #");
}

TEST_CASE("escape char right before a comment prefix escapes all the comment prefix", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "weird1=note \\### this is not a comment\n"
      "weird2=but \\#### this is a comment");
  osp::ini::IniFile inif(ss, '=', {"##"});

  REQUIRE(inif["Foo"]["weird1"].As<std::string>() == "note ### this is not a comment");
  REQUIRE(inif["Foo"]["weird2"].As<std::string>() == "but ##");
}

TEST_CASE("escape comment when writing", "IniFile") {
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

TEST_CASE("decode what we encoded", "IniFile") {
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

TEST_CASE("save with bool fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = true;
  inif["Foo"]["bar2"] = false;

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=true\nbar2=false\n\n");
}

TEST_CASE("save with char fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<char>('c');
  inif["Foo"]["bar2"] = static_cast<char>('q');

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=c\nbar2=q\n\n");
}

TEST_CASE("save with unsigned char fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned char>('c');
  inif["Foo"]["bar2"] = static_cast<unsigned char>('q');

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=c\nbar2=q\n\n");
}

TEST_CASE("save with short fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<short>(1);
  inif["Foo"]["bar2"] = static_cast<short>(-2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=-2\n\n");
}

TEST_CASE("save with unsigned short fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned short>(1);
  inif["Foo"]["bar2"] = static_cast<unsigned short>(13);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=13\n\n");
}

TEST_CASE("save with int fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<int>(1);
  inif["Foo"]["bar2"] = static_cast<int>(-2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=-2\n\n");
}

TEST_CASE("save with unsigned int fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned int>(1);
  inif["Foo"]["bar2"] = static_cast<unsigned int>(13);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=13\n\n");
}

TEST_CASE("save with long fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<long>(1);
  inif["Foo"]["bar2"] = static_cast<long>(-2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=-2\n\n");
}

TEST_CASE("save with unsigned long fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<unsigned long>(1);
  inif["Foo"]["bar2"] = static_cast<unsigned long>(13);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1\nbar2=13\n\n");
}

TEST_CASE("save with double fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<double>(1.2);
  inif["Foo"]["bar2"] = static_cast<double>(-2.4);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1.2\nbar2=-2.4\n\n");
}

TEST_CASE("save with float fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<float>(1.2f);
  inif["Foo"]["bar2"] = static_cast<float>(-2.4f);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=1.2\nbar2=-2.4\n\n");
}

TEST_CASE("save with std::string fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<std::string>("hello");
  inif["Foo"]["bar2"] = static_cast<std::string>("world");

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

TEST_CASE("save with const char* fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = static_cast<const char*>("hello");
  inif["Foo"]["bar2"] = static_cast<const char*>("world");

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

TEST_CASE("save with char* fields", "IniFile") {
  osp::ini::IniFile inif;
  char bar1[6] = "hello";
  char bar2[6] = "world";
  inif["Foo"]["bar1"] = static_cast<char*>(bar1);
  inif["Foo"]["bar2"] = static_cast<char*>(bar2);

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

TEST_CASE("save with string literal fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = "hello";
  inif["Foo"]["bar2"] = "world";

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}

#ifdef __cpp_lib_string_view
TEST_CASE("save with std::string_view fields", "IniFile") {
  osp::ini::IniFile inif;
  inif["Foo"]["bar1"] = std::string_view("hello");
  inif["Foo"]["bar2"] = std::string_view("world");

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1=hello\nbar2=world\n\n");
}
#endif

TEST_CASE("save with custom field sep", "IniFile") {
  osp::ini::IniFile inif(':', '#');
  inif["Foo"]["bar1"] = true;
  inif["Foo"]["bar2"] = false;

  std::string result = inif.Encode();
  REQUIRE(result == "[Foo]\nbar1:true\nbar2:false\n\n");
}

TEST_CASE("inline comments in sections are discarded", "IniFile") {
  std::istringstream ss("[Foo] # This is an inline comment\nbar=Hello world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("Foo") != inif.end());
}

TEST_CASE("inline comments in fields are discarded", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello #world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello");
}

TEST_CASE("inline comments can be escaped", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello \\#world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello #world!");
}

TEST_CASE("escape characters are kept if not before a comment prefix", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello \\world!");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello \\world!");
}

TEST_CASE("multi-line values are read correctly with space indents", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello\n"
      "    world!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values are read correctly with tab indents", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello\n"
      "\tworld!");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values discard end-of-line comments", "IniFile") {
  std::istringstream ss(
      "[Foo]\n"
      "bar=Hello ; everyone\n"
      "    world! ; comment");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "Hello\nworld!");
}

TEST_CASE("multi-line values discard interspersed comment lines", "IniFile") {
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

TEST_CASE("multi-line values should not be parsed when disabled", "IniFile") {
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

TEST_CASE("multi-line values should be parsed when enabled, even when the continuation contains =", "IniFile") {
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

TEST_CASE("when multi-line values are enabled, write newlines as multi-line value continuations", "IniFile") {
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
    "StringInsensitiveLessFunctor") {
  osp::ini::StringInsensitiveLess cc;

  REQUIRE(cc("a", "b"));
  REQUIRE(cc("a", "B"));
  REQUIRE(cc("aaa", "aaB"));
}

TEST_CASE("stringInsensitiveLess operator() returns false when words differs only in case",
          "StringInsensitiveLessFunctor") {
  osp::ini::StringInsensitiveLess cc;

  REQUIRE(cc("AA", "aa") == false);
}

TEST_CASE("stringInsensitiveLess operator() has a case insensitive strict weak ordering policy",
          "StringInsensitiveLessFunctor") {
  osp::ini::StringInsensitiveLess cc;

  REQUIRE(cc("B", "a") == false);
}

TEST_CASE("default inifile parser is case sensitive", "IniFile") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("foo") == inif.end());
  REQUIRE(inif["FOO"].find("BAR") == inif["FOO"].end());
}

TEST_CASE("case insensitive inifile ignores case of section", "IniFile") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif.find("foo") != inif.end());
  REQUIRE(inif.find("FOO") != inif.end());
}

TEST_CASE("case insensitive inifile ignores case of field", "IniFile") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif["FOO"].find("BAR") != inif["FOO"].end());
}

TEST_CASE(".As<>() works with IniFileCaseInsensitive", "IniFile") {
  std::istringstream ss("[FOO]\nbar=bla");
  osp::ini::IniFileCaseInsensitive inif(ss);

  REQUIRE(inif["FOO"]["bar"].As<std::string>() == "bla");
}

TEST_CASE("trim() works with empty strings", "TrimFunction") {
  std::string example1 = "";
  std::string example2 = "  \t\n  ";

  osp::ini::Trim(example1);
  osp::ini::Trim(example2);

  REQUIRE(example1.size() == 0);
  REQUIRE(example2.size() == 0);
}

TEST_CASE("trim() works with already trimmed strings", "TrimFunction") {
  std::string example1 = "example_text";
  std::string example2 = "example  \t\n  text";

  osp::ini::Trim(example1);
  osp::ini::Trim(example2);

  REQUIRE(example1 == "example_text");
  REQUIRE(example2 == "example  \t\n  text");
}

TEST_CASE("trim() works with untrimmed strings", "TrimFunction") {
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

/***************************************************
 *                Failing Tests
 ***************************************************/

TEST_CASE("fail to load unclosed section", "IniFile") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Foo\nbar=bla"));
}

TEST_CASE("fail to load field without equal", "IniFile") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar"));
}

TEST_CASE("fail to parse a multi-line field without indentation (when enabled)", "IniFile") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar=Hello\nworld!"));
}

TEST_CASE("fail to parse a multi-line field without indentation (when disabled)", "IniFile") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(false);
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar=Hello\nworld!"));
}

TEST_CASE("fail to continue multi-line field without start (when enabled)", "IniFile") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  REQUIRE_THROWS(inif.Decode("[Foo]\n    world!\nbar=Hello"));
}

TEST_CASE("fail to continue multi-line field without start (when disabled)", "IniFile") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(false);
  REQUIRE_THROWS(inif.Decode("[Foo]\n    world!\nbar=Hello"));
}

TEST_CASE("fail to parse as bool", "IniFile") {
  std::istringstream ss("[Foo]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE_THROWS(inif["Foo"]["bar"].As<bool>());
}

TEST_CASE("fail to parse as int", "IniFile") {
  std::istringstream ss("[Foo]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE_THROWS(inif["Foo"]["bar"].As<int>());
}

TEST_CASE("fail to parse as double", "IniFile") {
  std::istringstream ss("[Foo]\nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE_THROWS(inif["Foo"]["bar"].As<double>());
}

TEST_CASE("fail to parse field without section", "IniFile") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("bar=bla"));
}

TEST_CASE("spaces are not taken into account in field names", "IniFile") {
  std::istringstream ss(("[Foo]\n  \t  bar  \t  =hello world"));
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"].find("bar") != inif["Foo"].end());
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello world");
}

TEST_CASE("spaces are not taken into account in field values", "IniFile") {
  std::istringstream ss(("[Foo]\nbar=  \t  hello world  \t  "));
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello world");
}

TEST_CASE("spaces are not taken into account in sections", "IniFile") {
  std::istringstream ss("  \t  [Foo]  \t  \nbar=bla");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("Foo") != inif.end());
}

TEST_CASE("parse section with duplicate field and overwriteDuplicateFields_ set to false", "IniFile") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(false);
  REQUIRE_THROWS(inif.Decode("[Foo]\nbar=hello\nbar=world"));
}
