/*
 * test_edge_cases.cpp
 *
 * Tests for edge cases, error handling, and unusual inputs.
 */

#include "osp/inicpp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <sstream>

/***************************************************
 *          Parsing Edge Cases
 ***************************************************/

TEST_CASE("parse field with equals sign in value", "EdgeCase") {
  std::istringstream ss("[Foo]\nurl=http://example.com?a=1&b=2");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["url"].As<std::string>() == "http://example.com?a=1&b=2");
}

TEST_CASE("parse field with only equals sign as value", "EdgeCase") {
  std::istringstream ss("[Foo]\nsep==");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["sep"].As<std::string>() == "=");
}

TEST_CASE("parse field with multiple equals signs", "EdgeCase") {
  std::istringstream ss("[Foo]\nexpr=a=b=c=d");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Foo"]["expr"].As<std::string>() == "a=b=c=d");
}

TEST_CASE("parse section with trailing content after bracket", "EdgeCase") {
  std::istringstream ss("[Foo] some trailing text\nbar=bla");
  osp::ini::IniFile inif(ss);

  // section name should be "Foo"
  REQUIRE(inif.find("Foo") != inif.end());
}

TEST_CASE("parse empty value field", "EdgeCase") {
  std::istringstream ss("[Sec]\nempty=\nnotempty=val");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"]["empty"].As<std::string>() == "");
  REQUIRE(inif["Sec"]["notempty"].As<std::string>() == "val");
}

TEST_CASE("parse value with leading and trailing spaces", "EdgeCase") {
  std::istringstream ss("[Sec]\nkey=  spaced value  ");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"]["key"].As<std::string>() == "spaced value");
}

TEST_CASE("parse key with leading and trailing spaces", "EdgeCase") {
  std::istringstream ss("[Sec]\n  spaced key  =value");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"].find("spaced key") != inif["Sec"].end());
  REQUIRE(inif["Sec"]["spaced key"].As<std::string>() == "value");
}

TEST_CASE("parse section name with leading and trailing spaces", "EdgeCase") {
  std::istringstream ss("  [MySection]  \nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("MySection") != inif.end());
}

TEST_CASE("parse windows-style line endings (CRLF)", "EdgeCase") {
  std::istringstream ss("[Foo]\r\nbar=hello\r\nbaz=world\r\n");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "hello");
  REQUIRE(inif["Foo"]["baz"].As<std::string>() == "world");
}

TEST_CASE("parse with blank lines between sections", "EdgeCase") {
  std::istringstream ss("[A]\nx=1\n\n\n[B]\ny=2\n\n");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.size() == 2);
  REQUIRE(inif["A"]["x"].As<int>() == 1);
  REQUIRE(inif["B"]["y"].As<int>() == 2);
}

TEST_CASE("parse with mixed comment styles", "EdgeCase") {
  std::istringstream ss("[Foo]\n# hash comment\n; semicolon comment\nbar=val");
  osp::ini::IniFile inif;
  inif.SetCommentPrefixes({"#", ";"});
  inif.Decode(ss);

  REQUIRE(inif["Foo"].size() == 1);
  REQUIRE(inif["Foo"]["bar"].As<std::string>() == "val");
}

TEST_CASE("numeric string as section name", "EdgeCase") {
  std::istringstream ss("[123]\nk=v");
  osp::ini::IniFile inif(ss);

  REQUIRE(inif.find("123") != inif.end());
}

TEST_CASE("very long field value", "EdgeCase") {
  std::string longVal(10000, 'x');
  std::string content = "[Sec]\nlong=" + longVal;
  std::istringstream ss(content);
  osp::ini::IniFile inif(ss);

  REQUIRE(inif["Sec"]["long"].As<std::string>() == longVal);
}

TEST_CASE("many sections", "EdgeCase") {
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

/***************************************************
 *          Error Handling
 ***************************************************/

TEST_CASE("fail to parse field before any section", "EdgeCase") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("orphan=value"));
}

TEST_CASE("fail to parse unclosed section bracket", "EdgeCase") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Unclosed\nk=v"));
}

TEST_CASE("fail to parse empty section name", "EdgeCase") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[]\nk=v"));
}

TEST_CASE("fail to parse line without separator in section", "EdgeCase") {
  osp::ini::IniFile inif;
  REQUIRE_THROWS(inif.Decode("[Sec]\nno_separator_here"));
}

TEST_CASE("duplicate field throws when disallowed", "EdgeCase") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(false);
  REQUIRE_THROWS(inif.Decode("[Sec]\nk=v1\nk=v2"));
}

TEST_CASE("duplicate field overwrites when allowed", "EdgeCase") {
  osp::ini::IniFile inif;
  inif.AllowOverwriteDuplicateFields(true);
  inif.Decode("[Sec]\nk=v1\nk=v2");
  REQUIRE(inif["Sec"]["k"].As<std::string>() == "v2");
}

/***************************************************
 *          Custom Separator
 ***************************************************/

TEST_CASE("custom field separator colon", "EdgeCase") {
  std::istringstream ss("[Sec]\nkey:value");
  osp::ini::IniFile inif;
  inif.SetFieldSep(':');
  inif.Decode(ss);

  REQUIRE(inif["Sec"]["key"].As<std::string>() == "value");
}

TEST_CASE("custom field separator in constructor", "EdgeCase") {
  std::istringstream ss("[Sec]\nkey:value");
  osp::ini::IniFile inif(':', '#');
  inif.Decode(ss);

  REQUIRE(inif["Sec"]["key"].As<std::string>() == "value");
}

TEST_CASE("encode with custom field separator", "EdgeCase") {
  osp::ini::IniFile inif(':', '#');
  inif["S"]["k"] = "v";

  std::string result = inif.Encode();
  REQUIRE(result.find("k:v") != std::string::npos);
}

/***************************************************
 *          Escape Character
 ***************************************************/

TEST_CASE("custom escape character", "EdgeCase") {
  osp::ini::IniFile inif;
  inif.SetEscapeChar('!');
  inif.SetCommentPrefixes({"#"});
  inif.Decode("[Sec]\nval=hello !# world");

  REQUIRE(inif["Sec"]["val"].As<std::string>() == "hello # world");
}

/***************************************************
 *          Multi-line Values
 ***************************************************/

TEST_CASE("multi-line value with multiple continuation lines", "EdgeCase") {
  std::istringstream ss("[Sec]\ntext=line1\n\tline2\n\tline3\n\tline4");
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif.Decode(ss);

  REQUIRE(inif["Sec"]["text"].As<std::string>() == "line1\nline2\nline3\nline4");
}

TEST_CASE("multi-line value encode produces continuation with tab", "EdgeCase") {
  osp::ini::IniFile inif;
  inif.SetMultiLineValues(true);
  inif["Sec"]["ml"] = "first\nsecond\nthird";

  std::string result = inif.Encode();
  REQUIRE(result.find("ml=first\n\tsecond\n\tthird") != std::string::npos);
}

TEST_CASE("multi-line roundtrip", "EdgeCase") {
  osp::ini::IniFile original;
  original.SetMultiLineValues(true);
  original["Config"]["desc"] = "line1\nline2\nline3";

  std::string encoded = original.Encode();

  osp::ini::IniFile decoded;
  decoded.SetMultiLineValues(true);
  decoded.Decode(encoded);

  REQUIRE(decoded["Config"]["desc"].As<std::string>() == "line1\nline2\nline3");
}

/***************************************************
 *          trim() Function Tests
 ***************************************************/

TEST_CASE("trim removes tabs and newlines", "Trim") {
  std::string s = "\t\n  hello  \n\t";
  osp::ini::Trim(s);
  REQUIRE(s == "hello");
}

TEST_CASE("trim preserves internal whitespace", "Trim") {
  std::string s = "  hello   world  ";
  osp::ini::Trim(s);
  REQUIRE(s == "hello   world");
}

TEST_CASE("trim on already trimmed string is no-op", "Trim") {
  std::string s = "clean";
  osp::ini::Trim(s);
  REQUIRE(s == "clean");
}

TEST_CASE("trim whitespace-only string becomes empty", "Trim") {
  std::string s = "   \t\n\r  ";
  osp::ini::Trim(s);
  REQUIRE(s.empty());
}

/***************************************************
 *          decode(string) overload
 ***************************************************/

TEST_CASE("decode from string directly", "EdgeCase") {
  osp::ini::IniFile inif;
  inif.Decode("[Direct]\nk=v");
  REQUIRE(inif["Direct"]["k"].As<std::string>() == "v");
}

TEST_CASE("decode clears previous content", "EdgeCase") {
  osp::ini::IniFile inif;
  inif.Decode("[First]\na=1");
  REQUIRE(inif.size() == 1);

  inif.Decode("[Second]\nb=2\n[Third]\nc=3");
  REQUIRE(inif.size() == 2);
  REQUIRE(inif.find("First") == inif.end());
}
