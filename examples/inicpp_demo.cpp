/**
 * @file inicpp_demo.cpp
 * @brief Comprehensive demonstration of inicpp library features.
 *
 * This demo showcases:
 * - Creating and encoding INI files
 * - Decoding INI from string
 * - Loading and saving INI files
 * - Type conversions (int, float, bool, string)
 * - Section/field iteration
 * - Custom type conversion (std::vector)
 */

#include "osp/inicpp.hpp"
#include <iostream>
#include <sstream>

// Custom type conversion for std::vector
// The conversion functor must live in the "osp::ini" namespace
namespace osp {
namespace ini {
/** Conversion functor to parse std::vectors from an ini field.
 * The generic template can be passed down to the vector. */
template <typename T>
struct Convert<std::vector<T>> {
  /** Decodes a std::vector from a string. */
  void Decode(const std::string& value, std::vector<T>& result) {
    result.clear();

    // variable to store the decoded value of each element
    T decoded;
    // maintain a start and end pos within the string
    size_t startPos = 0;
    size_t endPos = 0;
    size_t cnt;

    while (endPos != std::string::npos) {
      if (endPos != 0)
        startPos = endPos + 1;
      // search for the next comma as separator
      endPos = value.find(',', startPos);

      // if no comma was found use the rest of the string as input
      if (endPos == std::string::npos)
        cnt = value.size() - startPos;
      else
        cnt = endPos - startPos;

      std::string tmp = value.substr(startPos, cnt);
      // use the conversion functor for the type contained in
      // the vector, so the vector can use any type that
      // is compatible with inifile-cpp
      Convert<T> conv;
      conv.Decode(tmp, decoded);
      result.push_back(decoded);
    }
  }

  /** Encodes a std::vector to a string. */
  void Encode(const std::vector<T>& value, std::string& result) {
    // variable to store the encoded element value
    std::string encoded;
    // string stream to build the result stream
    std::stringstream ss;
    for (size_t i = 0; i < value.size(); ++i) {
      // use the conversion functor for the type contained in
      // the vector, so the vector can use any type that
      // is compatible with inifile-cpp
      Convert<T> conv;
      conv.Encode(value[i], encoded);
      ss << encoded;

      // if this is not the last element add a comma as separator
      if (i != value.size() - 1)
        ss << ',';
    }
    // store the created string in the result
    result = ss.str();
  }
};
}  // namespace ini
}  // namespace osp

static void DemoEncode() {
  std::cout << "=== Demo 1: Creating and Encoding INI ===" << std::endl;

  osp::ini::IniFile ini;

  // Populate INI file with various types
  ini["Server"]["host"] = "127.0.0.1";
  ini["Server"]["port"] = 8080;
  ini["Server"]["timeout"] = 30.5f;
  ini["Server"]["debug"] = true;

  ini["Database"]["name"] = "mydb";
  ini["Database"]["user"] = "admin";
  ini["Database"]["max_connections"] = 100;

  // Encode to string
  std::string encoded = ini.Encode();
  std::cout << "Encoded INI file:" << std::endl;
  std::cout << encoded << std::endl;
}

static void DemoDecode() {
  std::cout << "=== Demo 2: Decoding INI from String ===" << std::endl;

  std::string content =
      "[Application]\n"
      "name=MyApp\n"
      "version=1.2.3\n"
      "enabled=true\n"
      "[Performance]\n"
      "threads=4\n"
      "cache_size=1024\n";

  osp::ini::IniFile ini;
  ini.Decode(content);

  // Access and display parsed values
  std::cout << "Application name: " << ini["Application"]["name"].As<std::string>() << std::endl;
  std::cout << "Application version: " << ini["Application"]["version"].As<std::string>() << std::endl;
  std::cout << "Application enabled: " << (ini["Application"]["enabled"].As<bool>() ? "yes" : "no") << std::endl;
  std::cout << "Performance threads: " << ini["Performance"]["threads"].As<int>() << std::endl;
  std::cout << "Performance cache_size: " << ini["Performance"]["cache_size"].As<int>() << std::endl;
  std::cout << std::endl;
}

static void DemoFileIO() {
  std::cout << "=== Demo 3: File Load/Save Operations ===" << std::endl;

  const char* filename = "/tmp/inicpp_demo.ini";

  // Create and save
  osp::ini::IniFile ini;
  ini["System"]["os"] = "Linux";
  ini["System"]["architecture"] = "x86_64";
  ini["System"]["cores"] = 8;
  ini["Features"]["async"] = true;
  ini["Features"]["realtime"] = true;

  ini.Save(filename);
  std::cout << "Saved INI file to: " << filename << std::endl;

  // Load and verify
  osp::ini::IniFile loaded;
  loaded.Load(filename);

  std::cout << "Loaded INI file contents:" << std::endl;
  std::cout << "  OS: " << loaded["System"]["os"].As<std::string>() << std::endl;
  std::cout << "  Architecture: " << loaded["System"]["architecture"].As<std::string>() << std::endl;
  std::cout << "  Cores: " << loaded["System"]["cores"].As<int>() << std::endl;
  std::cout << "  Async enabled: " << (loaded["Features"]["async"].As<bool>() ? "yes" : "no") << std::endl;
  std::cout << std::endl;
}

static void DemoIteration() {
  std::cout << "=== Demo 4: Iterating Through Sections and Fields ===" << std::endl;

  osp::ini::IniFile ini;
  ini["Network"]["interface"] = "eth0";
  ini["Network"]["mtu"] = 1500;
  ini["Logging"]["level"] = "INFO";
  ini["Logging"]["file"] = "/var/log/app.log";

  std::cout << "INI file has " << ini.size() << " sections" << std::endl;

  for (const auto& sectionPair : ini) {
    const std::string& sectionName = sectionPair.first;
    const osp::ini::IniSection& section = sectionPair.second;

    std::cout << "Section [" << sectionName << "] has " << section.size() << " fields:" << std::endl;

    for (const auto& fieldPair : section) {
      const std::string& fieldName = fieldPair.first;
      const osp::ini::IniField& field = fieldPair.second;
      std::cout << "  " << fieldName << " = " << field.As<std::string>() << std::endl;
    }
  }
  std::cout << std::endl;
}

static void DemoCustomTypeConversion() {
  std::cout << "=== Demo 5: Custom Type Conversion (std::vector) ===" << std::endl;

  // create some ini content that we can parse
  std::string content = "[Foo]\nintList=1,2,3,4,5,6,7,8\ndoubleList=3.4,1.2,2.2,4.7";

  // decode the ini contents
  osp::ini::IniFile inputIni;
  inputIni.Decode(content);

  // print the results
  std::cout << "Parsed ini file with vector fields:" << std::endl;

  // parse the int list
  std::vector<int> intList = inputIni["Foo"]["intList"].As<std::vector<int>>();
  std::cout << "int list:" << std::endl;
  for (size_t i = 0; i < intList.size(); ++i)
    std::cout << "  " << intList[i] << std::endl;

  // parse the double list
  std::vector<double> doubleList = inputIni["Foo"]["doubleList"].As<std::vector<double>>();
  std::cout << "double list:" << std::endl;
  for (size_t i = 0; i < doubleList.size(); ++i)
    std::cout << "  " << doubleList[i] << std::endl;

  std::cout << std::endl;

  // create another ini file for encoding
  osp::ini::IniFile outputIni;
  outputIni["Bar"]["floatList"] = std::vector<float>{1.0f, 9.3f, 3.256f};
  outputIni["Bar"]["boolList"] = std::vector<bool>{true, false, false, true};

  std::cout << "Encoded ini file with vector fields:" << std::endl;
  std::cout << outputIni.Encode() << std::endl;
}

int main(int argc, char** argv) {
  std::cout << "INI C++ Library Demonstration" << std::endl;
  std::cout << "==============================" << std::endl;
  std::cout << std::endl;

  DemoEncode();
  DemoDecode();
  DemoFileIO();
  DemoIteration();
  DemoCustomTypeConversion();

  std::cout << "All demonstrations completed successfully!" << std::endl;

  return 0;
}
