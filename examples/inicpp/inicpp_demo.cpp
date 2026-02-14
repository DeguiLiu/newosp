/**
 * @file inicpp_demo.cpp
 * @brief Comprehensive demonstration of inicpp library features.
 *
 * This demo showcases:
 * - Creating and encoding INI files
 * - Decoding INI from string
 * - Loading and saving INI files
 * - Type conversions (int, float, bool, string)
 */

#include "osp/inicpp.h"
#include <iostream>

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

int main(int argc, char** argv) {
  std::cout << "INI C++ Library Demonstration" << std::endl;
  std::cout << "==============================" << std::endl;
  std::cout << std::endl;
  
  DemoEncode();
  DemoDecode();
  DemoFileIO();
  DemoIteration();
  
  std::cout << "All demonstrations completed successfully!" << std::endl;
  
  return 0;
}
