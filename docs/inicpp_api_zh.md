# inicpp API 参考文档

> newosp 内嵌的 INI 配置文件解析库
> 版本: 1.0
> 来源: [inifile-cpp](https://github.com/DeguiLiu/inifile-cpp) (已集成至 `include/osp/inicpp.h`)

---

## 概述

`inicpp` 是一个轻量级、header-only 的 INI 文件解析库，已被集成到 newosp 项目中。它提供了简单易用的 API 来读取、写入和操作 INI 格式的配置文件。

### 特性

- **Header-only**: 单头文件，无需编译
- **C++17 标准**: 完全兼容 C++17
- **类型安全**: 支持多种基本类型的自动转换
- **灵活解析**: 支持注释、空行、空白符处理
- **文件 I/O**: 直接加载/保存 INI 文件
- **字符串解析**: 从内存字符串解析 INI 内容
- **可扩展**: 支持自定义类型转换

### 命名空间

所有 inicpp 类和函数都在 `osp::ini` 命名空间中。

---

## 核心类

### IniField

表示 INI 文件中的一个字段值。

#### 构造函数

```cpp
// 默认构造（空字段）
IniField();

// 从字符串构造
IniField(const std::string& value);

// 拷贝构造
IniField(const IniField& field);
```

#### 类型转换

```cpp
// 将字段值转换为指定类型
template <typename T>
T As() const;
```

支持的类型：
- `bool`
- `char`, `unsigned char`
- `short`, `unsigned short`
- `int`, `unsigned int`
- `long`, `unsigned long`
- `float`, `double`
- `std::string`
- `const char*`
- `std::string_view` (C++17)

#### 赋值操作

```cpp
// 从各种类型赋值
template <typename T>
IniField& operator=(const T& value);

// 从另一个字段拷贝
IniField& operator=(const IniField& field);
```

#### 示例

```cpp
osp::ini::IniField field;

field = 42;
int value = field.As<int>();  // 42

field = "hello";
std::string str = field.As<std::string>();  // "hello"

field = 3.14;
double d = field.As<double>();  // 3.14
```

---

### IniSection

表示 INI 文件中的一个节（section），本质上是一个 `std::map<std::string, IniField>`。

#### 类型定义

```cpp
using IniSection = IniSectionBase<std::less<std::string>>;
using IniSectionCaseInsensitive = IniSectionBase<StringInsensitiveLess>;
```

#### 字段访问

```cpp
// 下标访问（如不存在则创建）
IniField& operator[](const std::string& key);

// 查找字段
iterator find(const std::string& key);
const_iterator find(const std::string& key) const;

// 获取字段数量
size_t size() const;

// 迭代器
iterator begin();
iterator end();
const_iterator begin() const;
const_iterator end() const;
```

#### 示例

```cpp
osp::ini::IniSection section;

section["name"] = "server";
section["port"] = 8080;
section["enabled"] = true;

// 检查字段是否存在
if (section.find("port") != section.end()) {
    int port = section["port"].As<int>();
}

// 遍历所有字段
for (const auto& [key, field] : section) {
    std::cout << key << " = " << field.As<std::string>() << std::endl;
}
```

---

### IniFile

表示完整的 INI 文件，本质上是一个 `std::map<std::string, IniSection>`。

#### 类型定义

```cpp
using IniFile = IniFileBase<std::less<std::string>>;
using IniFileCaseInsensitive = IniFileBase<StringInsensitiveLess>;
```

#### 构造函数

```cpp
// 默认构造
IniFile();

// 从文件构造
IniFile(const std::string& filename);

// 从流构造
IniFile(std::istream& is);

// 自定义字段分隔符和注释符
IniFile(const char field_sep, const char comment);
IniFile(const char field_sep, const std::vector<std::string>& comment_prefixes);
```

#### 文件 I/O

```cpp
// 从文件加载
void Load(const std::string& filename);

// 保存到文件
void Save(const std::string& filename) const;
```

#### 字符串解析/编码

```cpp
// 从字符串解析
void Decode(const std::string& content);
void Decode(std::istream& is);

// 编码为字符串
std::string Encode() const;
void Encode(std::ostream& os) const;
```

#### 节访问

```cpp
// 下标访问（如不存在则创建）
IniSection& operator[](const std::string& section);

// 查找节
iterator find(const std::string& section);
const_iterator find(const std::string& section) const;

// 获取节数量
size_t size() const;

// 迭代器
iterator begin();
iterator end();
const_iterator begin() const;
const_iterator end() const;
```

#### 配置选项

```cpp
// 设置字段分隔符（默认 '='）
void SetFieldSep(const char sep);

// 设置注释字符（默认 '#'）
void SetCommentChar(const char comment);

// 设置注释前缀列表（默认 {"#", ";"}）
void SetCommentPrefixes(const std::vector<std::string>& comment_prefixes);

// 设置转义字符（默认 '\'）
void SetEscapeChar(const char esc);

// 启用/禁用多行值（默认 false）
void SetMultiLineValues(bool enable);

// 允许/禁止覆盖重复字段（默认 true）
void AllowOverwriteDuplicateFields(bool allowed);
```

#### 示例

```cpp
// 创建和保存
osp::ini::IniFile ini;
ini["Server"]["host"] = "127.0.0.1";
ini["Server"]["port"] = 8080;
ini["Database"]["name"] = "mydb";
ini.Save("config.ini");

// 加载和读取
osp::ini::IniFile loaded;
loaded.Load("config.ini");
std::string host = loaded["Server"]["host"].As<std::string>();
int port = loaded["Server"]["port"].As<int>();

// 从字符串解析
std::string content = "[App]\nname=test\nversion=1.0\n";
osp::ini::IniFile ini2;
ini2.Decode(content);

// 编码为字符串
std::string encoded = ini2.Encode();
std::cout << encoded << std::endl;

// 遍历所有节和字段
for (const auto& [section_name, section] : ini) {
    std::cout << "[" << section_name << "]" << std::endl;
    for (const auto& [field_name, field] : section) {
        std::cout << field_name << " = " 
                  << field.As<std::string>() << std::endl;
    }
}
```

---

## 自定义类型转换

可以为自定义类型实现 `Convert` 模板特化来支持自动类型转换。

### 转换接口

```cpp
namespace osp {
namespace ini {

template <typename T>
struct Convert {
    // 从字符串解码为类型 T
    void Decode(const std::string& value, T& result);
    
    // 将类型 T 编码为字符串
    void Encode(const T& value, std::string& result);
};

}  // namespace ini
}  // namespace osp
```

### 示例：std::vector 支持

```cpp
namespace osp {
namespace ini {

template <typename T>
struct Convert<std::vector<T>> {
    void Decode(const std::string& value, std::vector<T>& result) {
        result.clear();
        T decoded;
        size_t startPos = 0;
        size_t endPos = 0;
        
        while (endPos != std::string::npos) {
            if (endPos != 0) startPos = endPos + 1;
            endPos = value.find(',', startPos);
            
            size_t cnt = (endPos == std::string::npos) 
                ? value.size() - startPos 
                : endPos - startPos;
            
            std::string tmp = value.substr(startPos, cnt);
            Convert<T> conv;
            conv.Decode(tmp, decoded);
            result.push_back(decoded);
        }
    }
    
    void Encode(const std::vector<T>& value, std::string& result) {
        std::string encoded;
        std::stringstream ss;
        for (size_t i = 0; i < value.size(); ++i) {
            Convert<T> conv;
            conv.Encode(value[i], encoded);
            ss << encoded;
            if (i != value.size() - 1) ss << ',';
        }
        result = ss.str();
    }
};

}  // namespace ini
}  // namespace osp
```

使用示例：

```cpp
osp::ini::IniFile ini;
ini["Data"]["numbers"] = std::vector<int>{1, 2, 3, 4, 5};

// 读取
std::vector<int> numbers = ini["Data"]["numbers"].As<std::vector<int>>();
```

---

## INI 文件格式

### 基本语法

```ini
; 这是注释（分号）
# 这也是注释（井号）

[Section1]
key1 = value1
key2 = 123
key3 = true

[Section2]
# 支持空格
  key4  =  value4  

# 支持特殊字符
path = /usr/local/bin
url = https://example.com:8080/path?key=value
```

### 支持的值类型

| 类型 | 示例 | 说明 |
|------|------|------|
| 字符串 | `name=hello` | 任意文本 |
| 整数 | `port=8080` | 十进制、八进制(0755)、十六进制(0xFF) |
| 浮点数 | `timeout=30.5` | 支持科学计数法 |
| 布尔值 | `debug=true` | true/false (大小写不敏感) |

### 注释

- 行注释：以 `#` 或 `;` 开头
- 行内注释：在值后面使用 `#` 或 `;`
- 转义注释符：使用 `\#` 或 `\;` 表示字面字符

```ini
[Example]
# 这是行注释
key1 = value1  ; 这是行内注释
key2 = value\; with semicolon  ; 转义的分号
```

### 多行值

启用多行值支持后，可以将值分多行书写：

```cpp
ini.SetMultiLineValues(true);
```

```ini
[Text]
description = This is a long text
    that spans multiple lines
    with proper indentation
```

---

## 与 config.hpp 的集成

`config.hpp` 使用 `inicpp` 作为 INI 后端，提供统一的配置接口。

### config.hpp 中的使用

```cpp
#ifdef OSP_CONFIG_INI_ENABLED
#include "osp/inicpp.h"

template <>
struct ConfigParser<IniBackend> {
    static expected<void, ConfigError> ParseFile(ConfigStore& store, const char* path) {
        try {
            ini::IniFile ini_file;
            ini_file.Load(path);
            
            for (const auto& section_pair : ini_file) {
                for (const auto& field_pair : section_pair.second) {
                    std::string value = field_pair.second.As<std::string>();
                    if (!store.AddEntry(section_pair.first.c_str(), 
                                       field_pair.first.c_str(), 
                                       value.c_str())) {
                        return expected<void, ConfigError>::error(ConfigError::kBufferFull);
                    }
                }
            }
            return expected<void, ConfigError>::success();
        } catch (const std::exception&) {
            return expected<void, ConfigError>::error(ConfigError::kParseError);
        }
    }
};
#endif
```

### 使用示例

```cpp
#include "osp/config.hpp"

// 使用 IniConfig 类型别名
osp::IniConfig cfg;
auto result = cfg.LoadFile("app.ini");

if (result.has_value()) {
    int port = cfg.GetInt("server", "port", 8080);
    std::string host = cfg.GetString("server", "host", "localhost");
    bool debug = cfg.GetBool("app", "debug", false);
}
```

---

## 线程安全性

- **IniField**: 不是线程安全的，应仅在单线程中使用
- **IniSection**: 不是线程安全的，应仅在单线程中使用
- **IniFile**: 不是线程安全的，应仅在单线程中使用

**推荐做法**：
1. 在程序启动时加载配置（单线程）
2. 配置加载后作为只读使用（多线程安全）
3. 如需运行时修改，使用外部同步机制（mutex）

---

## 示例程序

项目包含以下 inicpp 示例：

1. **inicpp_demo.cpp** - 综合演示
   - 创建和编码 INI 文件
   - 从字符串解码
   - 文件加载/保存
   - 遍历节和字段

2. **custom_type_conversion.cpp** - 自定义类型转换
   - std::vector<T> 支持
   - 逗号分隔列表解析

运行示例：

```bash
# 构建示例
cmake -B build -DOSP_BUILD_EXAMPLES=ON
cmake --build build

# 运行综合演示
./build/examples/osp_inicpp_demo

# 运行自定义类型示例
./build/examples/osp_inicpp_custom_type
```

---

## 测试覆盖

`test_inicpp.cpp` 提供了完整的单元测试覆盖：

- ✓ IniField 类型转换（整数、浮点、布尔、字符串）
- ✓ IniSection 基本操作
- ✓ IniFile 解析（简单、多节、注释、空白符）
- ✓ IniFile 编码和往返测试
- ✓ 文件 I/O 操作
- ✓ 边缘情况（重复字段、空值、特殊字符、进制转换）
- ✓ 迭代器支持

运行测试：

```bash
cmake -B build -DOSP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -R test_inicpp
```

---

## 性能特性

- **解析速度**: O(n)，其中 n 是文件大小
- **内存占用**: 使用 std::map 存储，O(sections × fields)
- **查找复杂度**: O(log n)（基于 std::map 的红黑树）
- **迭代性能**: O(n)，按键排序遍历

对于大型配置文件（>1000行），建议：
1. 使用 `IniFileCaseInsensitive` 提高查找效率
2. 缓存常用字段值而非重复查询
3. 考虑使用 `config.hpp` 的 `ConfigStore` 进行扁平化存储

---

## 已知限制

1. **不支持节嵌套**: INI 格式本身不支持多级节
2. **键值对顺序**: 使用 std::map，按键字典序存储
3. **内存解析**: 大文件需一次性加载到内存
4. **异常处理**: 类型转换失败会抛出 `std::invalid_argument`
5. **Unicode 支持**: 仅支持 ASCII 和 UTF-8 编码

---

## 常见问题

### Q: 如何处理不存在的键？

A: 使用 `find()` 检查键是否存在，或使用默认值：

```cpp
// 方法1：检查存在性
if (ini["Section"].find("key") != ini["Section"].end()) {
    int value = ini["Section"]["key"].As<int>();
}

// 方法2：使用 config.hpp 的默认值API
osp::IniConfig cfg;
cfg.LoadFile("app.ini");
int value = cfg.GetInt("section", "key", 42);  // 默认值 42
```

### Q: 如何处理类型转换错误？

A: 使用 try-catch 捕获异常：

```cpp
try {
    int value = ini["Section"]["key"].As<int>();
} catch (const std::invalid_argument& e) {
    std::cerr << "转换失败: " << e.what() << std::endl;
    // 使用默认值
    int value = 0;
}
```

### Q: 如何保留键的插入顺序？

A: std::map 按键排序，无法保留插入顺序。如需保序，可：
1. 使用外部索引记录顺序
2. 修改 IniFile 使用 `std::vector<std::pair<>>`
3. 使用第三方有序map（如 boost::container::flat_map）

### Q: 支持哪些注释风格？

A: 默认支持 `#` 和 `;`，可自定义：

```cpp
ini.SetCommentPrefixes({"#", ";", "//"});
```

---

## 参考资源

- 源代码: `include/osp/inicpp.h`
- 示例代码: `examples/inicpp/`
- 单元测试: `tests/test_inicpp.cpp`
- 上游项目: https://github.com/DeguiLiu/inifile-cpp
- config.hpp 集成: `include/osp/config.hpp`

---

**版权**: MIT License (Fabian Meyer)  
**集成**: 2024, newosp 项目
