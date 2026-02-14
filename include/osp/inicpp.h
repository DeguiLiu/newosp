/*
 * inicpp.h
 *
 * Created on: 26 Dec 2015
 *     Author: Fabian Meyer
 *    License: MIT
 */

#ifndef INICPP_H_
#define INICPP_H_

#include <algorithm>
#include <assert.h>
#include <fstream>
#include <istream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __cpp_lib_string_view  // This one is defined in <string> if we have std::string_view
#include <string_view>
#endif

namespace osp {
namespace ini {
/************************************************
 * Helper Functions
 ************************************************/

/** Returns a string of whitespace characters. */
constexpr const char* Whitespaces() {
  return " \t\n\r\f\v";
}

/** Returns a string of indentation characters. */
constexpr const char* Indents() {
  return " \t";
}

/** Trims a string in place.
 * @param str string to be trimmed in place */
inline void Trim(std::string& str) {
  // first erasing from end should be slightly more efficient
  // because erasing from start potentially moves all chars
  // multiple indices towards the front.

  auto lastpos = str.find_last_not_of(Whitespaces());
  if (lastpos == std::string::npos) {
    str.clear();
    return;
  }

  str.erase(lastpos + 1);
  str.erase(0, str.find_first_not_of(Whitespaces()));
}

/************************************************
 * Conversion Functors
 ************************************************/

inline bool StrToLong(const std::string& value, long& result) {
  char* endptr;
  // check if decimal
  result = std::strtol(value.c_str(), &endptr, 10);
  if (*endptr == '\0')
    return true;
  // check if octal
  result = std::strtol(value.c_str(), &endptr, 8);
  if (*endptr == '\0')
    return true;
  // check if hex
  result = std::strtol(value.c_str(), &endptr, 16);
  if (*endptr == '\0')
    return true;

  return false;
}

inline bool StrToULong(const std::string& value, unsigned long& result) {
  char* endptr;
  // check if decimal
  result = std::strtoul(value.c_str(), &endptr, 10);
  if (*endptr == '\0')
    return true;
  // check if octal
  result = std::strtoul(value.c_str(), &endptr, 8);
  if (*endptr == '\0')
    return true;
  // check if hex
  result = std::strtoul(value.c_str(), &endptr, 16);
  if (*endptr == '\0')
    return true;

  return false;
}

template <typename T>
struct Convert {};

template <>
struct Convert<bool> {
  void Decode(const std::string& value, bool& result) {
    std::string str(value);
    std::transform(str.begin(), str.end(), str.begin(), [](const char c) { return static_cast<char>(::toupper(c)); });

    if (str == "TRUE")
      result = true;
    else if (str == "FALSE")
      result = false;
    else
      throw std::invalid_argument("field is not a bool");
  }

  void Encode(const bool value, std::string& result) { result = value ? "true" : "false"; }
};

template <>
struct Convert<char> {
  void Decode(const std::string& value, char& result) {
    assert(value.size() > 0);
    result = value[0];
  }

  void Encode(const char value, std::string& result) { result = value; }
};

template <>
struct Convert<unsigned char> {
  void Decode(const std::string& value, unsigned char& result) {
    assert(value.size() > 0);
    result = value[0];
  }

  void Encode(const unsigned char value, std::string& result) { result = value; }
};

template <>
struct Convert<short> {
  void Decode(const std::string& value, short& result) {
    long tmp;
    if (!StrToLong(value, tmp))
      throw std::invalid_argument("field is not a short");
    result = static_cast<short>(tmp);
  }

  void Encode(const short value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<unsigned short> {
  void Decode(const std::string& value, unsigned short& result) {
    unsigned long tmp;
    if (!StrToULong(value, tmp))
      throw std::invalid_argument("field is not an unsigned short");
    result = static_cast<unsigned short>(tmp);
  }

  void Encode(const unsigned short value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<int> {
  void Decode(const std::string& value, int& result) {
    long tmp;
    if (!StrToLong(value, tmp))
      throw std::invalid_argument("field is not an int");
    result = static_cast<int>(tmp);
  }

  void Encode(const int value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<unsigned int> {
  void Decode(const std::string& value, unsigned int& result) {
    unsigned long tmp;
    if (!StrToULong(value, tmp))
      throw std::invalid_argument("field is not an unsigned int");
    result = static_cast<unsigned int>(tmp);
  }

  void Encode(const unsigned int value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<long> {
  void Decode(const std::string& value, long& result) {
    if (!StrToLong(value, result))
      throw std::invalid_argument("field is not a long");
  }

  void Encode(const long value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<unsigned long> {
  void Decode(const std::string& value, unsigned long& result) {
    if (!StrToULong(value, result))
      throw std::invalid_argument("field is not an unsigned long");
  }

  void Encode(const unsigned long value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<double> {
  void Decode(const std::string& value, double& result) { result = std::stod(value); }

  void Encode(const double value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<float> {
  void Decode(const std::string& value, float& result) { result = std::stof(value); }

  void Encode(const float value, std::string& result) {
    std::stringstream ss;
    ss << value;
    result = ss.str();
  }
};

template <>
struct Convert<std::string> {
  void Decode(const std::string& value, std::string& result) { result = value; }

  void Encode(const std::string& value, std::string& result) { result = value; }
};

#ifdef __cpp_lib_string_view
template <>
struct Convert<std::string_view> {
  void Decode(const std::string& value, std::string_view& result) { result = value; }

  void Encode(const std::string_view value, std::string& result) { result = value; }
};
#endif

template <>
struct Convert<const char*> {
  void Encode(const char* const& value, std::string& result) { result = value; }

  void Decode(const std::string& value, const char*& result) { result = value.c_str(); }
};

template <>
struct Convert<char*> {
  void Encode(const char* const& value, std::string& result) { result = value; }
};

template <size_t n>
struct Convert<char[n]> {
  void Encode(const char* value, std::string& result) { result = value; }
};

class IniField {
 private:
  std::string value_;

 public:
  IniField() : value_() {}

  IniField(const std::string& value) : value_(value) {}
  IniField(const IniField& field) : value_(field.value_) {}

  ~IniField() {}

  template <typename T>
  T As() const {
    Convert<T> conv;
    T result;
    conv.Decode(value_, result);
    return result;
  }

  template <typename T>
  IniField& operator=(const T& value) {
    Convert<T> conv;
    conv.Encode(value, value_);
    return *this;
  }

  IniField& operator=(const IniField& field) {
    value_ = field.value_;
    return *this;
  }
};

struct StringInsensitiveLess {
  bool operator()(std::string lhs, std::string rhs) const {
    std::transform(lhs.begin(), lhs.end(), lhs.begin(), [](const char c) { return static_cast<char>(::tolower(c)); });
    std::transform(rhs.begin(), rhs.end(), rhs.begin(), [](const char c) { return static_cast<char>(::tolower(c)); });
    return lhs < rhs;
  }
};

template <typename Comparator>
class IniSectionBase : public std::map<std::string, IniField, Comparator> {
 public:
  IniSectionBase() {}
  ~IniSectionBase() {}
};

using IniSection = IniSectionBase<std::less<std::string>>;
using IniSectionCaseInsensitive = IniSectionBase<StringInsensitiveLess>;

template <typename Comparator>
class IniFileBase : public std::map<std::string, IniSectionBase<Comparator>, Comparator> {
 private:
  char field_sep_ = '=';
  char esc_ = '\\';
  std::vector<std::string> comment_prefixes_ = {"#", ";"};
  bool multi_line_values_ = false;
  bool overwrite_duplicate_fields_ = true;

  void EraseComment(const std::string& comment_prefix, std::string& str, std::string::size_type startpos = 0) {
    size_t prefixpos = str.find(comment_prefix, startpos);
    if (std::string::npos == prefixpos)
      return;
    // Found a comment prefix, is it escaped?
    if (0 != prefixpos && str[prefixpos - 1] == esc_) {
      // The comment prefix is escaped, so just delete the escape char
      // and keep erasing after the comment prefix
      str.erase(prefixpos - 1, 1);
      EraseComment(comment_prefix, str, prefixpos - 1 + comment_prefix.size());
    } else {
      str.erase(prefixpos);
    }
  }

  void EraseComments(std::string& str) {
    for (const std::string& comment_prefix : comment_prefixes_)
      EraseComment(comment_prefix, str);
  }

  /** Tries to find a suitable comment prefix for the string data at the given
   * position. Returns comment_prefixes_.end() if no match was found. */
  std::vector<std::string>::const_iterator FindCommentPrefix(const std::string& str, const std::size_t startpos) const {
    // if startpos is invalid simply return "not found"
    if (startpos >= str.size())
      return comment_prefixes_.end();

    for (size_t i = 0; i < comment_prefixes_.size(); ++i) {
      const std::string& prefix = comment_prefixes_[i];
      // if this comment prefix is longer than the string view itself
      // then skip
      if (prefix.size() + startpos > str.size())
        continue;

      bool match = true;
      for (size_t j = 0; j < prefix.size() && match; ++j)
        match = str[startpos + j] == prefix[j];

      if (match)
        return comment_prefixes_.begin() + i;
    }

    return comment_prefixes_.end();
  }

  void WriteEscaped(std::ostream& os, const std::string& str) const {
    for (size_t i = 0; i < str.length(); ++i) {
      auto prefixpos = FindCommentPrefix(str, i);
      // if no suitable prefix was found at this position
      // then simply write the current character
      if (prefixpos != comment_prefixes_.end()) {
        const std::string& prefix = *prefixpos;
        os.put(esc_);
        os.write(prefix.c_str(), prefix.size());
        i += prefix.size() - 1;
      } else if (multi_line_values_ && str[i] == '\n') {
        os.write("\n\t", 2);
      } else {
        os.put(str[i]);
      }
    }
  }

 public:
  IniFileBase() = default;

  IniFileBase(const char field_sep, const char comment)
      : field_sep_(field_sep), comment_prefixes_(1, std::string(1, comment)) {}

  IniFileBase(const std::string& filename) { Load(filename); }

  IniFileBase(std::istream& is) { Decode(is); }

  IniFileBase(const char field_sep, const std::vector<std::string>& comment_prefixes)
      : field_sep_(field_sep), comment_prefixes_(comment_prefixes) {}

  IniFileBase(const std::string& filename, const char field_sep, const std::vector<std::string>& comment_prefixes)
      : field_sep_(field_sep), comment_prefixes_(comment_prefixes) {
    Load(filename);
  }

  IniFileBase(std::istream& is, const char field_sep, const std::vector<std::string>& comment_prefixes)
      : field_sep_(field_sep), comment_prefixes_(comment_prefixes) {
    Decode(is);
  }

  ~IniFileBase() {}

  /** Sets the separator character for fields in the INI file.
   * @param sep separator character to be used. */
  void SetFieldSep(const char sep) { field_sep_ = sep; }

  /** Sets the character that should be interpreted as the start of comments.
   * Default is '#'.
   * @param comment comment character to be used. */
  void SetCommentChar(const char comment) { comment_prefixes_ = {std::string(1, comment)}; }

  /** Sets the list of strings that should be interpreted as the start of comments.
   * Default is [ "#" ].
   * @param comment_prefixes vector of comment prefix strings to be used. */
  void SetCommentPrefixes(const std::vector<std::string>& comment_prefixes) { comment_prefixes_ = comment_prefixes; }

  /** Sets the character that should be used to escape comment prefixes.
   * Default is '\'.
   * @param esc escape character to be used. */
  void SetEscapeChar(const char esc) { esc_ = esc; }

  /** Sets whether or not to parse multi-line field values.
   * Default is false.
   * @param enable enable or disable? */
  void SetMultiLineValues(bool enable) { multi_line_values_ = enable; }

  /** Sets whether or not overwriting duplicate fields is allowed.
   * If overwriting duplicate fields is not allowed,
   * an exception is thrown when a duplicate field is found inside a section.
   * Default is true.
   * @param allowed Is overwriting duplicate fields allowed or not? */
  void AllowOverwriteDuplicateFields(bool allowed) { overwrite_duplicate_fields_ = allowed; }

  /** Tries to decode an ini file from the given input stream.
   * @param is input stream from which data should be read. */
  void Decode(std::istream& is) {
    this->clear();
    int line_no = 0;
    IniSectionBase<Comparator>* current_section = nullptr;
    std::string multi_line_value_field_name;
    std::string line;
    // iterate file line by line
    while (!is.eof() && !is.fail()) {
      std::getline(is, line, '\n');
      EraseComments(line);
      bool has_indent = line.find_first_not_of(Indents()) != 0;
      Trim(line);
      ++line_no;

      // skip if line is empty
      if (line.size() == 0)
        continue;

      if (line[0] == '[') {
        // line is a section
        // check if the section is also closed on same line
        std::size_t pos = line.find("]");
        if (pos == std::string::npos) {
          std::stringstream ss;
          ss << "l." << line_no << ": ini parsing failed, section not closed";
          throw std::logic_error(ss.str());
        }
        // check if the section name is empty
        if (pos == 1) {
          std::stringstream ss;
          ss << "l." << line_no << ": ini parsing failed, section is empty";
          throw std::logic_error(ss.str());
        }

        // retrieve section name
        std::string sec_name = line.substr(1, pos - 1);
        current_section = &((*this)[sec_name]);

        // clear multiline value field name
        // a new section means there is no value to continue
        multi_line_value_field_name = "";
      } else {
        // line is a field definition
        // check if section was already opened
        if (current_section == nullptr) {
          std::stringstream ss;
          ss << "l." << line_no
             << ": ini parsing failed, field has no section"
                " or ini file in use by another application";
          throw std::logic_error(ss.str());
        }

        // find key value separator
        std::size_t pos = line.find(field_sep_);
        if (multi_line_values_ && has_indent && !multi_line_value_field_name.empty()) {
          // extend a multi-line value
          IniField previous_value = (*current_section)[multi_line_value_field_name];
          std::string value = previous_value.As<std::string>() + "\n" + line;
          (*current_section)[multi_line_value_field_name] = value;
        } else if (pos == std::string::npos) {
          std::stringstream ss;
          ss << "l." << line_no << ": ini parsing failed, no '" << field_sep_ << "' found";
          if (multi_line_values_)
            ss << ", and not a multi-line value continuation";
          throw std::logic_error(ss.str());
        } else {
          // retrieve field name and value
          std::string name = line.substr(0, pos);
          Trim(name);
          if (!overwrite_duplicate_fields_ && current_section->count(name) != 0) {
            std::stringstream ss;
            ss << "l." << line_no << ": ini parsing failed, duplicate field found";
            throw std::logic_error(ss.str());
          }
          std::string value = line.substr(pos + 1, std::string::npos);
          Trim(value);
          (*current_section)[name] = value;
          // store last field name for potential multi-line values
          multi_line_value_field_name = name;
        }
      }
    }
  }

  /** Tries to decode an ini file from the given input string.
   * @param content string to be decoded. */
  void Decode(const std::string& content) {
    std::istringstream ss(content);
    Decode(ss);
  }

  /** Tries to load and decode an ini file from the file at the given path.
   * @param file_name path to the file that should be loaded. */
  void Load(const std::string& file_name) {
    std::ifstream is(file_name.c_str());
    Decode(is);
  }

  /** Encodes this inifile object and writes the output to the given stream.
   * @param os target stream. */
  void Encode(std::ostream& os) const {
    // iterate through all sections in this file
    for (const auto& file_pair : *this) {
      os.put('[');
      WriteEscaped(os, file_pair.first);
      os.put(']');
      os.put('\n');

      // iterate through all fields in the section
      for (const auto& sec_pair : file_pair.second) {
        WriteEscaped(os, sec_pair.first);
        os.put(field_sep_);
        WriteEscaped(os, sec_pair.second.template As<std::string>());
        os.put('\n');
      }

      // Add a newline after each section
      os.put('\n');
    }
  }

  /** Encodes this inifile object as string and returns the result.
   * @return encoded infile string. */
  std::string Encode() const {
    std::ostringstream ss;
    Encode(ss);
    return ss.str();
  }

  /** Saves this inifile object to the file at the given path.
   * @param file_name path to the file where the data should be stored. */
  void Save(const std::string& file_name) const {
    std::ofstream os(file_name.c_str());
    Encode(os);
  }
};

using IniFile = IniFileBase<std::less<std::string>>;
using IniFileCaseInsensitive = IniFileBase<StringInsensitiveLess>;
}  // namespace ini
}  // namespace osp

#endif
