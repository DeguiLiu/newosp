/**
 * @file config.hpp
 * @brief Header-only configuration parser with INI backend.
 *
 * Stack-allocated key-value store with fixed-capacity entry table.
 * INI parsing powered by inih library (FetchContent'd by CMake).
 * JSON and YAML backends are reserved for future implementation.
 * Compatible with -fno-exceptions -fno-rtti.
 */

#ifndef OSP_CONFIG_HPP_
#define OSP_CONFIG_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef OSP_CONFIG_INI_ENABLED
#include "ini.h"  // inih library header (provided via FetchContent)
#endif

namespace osp {

// ============================================================================
// ConfigFormat - Supported configuration file formats
// ============================================================================

/**
 * @brief Enumeration of supported configuration file formats.
 *
 * kAuto attempts to detect the format from the file extension.
 */
enum class ConfigFormat : uint8_t {
  kAuto = 0,  ///< Detect from file extension
  kIni,       ///< INI format (requires OSP_CONFIG_INI_ENABLED)
  kJson,      ///< JSON format (requires OSP_CONFIG_JSON_ENABLED)
  kYaml,      ///< YAML format (requires OSP_CONFIG_YAML_ENABLED)
};

// ============================================================================
// Config - Fixed-capacity configuration parser
// ============================================================================

/**
 * @brief Header-only configuration parser with fixed-capacity storage.
 *
 * Stores up to kMaxEntries key-value pairs organized by [section].
 * All storage is stack-allocated within the Config object itself.
 *
 * Usage:
 * @code
 *   osp::Config cfg;
 *   auto result = cfg.LoadFile("/etc/myapp.ini");
 *   if (result.has_value()) {
 *     int32_t port = cfg.GetInt("network", "port", 8080);
 *     const char* host = cfg.GetString("network", "host", "0.0.0.0");
 *   }
 * @endcode
 */
class Config final {
 public:
  Config() = default;

  // --------------------------------------------------------------------------
  // Loading
  // --------------------------------------------------------------------------

  /**
   * @brief Load configuration from a file on disk.
   *
   * @param path     Null-terminated file path
   * @param format   File format; kAuto detects from extension
   * @return success or ConfigError
   */
  expected<void, ConfigError> LoadFile(const char* path,
                                       ConfigFormat format = ConfigFormat::kAuto) {
    OSP_ASSERT(path != nullptr);

    if (format == ConfigFormat::kAuto) {
      format = DetectFormat(path);
    }

    switch (format) {
#ifdef OSP_CONFIG_INI_ENABLED
      case ConfigFormat::kIni:
        return LoadFileIni(path);
#endif
      case ConfigFormat::kJson:
        return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
      case ConfigFormat::kYaml:
        return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
      default:
        return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
    }
  }

  /**
   * @brief Load configuration from an in-memory buffer.
   *
   * @param data     Pointer to buffer (must be null-terminated for INI)
   * @param size     Size of the buffer in bytes (excluding null terminator)
   * @param format   File format (kAuto is not supported here; specify explicitly)
   * @return success or ConfigError
   */
  expected<void, ConfigError> LoadBuffer(const char* data, uint32_t size,
                                         ConfigFormat format) {
    OSP_ASSERT(data != nullptr);
    (void)size;  // INI parser relies on null-terminated string

    switch (format) {
#ifdef OSP_CONFIG_INI_ENABLED
      case ConfigFormat::kIni:
        return LoadBufferIni(data);
#endif
      case ConfigFormat::kJson:
        return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
      case ConfigFormat::kYaml:
        return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
      default:
        return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
    }
  }

  // --------------------------------------------------------------------------
  // Typed Getters (with defaults)
  // --------------------------------------------------------------------------

  /**
   * @brief Get a string value from the configuration.
   *
   * @param section     Section name (use "" for global/root)
   * @param key         Key name
   * @param default_val Returned if key is not found
   * @return Pointer to the stored value, or default_val
   */
  const char* GetString(const char* section, const char* key,
                        const char* default_val = "") const {
    const Entry* e = FindEntry(section, key);
    return (e != nullptr) ? e->value : default_val;
  }

  /**
   * @brief Get an integer value from the configuration.
   *
   * Parses the stored string with strtol (base 10).
   *
   * @param section     Section name
   * @param key         Key name
   * @param default_val Returned if key is not found or parse fails
   * @return Parsed int32_t value
   */
  int32_t GetInt(const char* section, const char* key,
                 int32_t default_val = 0) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) {
      return default_val;
    }
    char* end = nullptr;
    long val = std::strtol(e->value, &end, 10);
    if (end == e->value) {
      return default_val;  // no digits parsed
    }
    return static_cast<int32_t>(val);
  }

  /**
   * @brief Get a port number (uint16_t) from the configuration.
   *
   * Clamps the parsed integer to the [0, 65535] range.
   *
   * @param section     Section name
   * @param key         Key name
   * @param default_val Returned if key is not found
   * @return Parsed and clamped uint16_t value
   */
  uint16_t GetPort(const char* section, const char* key,
                   uint16_t default_val = 0) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) {
      return default_val;
    }
    char* end = nullptr;
    long val = std::strtol(e->value, &end, 10);
    if (end == e->value) {
      return default_val;
    }
    if (val < 0) {
      return 0;
    }
    if (val > 65535) {
      return 65535;
    }
    return static_cast<uint16_t>(val);
  }

  /**
   * @brief Get a boolean value from the configuration.
   *
   * Recognizes (case-insensitive): "true", "1", "yes", "on" as true.
   * Everything else (including missing key) returns default_val.
   *
   * @param section     Section name
   * @param key         Key name
   * @param default_val Returned if key is not found
   * @return Parsed boolean value
   */
  bool GetBool(const char* section, const char* key,
               bool default_val = false) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) {
      return default_val;
    }
    return ParseBool(e->value);
  }

  /**
   * @brief Get a double-precision floating-point value from the configuration.
   *
   * @param section     Section name
   * @param key         Key name
   * @param default_val Returned if key is not found or parse fails
   * @return Parsed double value
   */
  double GetDouble(const char* section, const char* key,
                   double default_val = 0.0) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) {
      return default_val;
    }
    char* end = nullptr;
    double val = std::strtod(e->value, &end);
    if (end == e->value) {
      return default_val;
    }
    return val;
  }

  // --------------------------------------------------------------------------
  // Optional Getters (return empty optional if not found)
  // --------------------------------------------------------------------------

  /**
   * @brief Find an integer value, returning empty optional if not present.
   */
  optional<int32_t> FindInt(const char* section, const char* key) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) {
      return optional<int32_t>{};
    }
    char* end = nullptr;
    long val = std::strtol(e->value, &end, 10);
    if (end == e->value) {
      return optional<int32_t>{};
    }
    return optional<int32_t>{static_cast<int32_t>(val)};
  }

  /**
   * @brief Find a boolean value, returning empty optional if not present.
   */
  optional<bool> FindBool(const char* section, const char* key) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) {
      return optional<bool>{};
    }
    return optional<bool>{ParseBool(e->value)};
  }

  // --------------------------------------------------------------------------
  // Query
  // --------------------------------------------------------------------------

  /**
   * @brief Check whether a section exists in the loaded configuration.
   */
  bool HasSection(const char* section) const {
    OSP_ASSERT(section != nullptr);
    for (uint32_t i = 0; i < count_; ++i) {
      if (StrCaseEqual(entries_[i].section, section)) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Check whether a specific key exists in the given section.
   */
  bool HasKey(const char* section, const char* key) const {
    return FindEntry(section, key) != nullptr;
  }

 private:
  // --------------------------------------------------------------------------
  // Storage Constants and Entry Structure
  // --------------------------------------------------------------------------

  static constexpr uint32_t kMaxEntries = 128;   ///< Max key-value pairs
  static constexpr uint32_t kMaxKeyLen = 64;      ///< Max section/key length
  static constexpr uint32_t kMaxValueLen = 256;   ///< Max value length

  struct Entry {
    char section[kMaxKeyLen];
    char key[kMaxKeyLen];
    char value[kMaxValueLen];
  };

  Entry entries_[kMaxEntries];
  uint32_t count_ = 0;

  // --------------------------------------------------------------------------
  // Format Detection
  // --------------------------------------------------------------------------

  /**
   * @brief Detect configuration format from file extension.
   *
   * @param path  File path to inspect
   * @return Detected ConfigFormat, or kIni as fallback
   */
  static ConfigFormat DetectFormat(const char* path) noexcept {
    const char* dot = nullptr;
    for (const char* p = path; *p != '\0'; ++p) {
      if (*p == '.') {
        dot = p;
      }
    }
    if (dot == nullptr) {
      return ConfigFormat::kIni;  // default fallback
    }
    ++dot;  // skip the '.'
    if (StrCaseEqual(dot, "ini") || StrCaseEqual(dot, "cfg") ||
        StrCaseEqual(dot, "conf")) {
      return ConfigFormat::kIni;
    }
    if (StrCaseEqual(dot, "json")) {
      return ConfigFormat::kJson;
    }
    if (StrCaseEqual(dot, "yaml") || StrCaseEqual(dot, "yml")) {
      return ConfigFormat::kYaml;
    }
    return ConfigFormat::kIni;  // default fallback
  }

  // --------------------------------------------------------------------------
  // Entry Lookup
  // --------------------------------------------------------------------------

  /**
   * @brief Linear scan for an entry matching section and key.
   *
   * @param section  Section name (case-insensitive comparison)
   * @param key      Key name (case-insensitive comparison)
   * @return Pointer to matching entry, or nullptr
   */
  const Entry* FindEntry(const char* section, const char* key) const {
    OSP_ASSERT(section != nullptr);
    OSP_ASSERT(key != nullptr);
    for (uint32_t i = 0; i < count_; ++i) {
      if (StrCaseEqual(entries_[i].section, section) &&
          StrCaseEqual(entries_[i].key, key)) {
        return &entries_[i];
      }
    }
    return nullptr;
  }

  // --------------------------------------------------------------------------
  // Entry Insertion
  // --------------------------------------------------------------------------

  /**
   * @brief Add or update an entry in the table.
   *
   * Performs a linear scan for an existing section+key pair.
   * If found, the value is overwritten. If not found, a new entry is appended.
   *
   * @param section  Section name (truncated to kMaxKeyLen - 1)
   * @param key      Key name (truncated to kMaxKeyLen - 1)
   * @param value    Value string (truncated to kMaxValueLen - 1)
   * @return true on success, false if the table is full
   */
  bool AddEntry(const char* section, const char* key, const char* value) {
    // Look for existing entry to update
    for (uint32_t i = 0; i < count_; ++i) {
      if (StrCaseEqual(entries_[i].section, section) &&
          StrCaseEqual(entries_[i].key, key)) {
        SafeCopy(entries_[i].value, value, kMaxValueLen);
        return true;
      }
    }

    // Append new entry
    if (count_ >= kMaxEntries) {
      return false;
    }

    Entry& e = entries_[count_];
    SafeCopy(e.section, section, kMaxKeyLen);
    SafeCopy(e.key, key, kMaxKeyLen);
    SafeCopy(e.value, value, kMaxValueLen);
    ++count_;
    return true;
  }

  // --------------------------------------------------------------------------
  // String Utilities
  // --------------------------------------------------------------------------

  /**
   * @brief Case-insensitive string equality check.
   */
  static bool StrCaseEqual(const char* a, const char* b) noexcept {
    while (*a != '\0' && *b != '\0') {
      if (ToLower(static_cast<unsigned char>(*a)) !=
          ToLower(static_cast<unsigned char>(*b))) {
        return false;
      }
      ++a;
      ++b;
    }
    return *a == *b;  // both must be '\0'
  }

  /**
   * @brief ASCII lowercase conversion (avoids locale-dependent tolower).
   */
  static char ToLower(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A'))
                                  : static_cast<char>(c);
  }

  /**
   * @brief Safe string copy with truncation and null termination.
   *
   * @param dst      Destination buffer
   * @param src      Source string
   * @param dst_size Total size of the destination buffer
   */
  static void SafeCopy(char* dst, const char* src, uint32_t dst_size) noexcept {
    if (src == nullptr) {
      dst[0] = '\0';
      return;
    }
    uint32_t i = 0;
    while (i < (dst_size - 1U) && src[i] != '\0') {
      dst[i] = src[i];
      ++i;
    }
    dst[i] = '\0';
  }

  /**
   * @brief Parse a string as boolean (case-insensitive).
   *
   * Recognizes "true", "1", "yes", "on" as true; everything else as false.
   */
  static bool ParseBool(const char* str) noexcept {
    if (str == nullptr) {
      return false;
    }
    return StrCaseEqual(str, "true") || StrCaseEqual(str, "1") ||
           StrCaseEqual(str, "yes") || StrCaseEqual(str, "on");
  }

  // --------------------------------------------------------------------------
  // INI Backend
  // --------------------------------------------------------------------------

#ifdef OSP_CONFIG_INI_ENABLED

  /**
   * @brief inih callback for ini_parse / ini_parse_string.
   *
   * Signature required by inih:
   *   int handler(void* user, const char* section, const char* name,
   *               const char* value)
   *
   * @return 1 on success, 0 to signal parse error (stops parsing)
   */
  static int IniHandler(void* user, const char* section, const char* name,
                        const char* value) {
    Config* self = static_cast<Config*>(user);
    if (!self->AddEntry(section != nullptr ? section : "",
                        name != nullptr ? name : "",
                        value != nullptr ? value : "")) {
      return 0;  // entry table full, signal error
    }
    return 1;
  }

  /**
   * @brief Load INI file directly via inih's ini_parse.
   */
  expected<void, ConfigError> LoadFileIni(const char* path) {
    int result = ini_parse(path, IniHandler, this);
    if (result == -1) {
      return expected<void, ConfigError>::error(ConfigError::kFileNotFound);
    }
    if (result != 0) {
      return expected<void, ConfigError>::error(ConfigError::kParseError);
    }
    return expected<void, ConfigError>::success();
  }

  /**
   * @brief Load INI data from a null-terminated buffer via inih's
   *        ini_parse_string.
   */
  expected<void, ConfigError> LoadBufferIni(const char* data) {
    int result = ini_parse_string(data, IniHandler, this);
    if (result != 0) {
      return expected<void, ConfigError>::error(ConfigError::kParseError);
    }
    return expected<void, ConfigError>::success();
  }

#endif  // OSP_CONFIG_INI_ENABLED

  // --------------------------------------------------------------------------
  // JSON Backend (placeholder)
  // --------------------------------------------------------------------------

#ifdef OSP_CONFIG_JSON_ENABLED
  // TODO: JSON backend implementation
  // Will use a lightweight JSON parser (e.g., cJSON or sajson).
  // The CMake target will define OSP_CONFIG_JSON_ENABLED when
  // the OSP_CONFIG_JSON option is ON and a suitable JSON library
  // is available via FetchContent.
#endif  // OSP_CONFIG_JSON_ENABLED

  // --------------------------------------------------------------------------
  // YAML Backend (placeholder)
  // --------------------------------------------------------------------------

#ifdef OSP_CONFIG_YAML_ENABLED
  // TODO: YAML backend implementation
  // Will use a lightweight YAML parser (e.g., mini-yaml or libyaml).
  // The CMake target will define OSP_CONFIG_YAML_ENABLED when
  // the OSP_CONFIG_YAML option is ON and a suitable YAML library
  // is available via FetchContent.
#endif  // OSP_CONFIG_YAML_ENABLED
};

}  // namespace osp

#endif  // OSP_CONFIG_HPP_
