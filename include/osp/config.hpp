/**
 * @file config.hpp
 * @brief Multi-format configuration parser with template-based backend dispatch.
 *
 * Design patterns:
 *   - Tag dispatch: IniBackend / JsonBackend / YamlBackend type tags
 *   - Template specialization: ConfigParser<Backend> per-format parsers
 *   - Variadic templates: Config<Backends...> compile-time composition
 *   - Recursive if-constexpr: zero-overhead format dispatch
 *   - CRTP-friendly base: ConfigStore provides shared data + getters
 *
 * Supported backends (CMake opt-in):
 *   - IniBackend  : inih library       (OSP_CONFIG_INI_ENABLED)
 *   - JsonBackend : nlohmann/json       (OSP_CONFIG_JSON_ENABLED)
 *   - YamlBackend : fkYAML             (OSP_CONFIG_YAML_ENABLED)
 *
 * All formats are flattened to "section + key = value" model.
 * Compatible with -fno-exceptions -fno-rtti.
 *
 * Usage:
 * @code
 *   osp::MultiConfig cfg;
 *   cfg.LoadFile("app.yaml");
 *   int32_t port = cfg.GetInt("network", "port", 8080);
 * @endcode
 */

#ifndef OSP_CONFIG_HPP_
#define OSP_CONFIG_HPP_

#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef OSP_CONFIG_INI_ENABLED
#include "ini.h"
#endif

#ifdef OSP_CONFIG_JSON_ENABLED
#include <nlohmann/json.hpp>
#endif

#ifdef OSP_CONFIG_YAML_ENABLED
#include <fkYAML/node.hpp>
#endif

namespace osp {

// ============================================================================
// ConfigFormat
// ============================================================================

enum class ConfigFormat : uint8_t {
  kAuto = 0,
  kIni,
  kJson,
  kYaml,
};

// ============================================================================
// Backend Tag Types (tag dispatch)
// ============================================================================

namespace detail {

inline bool ExtCaseEqual(const char* a, const char* b) noexcept {
  while (*a != '\0' && *b != '\0') {
    char la = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
    char lb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
    if (la != lb) return false;
    ++a; ++b;
  }
  return *a == *b;
}

}  // namespace detail

struct IniBackend {
  static constexpr ConfigFormat kFormat = ConfigFormat::kIni;
  static bool MatchesExtension(const char* ext) noexcept {
    return detail::ExtCaseEqual(ext, "ini") ||
           detail::ExtCaseEqual(ext, "cfg") ||
           detail::ExtCaseEqual(ext, "conf");
  }
};

struct JsonBackend {
  static constexpr ConfigFormat kFormat = ConfigFormat::kJson;
  static bool MatchesExtension(const char* ext) noexcept {
    return detail::ExtCaseEqual(ext, "json");
  }
};

struct YamlBackend {
  static constexpr ConfigFormat kFormat = ConfigFormat::kYaml;
  static bool MatchesExtension(const char* ext) noexcept {
    return detail::ExtCaseEqual(ext, "yaml") ||
           detail::ExtCaseEqual(ext, "yml");
  }
};

// ============================================================================
// ConfigStore - Flat key-value storage base
// ============================================================================

#ifndef OSP_CONFIG_MAX_FILE_SIZE
#define OSP_CONFIG_MAX_FILE_SIZE 8192U
#endif

class ConfigStore {
 public:
  // --- Typed Getters ---

  const char* GetString(const char* section, const char* key,
                        const char* default_val = "") const {
    const Entry* e = FindEntry(section, key);
    return (e != nullptr) ? e->value : default_val;
  }

  int32_t GetInt(const char* section, const char* key,
                 int32_t default_val = 0) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) return default_val;
    char* end = nullptr;
    long val = std::strtol(e->value, &end, 10);
    return (end == e->value) ? default_val : static_cast<int32_t>(val);
  }

  uint16_t GetPort(const char* section, const char* key,
                   uint16_t default_val = 0) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) return default_val;
    char* end = nullptr;
    long val = std::strtol(e->value, &end, 10);
    if (end == e->value) return default_val;
    if (val < 0) return 0;
    if (val > 65535) return 65535;
    return static_cast<uint16_t>(val);
  }

  bool GetBool(const char* section, const char* key,
               bool default_val = false) const {
    const Entry* e = FindEntry(section, key);
    return (e != nullptr) ? ParseBool(e->value) : default_val;
  }

  double GetDouble(const char* section, const char* key,
                   double default_val = 0.0) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) return default_val;
    char* end = nullptr;
    double val = std::strtod(e->value, &end);
    return (end == e->value) ? default_val : val;
  }

  // --- Optional Getters ---

  optional<int32_t> FindInt(const char* section, const char* key) const {
    const Entry* e = FindEntry(section, key);
    if (e == nullptr) return {};
    char* end = nullptr;
    long val = std::strtol(e->value, &end, 10);
    return (end == e->value) ? optional<int32_t>{}
                             : optional<int32_t>{static_cast<int32_t>(val)};
  }

  optional<bool> FindBool(const char* section, const char* key) const {
    const Entry* e = FindEntry(section, key);
    return (e == nullptr) ? optional<bool>{} : optional<bool>{ParseBool(e->value)};
  }

  // --- Query ---

  bool HasSection(const char* section) const {
    OSP_ASSERT(section != nullptr);
    for (uint32_t i = 0; i < count_; ++i) {
      if (StrCaseEqual(entries_[i].section, section)) return true;
    }
    return false;
  }

  bool HasKey(const char* section, const char* key) const {
    return FindEntry(section, key) != nullptr;
  }

  uint32_t EntryCount() const noexcept { return count_; }

 protected:
  static constexpr uint32_t kMaxEntries = 128;
  static constexpr uint32_t kMaxKeyLen = 64;
  static constexpr uint32_t kMaxValueLen = 256;

  struct Entry {
    char section[kMaxKeyLen];
    char key[kMaxKeyLen];
    char value[kMaxValueLen];
  };

  Entry entries_[kMaxEntries];
  uint32_t count_ = 0;

  bool AddEntry(const char* section, const char* key, const char* value) {
    for (uint32_t i = 0; i < count_; ++i) {
      if (StrCaseEqual(entries_[i].section, section) &&
          StrCaseEqual(entries_[i].key, key)) {
        SafeCopy(entries_[i].value, value, kMaxValueLen);
        return true;
      }
    }
    if (count_ >= kMaxEntries) return false;
    Entry& e = entries_[count_];
    SafeCopy(e.section, section, kMaxKeyLen);
    SafeCopy(e.key, key, kMaxKeyLen);
    SafeCopy(e.value, value, kMaxValueLen);
    ++count_;
    return true;
  }

  static expected<uint32_t, ConfigError> ReadFileToBuffer(
      const char* path, char* buf, uint32_t buf_size) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr)
      return expected<uint32_t, ConfigError>::error(ConfigError::kFileNotFound);
    size_t bytes = std::fread(buf, 1, buf_size - 1, f);
    std::fclose(f);
    buf[bytes] = '\0';
    return expected<uint32_t, ConfigError>::success(static_cast<uint32_t>(bytes));
  }

  const Entry* FindEntry(const char* section, const char* key) const {
    OSP_ASSERT(section != nullptr && key != nullptr);
    for (uint32_t i = 0; i < count_; ++i) {
      if (StrCaseEqual(entries_[i].section, section) &&
          StrCaseEqual(entries_[i].key, key))
        return &entries_[i];
    }
    return nullptr;
  }

  static bool StrCaseEqual(const char* a, const char* b) noexcept {
    while (*a != '\0' && *b != '\0') {
      char la = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
      char lb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
      if (la != lb) return false;
      ++a; ++b;
    }
    return *a == *b;
  }

  static void SafeCopy(char* dst, const char* src, uint32_t dst_size) noexcept {
    if (src == nullptr) { dst[0] = '\0'; return; }
    uint32_t i = 0;
    while (i < (dst_size - 1U) && src[i] != '\0') { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
  }

  static bool ParseBool(const char* str) noexcept {
    if (str == nullptr) return false;
    return StrCaseEqual(str, "true") || StrCaseEqual(str, "1") ||
           StrCaseEqual(str, "yes") || StrCaseEqual(str, "on");
  }

  static const char* GetExtension(const char* path) noexcept {
    const char* dot = nullptr;
    for (const char* p = path; *p != '\0'; ++p) {
      if (*p == '.') dot = p;
    }
    return (dot != nullptr) ? dot + 1 : nullptr;
  }

  template <typename> friend struct ConfigParser;
};

// ============================================================================
// ConfigParser<Backend> - Template specialization per format
// ============================================================================

/** Default: format not supported (compile-time safe fallback). */
template <typename Backend>
struct ConfigParser {
  static expected<void, ConfigError> ParseFile(ConfigStore&, const char*) {
    return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
  }
  static expected<void, ConfigError> ParseBuffer(ConfigStore&, const char*,
                                                  uint32_t) {
    return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
  }
};

// --- INI Backend ---

#ifdef OSP_CONFIG_INI_ENABLED
template <>
struct ConfigParser<IniBackend> {
  static expected<void, ConfigError> ParseFile(ConfigStore& store,
                                                const char* path) {
    int result = ini_parse(path, Handler, &store);
    if (result == -1)
      return expected<void, ConfigError>::error(ConfigError::kFileNotFound);
    if (result != 0)
      return expected<void, ConfigError>::error(ConfigError::kParseError);
    return expected<void, ConfigError>::success();
  }

  static expected<void, ConfigError> ParseBuffer(ConfigStore& store,
                                                  const char* data, uint32_t) {
    int result = ini_parse_string(data, Handler, &store);
    if (result != 0)
      return expected<void, ConfigError>::error(ConfigError::kParseError);
    return expected<void, ConfigError>::success();
  }

 private:
  static int Handler(void* user, const char* section, const char* name,
                     const char* value) {
    auto* s = static_cast<ConfigStore*>(user);
    return s->AddEntry(section ? section : "", name ? name : "",
                       value ? value : "") ? 1 : 0;
  }
};
#endif

// --- JSON Backend ---

#ifdef OSP_CONFIG_JSON_ENABLED
template <>
struct ConfigParser<JsonBackend> {
  static expected<void, ConfigError> ParseFile(ConfigStore& store,
                                                const char* path) {
    char buf[OSP_CONFIG_MAX_FILE_SIZE];
    auto r = ConfigStore::ReadFileToBuffer(path, buf, sizeof(buf));
    if (!r.has_value()) return expected<void, ConfigError>::error(r.get_error());
    return ParseBuffer(store, buf, r.value());
  }

  static expected<void, ConfigError> ParseBuffer(ConfigStore& store,
                                                  const char* data, uint32_t) {
    auto j = nlohmann::json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object())
      return expected<void, ConfigError>::error(ConfigError::kParseError);

    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it->is_object()) {
        for (auto kit = it->begin(); kit != it->end(); ++kit) {
          char val[ConfigStore::kMaxValueLen];
          ToStr(*kit, val, sizeof(val));
          if (!store.AddEntry(it.key().c_str(), kit.key().c_str(), val))
            return expected<void, ConfigError>::error(ConfigError::kBufferFull);
        }
      } else {
        char val[ConfigStore::kMaxValueLen];
        ToStr(*it, val, sizeof(val));
        if (!store.AddEntry("", it.key().c_str(), val))
          return expected<void, ConfigError>::error(ConfigError::kBufferFull);
      }
    }
    return expected<void, ConfigError>::success();
  }

 private:
  static void ToStr(const nlohmann::json& n, char* b, uint32_t sz) noexcept {
    if (n.is_string()) {
      ConfigStore::SafeCopy(b, n.get_ref<const std::string&>().c_str(), sz);
    } else if (n.is_boolean()) {
      ConfigStore::SafeCopy(b, n.get<bool>() ? "true" : "false", sz);
    } else if (n.is_number_integer()) {
      std::snprintf(b, sz, "%ld", static_cast<long>(n.get<int64_t>()));
    } else if (n.is_number_float()) {
      std::snprintf(b, sz, "%g", n.get<double>());
    } else {
      auto s = n.dump();
      ConfigStore::SafeCopy(b, s.c_str(), sz);
    }
  }
};
#endif

// --- YAML Backend ---

#ifdef OSP_CONFIG_YAML_ENABLED
template <>
struct ConfigParser<YamlBackend> {
  static expected<void, ConfigError> ParseFile(ConfigStore& store,
                                                const char* path) {
    char buf[OSP_CONFIG_MAX_FILE_SIZE];
    auto r = ConfigStore::ReadFileToBuffer(path, buf, sizeof(buf));
    if (!r.has_value()) return expected<void, ConfigError>::error(r.get_error());
    return ParseBuffer(store, buf, r.value());
  }

  static expected<void, ConfigError> ParseBuffer(ConfigStore& store,
                                                  const char* data,
                                                  uint32_t size) {
    std::string yaml_str(data, size);
    auto root = fkyaml::node::deserialize(yaml_str);
    if (root.is_null() || !root.is_mapping())
      return expected<void, ConfigError>::error(ConfigError::kParseError);

    for (auto it = root.begin(); it != root.end(); ++it) {
      auto sec = it.key().get_value<std::string>();
      auto& node = *it;

      if (node.is_mapping()) {
        for (auto kit = node.begin(); kit != node.end(); ++kit) {
          auto key = kit.key().get_value<std::string>();
          char val[ConfigStore::kMaxValueLen];
          ToStr(*kit, val, sizeof(val));
          if (!store.AddEntry(sec.c_str(), key.c_str(), val))
            return expected<void, ConfigError>::error(ConfigError::kBufferFull);
        }
      } else {
        char val[ConfigStore::kMaxValueLen];
        ToStr(node, val, sizeof(val));
        if (!store.AddEntry("", sec.c_str(), val))
          return expected<void, ConfigError>::error(ConfigError::kBufferFull);
      }
    }
    return expected<void, ConfigError>::success();
  }

 private:
  static void ToStr(const fkyaml::node& n, char* b, uint32_t sz) noexcept {
    if (n.is_string()) {
      auto s = n.get_value<std::string>();
      ConfigStore::SafeCopy(b, s.c_str(), sz);
    } else if (n.is_boolean()) {
      ConfigStore::SafeCopy(b, n.get_value<bool>() ? "true" : "false", sz);
    } else if (n.is_integer()) {
      std::snprintf(b, sz, "%ld", static_cast<long>(n.get_value<int64_t>()));
    } else if (n.is_float_number()) {
      std::snprintf(b, sz, "%g", n.get_value<double>());
    } else {
      b[0] = '\0';
    }
  }
};
#endif

// ============================================================================
// Config<Backends...> - Compile-time composable config reader
// ============================================================================

template <typename... Backends>
class Config final : public ConfigStore {
  static_assert(sizeof...(Backends) > 0, "Config requires at least one backend");

 public:
  Config() = default;

  expected<void, ConfigError> LoadFile(
      const char* path, ConfigFormat format = ConfigFormat::kAuto) {
    OSP_ASSERT(path != nullptr);
    if (format == ConfigFormat::kAuto) format = DetectFormat(path);
    return DispatchFile<Backends...>(path, format);
  }

  expected<void, ConfigError> LoadBuffer(const char* data, uint32_t size,
                                          ConfigFormat format) {
    OSP_ASSERT(data != nullptr);
    return DispatchBuffer<Backends...>(data, size, format);
  }

 private:
  // --- Recursive dispatch (file) ---
  template <typename First, typename... Rest>
  expected<void, ConfigError> DispatchFile(const char* path,
                                            ConfigFormat format) {
    if (First::kFormat == format)
      return ConfigParser<First>::ParseFile(*this, path);
    if constexpr (sizeof...(Rest) > 0)
      return DispatchFile<Rest...>(path, format);
    return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
  }

  // --- Recursive dispatch (buffer) ---
  template <typename First, typename... Rest>
  expected<void, ConfigError> DispatchBuffer(const char* data, uint32_t size,
                                              ConfigFormat format) {
    if (First::kFormat == format)
      return ConfigParser<First>::ParseBuffer(*this, data, size);
    if constexpr (sizeof...(Rest) > 0)
      return DispatchBuffer<Rest...>(data, size, format);
    return expected<void, ConfigError>::error(ConfigError::kFormatNotSupported);
  }

  // --- Auto-detect from file extension ---
  ConfigFormat DetectFormat(const char* path) const noexcept {
    const char* ext = GetExtension(path);
    if (ext == nullptr) return Head::kFormat;
    return DetectExt<Backends...>(ext);
  }

  template <typename First, typename... Rest>
  ConfigFormat DetectExt(const char* ext) const noexcept {
    if (First::MatchesExtension(ext)) return First::kFormat;
    if constexpr (sizeof...(Rest) > 0) return DetectExt<Rest...>(ext);
    return Head::kFormat;
  }

  // First backend type (used as fallback format)
  using Head = typename std::tuple_element<0, std::tuple<Backends...>>::type;
};

// ============================================================================
// Convenience Type Aliases
// ============================================================================

using MultiConfig = Config<
#ifdef OSP_CONFIG_INI_ENABLED
    IniBackend
#endif
#if defined(OSP_CONFIG_INI_ENABLED) && \
    (defined(OSP_CONFIG_JSON_ENABLED) || defined(OSP_CONFIG_YAML_ENABLED))
    ,
#endif
#ifdef OSP_CONFIG_JSON_ENABLED
    JsonBackend
#endif
#if defined(OSP_CONFIG_JSON_ENABLED) && defined(OSP_CONFIG_YAML_ENABLED)
    ,
#endif
#ifdef OSP_CONFIG_YAML_ENABLED
    YamlBackend
#endif
    >;

#ifdef OSP_CONFIG_INI_ENABLED
using IniConfig = Config<IniBackend>;
#endif
#ifdef OSP_CONFIG_JSON_ENABLED
using JsonConfig = Config<JsonBackend>;
#endif
#ifdef OSP_CONFIG_YAML_ENABLED
using YamlConfig = Config<YamlBackend>;
#endif

}  // namespace osp

#endif  // OSP_CONFIG_HPP_
