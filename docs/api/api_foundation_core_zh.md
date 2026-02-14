# newosp API 参考: 基础层与核心通信层

本文档覆盖 newosp 项目的基础设施模块和核心通信层的公共 API。

---

## 1. platform.hpp - 平台检测与编译器提示

**概述**: 提供平台检测宏、编译器优化提示、断言宏和单调时钟工具。

**头文件**: `include/osp/platform.hpp`
**依赖**: 无

### 平台检测宏

| 宏名 | 说明 |
|------|------|
| `OSP_PLATFORM_LINUX` | 定义为 1 表示 Linux 平台 |
| `OSP_PLATFORM_MACOS` | 定义为 1 表示 macOS 平台 |
| `OSP_PLATFORM_WINDOWS` | 定义为 1 表示 Windows 平台 |
| `OSP_ARCH_ARM` | 定义为 1 表示 ARM 架构 |
| `OSP_ARCH_X86` | 定义为 1 表示 x86/x64 架构 |

### 编译器提示宏

| 宏名 | 说明 |
|------|------|
| `OSP_LIKELY(x)` | 提示编译器条件 x 大概率为真 |
| `OSP_UNLIKELY(x)` | 提示编译器条件 x 大概率为假 |
| `OSP_UNUSED` | 标记未使用的变量/参数 |

### 断言宏

| 宏名 | 说明 | 线程安全性 |
|------|------|-----------|
| `OSP_ASSERT(cond)` | Debug 模式断言，Release 模式编译为空操作 | 线程安全 |

### 常量

| 名称 | 类型 | 值 | 说明 |
|------|------|-----|------|
| `kCacheLineSize` | `constexpr size_t` | 64 | 缓存行大小 (字节) |

### 函数

| 函数签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `uint64_t SteadyNowNs() noexcept` | 返回单调时钟纳秒时间戳 | 线程安全 |
| `uint64_t SteadyNowUs() noexcept` | 返回单调时钟微秒时间戳 | 线程安全 |

### ThreadHeartbeat 结构体

轻量级线程活跃度监控原语。

**成员**:
- `std::atomic<uint64_t> last_beat_us` - 最后心跳时间戳 (微秒)

**方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Beat() noexcept` | 记录心跳 (热路径，单次 relaxed store) | 线程安全 |
| `uint64_t LastBeatUs() const noexcept` | 读取最后心跳时间戳 | 线程安全 |

**示例**:
```cpp
osp::ThreadHeartbeat hb;
// 工作线程循环中:
hb.Beat();
// 监控线程:
uint64_t last = hb.LastBeatUs();
```

---

## 2. vocabulary.hpp - 词汇类型

**概述**: 提供栈分配的零堆开销词汇类型，兼容 `-fno-exceptions -fno-rtti`。

**头文件**: `include/osp/vocabulary.hpp`
**依赖**: `platform.hpp`

### expected<V, E>

持有成功值 (V) 或错误 (E) 的类型。

**静态工厂方法**:

| 方法签名 | 说明 |
|---------|------|
| `static expected success(const V& val)` | 构造成功值 |
| `static expected success(V&& val)` | 构造成功值 (移动) |
| `static expected error(E err)` | 构造错误值 |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool has_value() const noexcept` | 检查是否持有成功值 | 非线程安全 |
| `explicit operator bool() const noexcept` | 同 `has_value()` | 非线程安全 |
| `V& value() & noexcept` | 获取成功值引用 (断言 has_value) | 非线程安全 |
| `const V& value() const& noexcept` | 获取成功值常量引用 | 非线程安全 |
| `E get_error() const noexcept` | 获取错误值 (断言 !has_value) | 非线程安全 |
| `V value_or(const V& default_val) const` | 获取值或默认值 | 非线程安全 |

**示例**:
```cpp
auto result = expected<int, ConfigError>::success(42);
if (result) {
    printf("value: %d\n", result.value());
}
```

### optional<T>

持有值 (T) 或空的类型。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `optional() noexcept` | 构造空 optional |
| `optional(const T& val) noexcept` | 构造持有值的 optional |
| `optional(T&& val) noexcept` | 构造持有值的 optional (移动) |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool has_value() const noexcept` | 检查是否持有值 | 非线程安全 |
| `explicit operator bool() const noexcept` | 同 `has_value()` | 非线程安全 |
| `T& value() noexcept` | 获取值引用 (断言 has_value) | 非线程安全 |
| `const T& value() const noexcept` | 获取值常量引用 | 非线程安全 |
| `T value_or(const T& default_val) const` | 获取值或默认值 | 非线程安全 |
| `void reset() noexcept` | 清空 optional | 非线程安全 |

### FixedFunction<Sig, BufferSize>

固定大小的可调用对象包装器 (SBO)。

**模板参数**:
- `Sig` - 函数签名 (如 `void(int)`)
- `BufferSize` - 内部缓冲区大小 (默认 `2 * sizeof(void*)`)

**构造函数**:

| 签名 | 说明 |
|------|------|
| `FixedFunction() noexcept` | 构造空函数 |
| `template<typename F> FixedFunction(F&& f)` | 构造持有可调用对象的函数 (static_assert 大小) |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `Ret operator()(Args... args)` | 调用函数 (断言非空) | 非线程安全 |
| `explicit operator bool() const noexcept` | 检查是否持有函数 | 非线程安全 |

**示例**:
```cpp
FixedFunction<void(int)> fn = [](int x) { printf("%d\n", x); };
fn(42);
```

### FixedString<Capacity>

固定容量的栈分配字符串。

**模板参数**:
- `Capacity` - 最大字符数 (不含 null 终止符)

**构造函数**:

| 签名 | 说明 |
|------|------|
| `constexpr FixedString() noexcept` | 构造空字符串 |
| `FixedString(const char (&str)[N])` | 从字符串字面量构造 (编译期检查长度) |
| `FixedString(TruncateToCapacity_t, const char* str)` | 从 C 字符串构造 (截断到容量) |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `constexpr const char* c_str() const noexcept` | 获取 C 字符串 | 非线程安全 |
| `constexpr uint32_t size() const noexcept` | 获取字符串长度 | 非线程安全 |
| `static constexpr uint32_t capacity() noexcept` | 获取容量 | 线程安全 |
| `constexpr bool empty() const noexcept` | 检查是否为空 | 非线程安全 |
| `void clear() noexcept` | 清空字符串 | 非线程安全 |

**示例**:
```cpp
FixedString<32> name = "sensor_node";
printf("name: %s, len: %u\n", name.c_str(), name.size());
```

### FixedVector<T, Capacity>

固定容量的栈分配向量。

**模板参数**:
- `T` - 元素类型
- `Capacity` - 最大元素数

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool push_back(const T& value)` | 追加元素 (拷贝) | 非线程安全 |
| `bool push_back(T&& value)` | 追加元素 (移动) | 非线程安全 |
| `template<typename... Args> bool emplace_back(Args&&... args)` | 原地构造元素 | 非线程安全 |
| `bool pop_back()` | 弹出末尾元素 | 非线程安全 |
| `bool erase_unordered(uint32_t index)` | 删除元素 (无序，O(1)) | 非线程安全 |
| `void clear()` | 清空向量 | 非线程安全 |
| `uint32_t size() const` | 获取元素数量 | 非线程安全 |
| `static constexpr uint32_t capacity()` | 获取容量 | 线程安全 |
| `bool empty() const` | 检查是否为空 | 非线程安全 |
| `bool full() const` | 检查是否已满 | 非线程安全 |
| `T& operator[](uint32_t index)` | 访问元素 (无边界检查) | 非线程安全 |
| `T* data()` | 获取底层数组指针 | 非线程安全 |

**示例**:
```cpp
FixedVector<int, 8> vec;
vec.push_back(1);
vec.emplace_back(2);
for (uint32_t i = 0; i < vec.size(); ++i) {
    printf("%d ", vec[i]);
}
```

### ScopeGuard

RAII 清理守卫。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `explicit ScopeGuard(FixedFunction<void()> cleanup)` | 构造守卫，析构时执行清理回调 |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void release() noexcept` | 取消清理 (析构时不执行回调) | 非线程安全 |

**宏**:
```cpp
OSP_SCOPE_EXIT(statement);  // 作用域退出时执行 statement
```

**示例**:
```cpp
FILE* f = fopen("data.txt", "r");
OSP_SCOPE_EXIT(fclose(f));
// f 会在作用域结束时自动关闭
```

---

## 3. log.hpp - 日志系统

**概述**: 轻量级 printf 风格日志系统，支持级别过滤、分类和时间戳。

**头文件**: `include/osp/log.hpp`
**依赖**: `platform.hpp`

### Level 枚举

| 枚举值 | 说明 |
|--------|------|
| `kDebug` | 调试信息 (值 0) |
| `kInfo` | 一般信息 (值 1) |
| `kWarn` | 警告 (值 2) |
| `kError` | 错误 (值 3) |
| `kFatal` | 致命错误 (值 4，会调用 abort) |
| `kOff` | 关闭日志 (值 5) |

### 函数

| 函数签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Init(const char* conf_path = nullptr)` | 初始化日志系统 | 线程安全 |
| `void Shutdown()` | 关闭日志系统并刷新 stderr | 线程安全 |
| `Level GetLevel()` | 获取当前运行时最低日志级别 | 线程安全 |
| `void SetLevel(Level level)` | 设置运行时最低日志级别 | 线程安全 |
| `bool IsInitialized()` | 检查是否已初始化 | 线程安全 |

### 日志宏

| 宏名 | 说明 |
|------|------|
| `OSP_LOG_DEBUG(cat, fmt, ...)` | 输出 DEBUG 级别日志 |
| `OSP_LOG_INFO(cat, fmt, ...)` | 输出 INFO 级别日志 |
| `OSP_LOG_WARN(cat, fmt, ...)` | 输出 WARN 级别日志 |
| `OSP_LOG_ERROR(cat, fmt, ...)` | 输出 ERROR 级别日志 |
| `OSP_LOG_FATAL(cat, fmt, ...)` | 输出 FATAL 级别日志并 abort |

**参数**:
- `cat` - 分类字符串 (如 "Net", "Timer")
- `fmt` - printf 风格格式字符串
- `...` - 格式参数

**编译期过滤**:
定义 `OSP_LOG_MIN_LEVEL` 宏 (0-5) 可在编译期过滤低级别日志。

**示例**:
```cpp
osp::log::Init();
osp::log::SetLevel(osp::log::Level::kInfo);
OSP_LOG_INFO("App", "started with %d workers", 4);
OSP_LOG_ERROR("Net", "connection failed: %s", strerror(errno));
```

---

## 4. inicpp.hpp - INI 配置文件解析

**概述**: 轻量级 header-only INI 文件解析库，支持类型安全转换和自定义类型扩展。

**头文件**: `include/osp/inicpp.hpp`
**依赖**: 无 (header-only)
**命名空间**: `osp::ini`

### IniField 类

表示 INI 文件中的一个字段值。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `IniField()` | 构造空字段 |
| `IniField(const std::string& value)` | 从字符串构造 |

**类型转换**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T> T As() const` | 转换为指定类型 (支持 bool/int/float/double/string) | 非线程安全 |

**赋值操作**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T> IniField& operator=(const T& value)` | 从任意类型赋值 | 非线程安全 |

**示例**:
```cpp
osp::ini::IniField field;
field = 42;
int value = field.As<int>();  // 42
field = "hello";
std::string str = field.As<std::string>();  // "hello"
```

### IniSection / IniSectionBase

表示 INI 文件中的一个节 (section)，本质上是 `std::map<std::string, IniField>`。

**类型别名**:
- `IniSection` - 大小写敏感
- `IniSectionCaseInsensitive` - 大小写不敏感

**字段访问**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `IniField& operator[](const std::string& key)` | 访问字段 (不存在则创建) | 非线程安全 |
| `iterator find(const std::string& key)` | 查找字段 | 非线程安全 |
| `size_t size() const` | 获取字段数量 | 非线程安全 |

**示例**:
```cpp
osp::ini::IniSection section;
section["port"] = 8080;
section["enabled"] = true;
for (const auto& [key, field] : section) {
    printf("%s = %s\n", key.c_str(), field.As<std::string>().c_str());
}
```

### IniFile / IniFileBase

表示完整的 INI 文件，本质上是 `std::map<std::string, IniSection>`。

**类型别名**:
- `IniFile` - 大小写敏感
- `IniFileCaseInsensitive` - 大小写不敏感

**构造函数**:

| 签名 | 说明 |
|------|------|
| `IniFile()` | 构造空 INI 文件 |
| `IniFile(const std::string& filename)` | 从文件构造 |

**文件 I/O**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Load(const std::string& filename)` | 从文件加载 | 非线程安全 |
| `void Save(const std::string& filename) const` | 保存到文件 | 非线程安全 |

**字符串解析/编码**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Decode(const std::string& content)` | 从字符串解析 | 非线程安全 |
| `std::string Encode() const` | 编码为字符串 | 非线程安全 |

**节访问**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `IniSection& operator[](const std::string& section)` | 访问节 (不存在则创建) | 非线程安全 |
| `iterator find(const std::string& section)` | 查找节 | 非线程安全 |
| `size_t size() const` | 获取节数量 | 非线程安全 |

**配置选项**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void SetFieldSep(const char sep)` | 设置字段分隔符 (默认 '=') | 非线程安全 |
| `void SetCommentChar(const char comment)` | 设置注释字符 (默认 '#') | 非线程安全 |
| `void SetCommentPrefixes(const std::vector<std::string>& prefixes)` | 设置注释前缀列表 | 非线程安全 |
| `void SetEscapeChar(const char esc)` | 设置转义字符 (默认 '\\') | 非线程安全 |
| `void SetMultiLineValues(bool enable)` | 启用/禁用多行值 | 非线程安全 |
| `void AllowOverwriteDuplicateFields(bool allowed)` | 允许/禁止覆盖重复字段 | 非线程安全 |

**示例**:
```cpp
// 创建和保存
osp::ini::IniFile ini;
ini["Server"]["host"] = "127.0.0.1";
ini["Server"]["port"] = 8080;
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
```

### Convert<T> 模板

自定义类型转换扩展点。

**接口**:
```cpp
namespace osp {
namespace ini {

template <typename T>
struct Convert {
    void Decode(const std::string& value, T& result);
    void Encode(const T& value, std::string& result);
};

}  // namespace ini
}  // namespace osp
```

**示例 (std::vector 支持)**:
```cpp
namespace osp {
namespace ini {

template <typename T>
struct Convert<std::vector<T>> {
    void Decode(const std::string& value, std::vector<T>& result) {
        result.clear();
        T decoded;
        size_t startPos = 0, endPos = 0;
        while (endPos != std::string::npos) {
            if (endPos != 0) startPos = endPos + 1;
            endPos = value.find(',', startPos);
            size_t cnt = (endPos == std::string::npos)
                ? value.size() - startPos : endPos - startPos;
            std::string tmp = value.substr(startPos, cnt);
            Convert<T> conv;
            conv.Decode(tmp, decoded);
            result.push_back(decoded);
        }
    }

    void Encode(const std::vector<T>& value, std::string& result) {
        std::stringstream ss;
        for (size_t i = 0; i < value.size(); ++i) {
            std::string encoded;
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

### INI 文件格式

**基本语法**:
```ini
; 这是注释（分号）
# 这也是注释（井号）

[Section1]
key1 = value1
key2 = 123
key3 = true

[Section2]
path = /usr/local/bin
url = https://example.com:8080/path?key=value
```

**支持的值类型**:

| 类型 | 示例 | 说明 |
|------|------|------|
| 字符串 | `name=hello` | 任意文本 |
| 整数 | `port=8080` | 十进制、八进制(0755)、十六进制(0xFF) |
| 浮点数 | `timeout=30.5` | 支持科学计数法 |
| 布尔值 | `debug=true` | true/false (大小写不敏感) |

---

## 5. config.hpp - 配置解析

**概述**: 多格式配置文件解析器 (INI/JSON/YAML)，扁平化为 section+key=value 模型。

**头文件**: `include/osp/config.hpp`
**依赖**: `platform.hpp`, `vocabulary.hpp`

### ConfigFormat 枚举

| 枚举值 | 说明 |
|--------|------|
| `kAuto` | 自动检测格式 (根据文件扩展名) |
| `kIni` | INI 格式 |
| `kJson` | JSON 格式 |
| `kYaml` | YAML 格式 |

### ConfigError 枚举

| 枚举值 | 说明 |
|--------|------|
| `kFileNotFound` | 文件未找到 |
| `kParseError` | 解析错误 |
| `kFormatNotSupported` | 格式不支持 |
| `kBufferFull` | 条目缓冲区已满 |

### Config<Backends...> 类

多格式配置解析器 (模板参数化后端)。

**类型别名**:
- `MultiConfig` - 支持所有已启用的格式 (INI/JSON/YAML)
- `IniConfig` - 仅支持 INI (需 `OSP_CONFIG_INI_ENABLED`)
- `JsonConfig` - 仅支持 JSON (需 `OSP_CONFIG_JSON_ENABLED`)
- `YamlConfig` - 仅支持 YAML (需 `OSP_CONFIG_YAML_ENABLED`)

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `expected<void, ConfigError> LoadFile(const char* path, ConfigFormat format = kAuto)` | 加载配置文件 | 非线程安全 |
| `expected<void, ConfigError> LoadBuffer(const char* data, uint32_t size, ConfigFormat format)` | 从内存缓冲区加载 | 非线程安全 |
| `const char* GetString(const char* section, const char* key, const char* default_val = "")` | 获取字符串值 | 非线程安全 |
| `int32_t GetInt(const char* section, const char* key, int32_t default_val = 0)` | 获取整数值 | 非线程安全 |
| `uint16_t GetPort(const char* section, const char* key, uint16_t default_val = 0)` | 获取端口号 (0-65535) | 非线程安全 |
| `bool GetBool(const char* section, const char* key, bool default_val = false)` | 获取布尔值 | 非线程安全 |
| `double GetDouble(const char* section, const char* key, double default_val = 0.0)` | 获取浮点数值 | 非线程安全 |
| `optional<int32_t> FindInt(const char* section, const char* key)` | 查找整数值 (返回 optional) | 非线程安全 |
| `optional<bool> FindBool(const char* section, const char* key)` | 查找布尔值 (返回 optional) | 非线程安全 |
| `bool HasSection(const char* section)` | 检查 section 是否存在 | 非线程安全 |
| `bool HasKey(const char* section, const char* key)` | 检查 key 是否存在 | 非线程安全 |
| `uint32_t EntryCount() const` | 获取条目总数 | 非线程安全 |

**常量**:
- `OSP_CONFIG_MAX_FILE_SIZE` - 最大文件大小 (默认 8192 字节)

**示例**:
```cpp
osp::MultiConfig cfg;
auto result = cfg.LoadFile("app.yaml");
if (result) {
    int32_t port = cfg.GetInt("network", "port", 8080);
    bool debug = cfg.GetBool("app", "debug", false);
}
```

---

## 6. mem_pool.hpp - 内存池

**概述**: 固定块内存池，嵌入式空闲链表，零堆分配。

**头文件**: `include/osp/mem_pool.hpp`
**依赖**: `platform.hpp`, `vocabulary.hpp`

### MemPoolError 枚举

| 枚举值 | 说明 |
|--------|------|
| `kPoolExhausted` | 内存池已耗尽 |
| `kInvalidPointer` | 无效指�� |

### FixedPool<BlockSize, MaxBlocks>

原始块内存池。

**模板参数**:
- `BlockSize` - 块大小 (字节，>= sizeof(uint32_t))
- `MaxBlocks` - 最大块数

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void* Allocate()` | 分配一个块 | 线程安全 (mutex) |
| `expected<void*, MemPoolError> AllocateChecked()` | 分配一个块 (checked 版本) | 线程安全 (mutex) |
| `void Free(void* ptr)` | 释放一个块 | 线程安全 (mutex) |
| `bool OwnsPointer(const void* ptr) const` | 检查指针是否属于此池 | 线程安全 |
| `uint32_t FreeCount() const` | 获取空闲块数 | 线程安全 (mutex) |
| `uint32_t UsedCount() const` | 获取已用块数 | 线程安全 (mutex) |
| `static constexpr uint32_t Capacity()` | 获取总容量 | 线程安全 |
| `static constexpr uint32_t BlockSizeValue()` | 获取块大小 | 线程安全 |

**示例**:
```cpp
FixedPool<64, 128> pool;
void* ptr = pool.Allocate();
if (ptr) {
    // 使用内存
    pool.Free(ptr);
}
```

### ObjectPool<T, MaxObjects>

类型安全对象池 (placement new)。

**模板参数**:
- `T` - 对象类型
- `MaxObjects` - 最大对象数

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename... Args> T* Create(Args&&... args)` | 构造对象 | 线程安全 (mutex) |
| `template<typename... Args> expected<T*, MemPoolError> CreateChecked(Args&&... args)` | 构造对象 (checked 版本) | 线程安全 (mutex) |
| `void Destroy(T* obj)` | 销毁对象 | 线程安全 (mutex) |
| `bool OwnsPointer(const T* obj) const` | 检查指针是否属于此池 | 线程安全 |
| `uint32_t FreeCount() const` | 获取空闲槽位数 | 线程安全 (mutex) |
| `uint32_t UsedCount() const` | 获取已用对象数 | 线程安全 (mutex) |
| `static constexpr uint32_t Capacity()` | 获取总容量 | 线程安全 |

**示例**:
```cpp
struct Task { int id; };
ObjectPool<Task, 64> pool;
Task* t = pool.Create(42);
if (t) {
    printf("task id: %d\n", t->id);
    pool.Destroy(t);
}
```

---

## 7. semaphore.hpp - 信号量

**概述**: 计数信号量和二值信号量，支持 POSIX sem_t 包装。

**头文件**: `include/osp/semaphore.hpp`
**依赖**: `platform.hpp`

### SemaphoreError 枚举

| 枚举值 | 说明 |
|--------|------|
| `kTimeout` | 超时 |
| `kInterrupted` | 被信号中断 |
| `kInvalid` | 无效信号量 |

### LightSemaphore

基于 mutex + condition_variable 的计数信号量。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `explicit LightSemaphore(uint32_t initial_count = 0)` | 构造信号量，初始计数为 initial_count |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Signal()` | 增加计数并唤醒一个等待线程 | 线程安全 |
| `void Post()` | 同 `Signal()` | 线程安全 |
| `void Wait()` | 减少计数，若为 0 则阻塞 | 线程安全 |
| `bool TryWait()` | 非阻塞尝试减少计数 | 线程安全 |
| `bool WaitFor(uint64_t timeout_us)` | 超时等待 (微秒) | 线程安全 |
| `uint32_t Count() const` | 获取当前计数 (近似值) | 线程安全 |

**示例**:
```cpp
osp::LightSemaphore sem(0);
// 生产者线程:
sem.Signal();
// 消费者线程:
sem.Wait();
```

### BinarySemaphore

二值信号量 (最大计数为 1)。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `explicit BinarySemaphore(uint32_t initial_count = 0)` | 构造二值信号量，初始计数钳位到 0 或 1 |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Signal()` | 设置计数为 1 (钳位，不超过 1) | 线程安全 |
| `void Post()` | 同 `Signal()` | 线程安全 |
| `void Wait()` | 减少计数到 0，若为 0 则阻塞 | 线程安全 |
| `bool TryWait()` | 非阻塞尝试减少计数 | 线程安全 |
| `bool WaitFor(uint64_t timeout_us)` | 超时等待 (微秒) | 线程安全 |
| `uint32_t Count() const` | 获取当前计数 (0 或 1) | 线程安全 |

### PosixSemaphore (Linux/macOS)

POSIX sem_t 的 RAII 包装器。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `explicit PosixSemaphore(uint32_t initial_count = 0)` | 构造并初始化 POSIX 信号量 |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Post()` | 增加信号量 (sem_post) | 线程安全 |
| `void Wait()` | 减少信号量，阻塞直到非零 (sem_wait) | 线程安全 |
| `bool TryWait()` | 非阻塞尝试减少 (sem_trywait) | 线程安全 |
| `bool WaitFor(uint64_t timeout_us)` | 超时等待 (Linux: sem_timedwait, macOS: 轮询) | 线程安全 |
| `bool IsValid() const` | 检查信号量是否初始化成功 | 线程安全 |

---

## 8. spsc_ringbuffer.hpp - SPSC 环形缓冲

**概述**: 无锁、wait-free 单生产者单消费者环形缓冲，支持批量操作。

**头文件**: `include/osp/spsc_ringbuffer.hpp`
**依赖**: `platform.hpp`

### SpscRingbuffer<T, BufferSize, FakeTSO, IndexT>

**模板参数**:
- `T` - 元素类型 (trivially copyable 类型使用 memcpy 优化)
- `BufferSize` - 容量 (必须是 2 的幂)
- `FakeTSO` - 若为 true，使用 relaxed 内存序 (单核 MCU)
- `IndexT` - 索引类型 (默认 size_t)

**生产者 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool Push(const T& data)` | 推入一个元素 (拷贝) | 单生产者 |
| `bool Push(T&& data)` | 推入一个元素 (移动) | 单生产者 |
| `template<typename Callable> bool PushFromCallback(Callable&& callback)` | 仅在有空间时调用回调并推入 | 单生产者 |
| `size_t PushBatch(const T* buf, size_t count)` | 批量推入 | 单生产者 |
| `void ProducerClear()` | 生产者侧清空 (设置 head = tail) | 单生产者 |

**消费者 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool Pop(T& data)` | 弹出一个元素 | 单消费者 |
| `size_t PopBatch(T* buf, size_t count)` | 批量弹出 | 单消费者 |
| `size_t Discard(size_t count = 1)` | 丢弃元素而不读取 | 单消费者 |
| `T* Peek()` | 查看队首元素 (不移除) | 单消费者 |
| `T* At(size_t index)` | 访问第 n 个元素 (带边界检查) | 单消费者 |
| `const T& operator[](size_t index) const` | 访问第 n 个元素 (无边界检查) | 单消费者 |
| `void ConsumerClear()` | 消费者侧清空 (设置 tail = head) | 单消费者 |

**查询 API (任意侧)**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `IndexT Size() const` | 可读元素数 | 线程安全 |
| `IndexT Available() const` | 可写空闲槽位数 | 线程安全 |
| `bool IsEmpty() const` | 检查是否为空 | 线程安全 |
| `bool IsFull() const` | 检查是否已满 | 线程安全 |
| `static constexpr size_t Capacity()` | 获取容量 | 线程安全 |

**示例**:
```cpp
SpscRingbuffer<int, 1024> rb;
// 生产者线程:
rb.Push(42);
// 消费者线程:
int val;
if (rb.Pop(val)) {
    printf("got: %d\n", val);
}
```

---

## 9. bus.hpp - 消息总线

**概述**: 无锁 MPSC 消息总线，支持优先级准入控制和 topic 路由。

**头文件**: `include/osp/bus.hpp`
**依赖**: `platform.hpp`, `vocabulary.hpp`

### 编译期配置宏

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_BUS_QUEUE_DEPTH` | 4096 | 队列深度 (2 的幂) |
| `OSP_BUS_MAX_MESSAGE_TYPES` | 8 | 最大消息类型数 |
| `OSP_BUS_MAX_CALLBACKS_PER_TYPE` | 16 | 每类型最大回调数 |
| `OSP_BUS_BATCH_SIZE` | 256 | 批处理大小 |

### MessagePriority 枚举

| 枚举值 | 说明 |
|--------|------|
| `kLow` | 低优先级 (队列 >= 60% 时丢弃) |
| `kMedium` | 中优先级 (队列 >= 80% 时丢弃) |
| `kHigh` | 高优先级 (队列 >= 99% 时丢弃) |

### MessageHeader 结构体

| 字段 | 类型 | 说明 |
|------|------|------|
| `msg_id` | `uint64_t` | 消息 ID (全局递增) |
| `timestamp_us` | `uint64_t` | 时间戳 (微秒) |
| `sender_id` | `uint32_t` | 发送者 ID |
| `topic_hash` | `uint32_t` | Topic 哈希 (0 = 无 topic 过滤) |
| `priority` | `MessagePriority` | 优先级 |

### MessageEnvelope<PayloadVariant>

消息信封 (header + payload)。

| 字段 | 类型 | 说明 |
|------|------|------|
| `header` | `MessageHeader` | 消息头 |
| `payload` | `PayloadVariant` | 消息负载 (std::variant) |

### BusError 枚举

| 枚举值 | 说明 |
|--------|------|
| `kQueueFull` | 队列已满 |
| `kOverflowDetected` | 检测到溢出 |
| `kCallbacksFull` | 回调数组已满 |

### SubscriptionHandle 结构体

订阅句柄。

| 字段 | 类型 | 说明 |
|------|------|------|
| `type_index` | `uint32_t` | 类型索引 |
| `callback_id` | `uint32_t` | 回调 ID |

**方法**:

| 方法签名 | 说明 |
|---------|------|
| `bool IsValid() const` | 检查句柄是否有效 |
| `static SubscriptionHandle Invalid()` | 返回无效句柄 |

### AsyncBus<PayloadVariant, QueueDepth, BatchSize>

异步消息总线 (单例模式)。

**模板参数**:
- `PayloadVariant` - std::variant<...> 消息类型集合
- `QueueDepth` - 队列深度 (默认 `OSP_BUS_QUEUE_DEPTH`)
- `BatchSize` - 批处理大小 (默认 `OSP_BUS_BATCH_SIZE`)

**静态方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `static AsyncBus& Instance()` | 获取全局单例 | 线程安全 |

**发布 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool Publish(PayloadVariant&& payload, uint32_t sender_id)` | 发布消息 (默认 MEDIUM 优先级) | 线程安全 (无锁) |
| `bool PublishWithPriority(PayloadVariant&& payload, uint32_t sender_id, MessagePriority priority)` | 发布消息 (指定优先级) | 线程安全 (无锁) |
| `bool PublishTopic(PayloadVariant&& payload, uint32_t sender_id, const char* topic)` | 发布带 topic 的消息 | 线程安全 (无锁) |
| `bool PublishTopicWithPriority(PayloadVariant&& payload, uint32_t sender_id, const char* topic, MessagePriority priority)` | 发布带 topic 和优先级的消息 | 线程安全 (无锁) |

**订阅 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T, typename Func> SubscriptionHandle Subscribe(Func&& callback)` | 订阅类型 T 的消息 | 线程安全 (SpinLock) |
| `template<typename T, typename Func> SubscriptionHandle SubscribeTopic(const char* topic, Func&& callback)` | 订阅类型 T + topic 的消息 | 线程安全 (SpinLock) |
| `void Unsubscribe(SubscriptionHandle handle)` | 取消订阅 | 线程安全 (SpinLock) |

**处理 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `uint32_t ProcessBatch(uint32_t max_count = BatchSize)` | 批量处理消息 | 单消费者 |
| `void ProcessAll()` | 处理所有待处理消息 | 单消费者 |

**查询 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `uint32_t PendingCount() const` | 获取待处理消息数 | 线程��全 |
| `BackpressureLevel GetBackpressureLevel() const` | 获取背压级别 | 线程安全 |
| `BusStatisticsSnapshot GetStatistics() const` | 获取统计快照 | 线程安全 |
| `void ResetStatistics()` | 重置统计计数器 | 线程安全 |

**示例**:
```cpp
struct SensorData { float temp; };
struct MotorCmd { int speed; };
using MyPayload = std::variant<SensorData, MotorCmd>;
using MyBus = osp::AsyncBus<MyPayload>;

MyBus::Instance().Subscribe<SensorData>([](const auto& env) {
    auto* data = std::get_if<SensorData>(&env.payload);
    printf("temp: %f\n", data->temp);
});

MyBus::Instance().Publish(SensorData{25.0f}, 1);
MyBus::Instance().ProcessBatch();
```

---

## 10. node.hpp - 通信节点

**概述**: 轻量级节点抽象，提供命名身份和类型化 pub/sub。

**头文件**: `include/osp/node.hpp`
**依赖**: `bus.hpp`, `vocabulary.hpp`

### 配置常量

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_MAX_NODE_SUBSCRIPTIONS` | 16 | 每节点最大订阅数 |

### NodeError 枚举

| 枚举值 | 说明 |
|--------|------|
| `kAlreadyStarted` | 节点已启动 |
| `kNotStarted` | 节点未启动 |
| `kSubscriptionFull` | 订阅数组已满 |
| `kPublishFailed` | 发布失败 |

### Publisher<T, PayloadVariant>

类型化消息发布器。

**构造函数**:

| 签名 | 说明 |
|------|------|
| `Publisher()` | 构造空发布器 |
| `Publisher(AsyncBus<PayloadVariant>& bus, uint32_t sender_id)` | 构造绑定到 bus 的发布器 |

**公共方法**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool Publish(const T& msg)` | 发布消息 (默认 MEDIUM 优先级) | 线程安全 (无锁) |
| `bool Publish(T&& msg)` | 发布消息 (移动) | 线程安全 (无锁) |
| `bool PublishWithPriority(const T& msg, MessagePriority priority)` | 发布消息 (指定优先级) | 线程安全 (无锁) |
| `bool IsValid() const` | 检查发布器是否有效 | 非线程安全 |

### Node<PayloadVariant>

通信节点。

**模板参数**:
- `PayloadVariant` - std::variant<...> 消息类型集合

**构造函数**:

| 签名 | 说明 |
|------|------|
| `explicit Node(const char* name, uint32_t id = 0)` | 构造节点 (使用全局单例 bus) |
| `Node(const char* name, uint32_t id, BusType& bus)` | 构造节点 (注入 bus) |

**访问器**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `const char* Name() const` | 获取节点名称 | 非线程安全 |
| `uint32_t Id() const` | 获取节点 ID | 非线程安全 |
| `bool IsStarted() const` | 检查是否已启动 | 非线程安全 |
| `uint32_t SubscriptionCount() const` | 获取订阅数 | 非线程安全 |

**生命周期**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `expected<void, NodeError> Start()` | 启动节点 | 非线程安全 |
| `void Stop()` | 停止节点并取消所有订阅 | 非线程安全 |

**发布 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T> bool Publish(T&& msg)` | 发布消息 | 线程安全 (无锁) |
| `template<typename T> bool PublishWithPriority(T&& msg, MessagePriority priority)` | 发布消息 (指定优先级) | 线程安全 (无锁) |
| `template<typename T> bool Publish(T&& msg, const char* topic)` | 发布带 topic 的消息 | 线程安全 (无锁) |
| `template<typename T> bool PublishWithPriority(T&& msg, const char* topic, MessagePriority priority)` | 发布带 topic 和优先级的消息 | 线程安全 (无锁) |

**订阅 API**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T, typename Func> expected<void, NodeError> Subscribe(Func&& callback)` | 订阅类型 T 的消息 | 非线程安全 |
| `template<typename T, typename Func> expected<void, NodeError> SubscribeTopic(const char* topic, Func&& callback)` | 订阅类型 T + topic 的消息 | 非线程安全 |

**事件循环**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `uint32_t SpinOnce(uint32_t max_count = 256)` | 处理一批消息 | 单消费者 |
| `void SpinAll()` | 处理所有待处理消息 | 单消费者 |

**示例**:
```cpp
struct SensorData { float temp; };
using MyPayload = std::variant<SensorData>;

osp::Node<MyPayload> sensor("sensor_node", 1);
sensor.Subscribe<SensorData>([](const SensorData& d, const auto& h) {
    printf("temp=%f from sender %u\n", d.temp, h.sender_id);
});
sensor.Start();
sensor.Publish(SensorData{25.0f});
sensor.SpinOnce();
```

---

## 11. worker_pool.hpp - 工作线程池

**概述**: 基于 AsyncBus 的多工作线程池，支持优先级和 CPU 亲和性。

**头文件**: `include/osp/worker_pool.hpp`
**依赖**: `bus.hpp`, `spsc_ringbuffer.hpp`, `vocabulary.hpp`

### 配置常量

| 宏名 | 默认值 | 说明 |
|------|--------|------|
| `OSP_WORKER_QUEUE_DEPTH` | 1024 | 每工作线程队列深度 |

### WorkerPoolConfig 结构体

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | `FixedString<32>` | "pool" | 线程池名称 |
| `worker_num` | `uint32_t` | 1 | 工作线程数 |
| `priority` | `int32_t` | 0 | 线程优先级 |
| `cpu_set_size` | `uint32_t` | 0 | CPU 集合大小 (Linux) |
| `cpu_set` | `const cpu_set_t*` | nullptr | CPU 亲和性掩码 (Linux) |

### WorkerPoolError 枚举

| 枚举值 | 说明 |
|--------|------|
| `kFlushTimeout` | FlushAndPause 超时 |
| `kWorkerUnhealthy` | 一个或多个工作线程死亡 |

### WorkerPoolStats 结构体

| 字段 | 类型 | 说明 |
|------|------|------|
| `dispatched` | `uint64_t` | 已分发任务数 |
| `processed` | `uint64_t` | 已处理任务数 |
| `worker_queue_full` | `uint64_t` | 工作队列满次数 |
| `bus_stats` | `BusStatisticsSnapshot` | 总线统计快照 |

### WorkerPool<PayloadVariant>

工作线程池。

**模板参数**:
- `PayloadVariant` - std::variant<...> 任务类型集合

**构造函数**:

| 签名 | 说明 |
|------|------|
| `explicit WorkerPool(const WorkerPoolConfig& cfg)` | 构造线程池 |

**处理器注册**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T> void RegisterHandler(void (*handler)(const T&, const MessageHeader&))` | 注册函数指针处理器 | 非线程安全 (启动前调用) |
| `template<typename T, typename Func> void RegisterHandler(Func&& func)` | 注册可调用对象处理器 | 非线程安全 (启动前调用) |

**生命周期**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `void Start()` | 启动调度器和工作线程 | 非线程安全 |
| `void Shutdown()` | 关闭线程池 | 非线程安全 |
| `expected<void, WorkerPoolError> FlushAndPause(uint64_t timeout_us)` | 刷新并暂停 | 非线程安全 |
| `void Resume()` | 恢复运行 | 非线程安全 |

**任务提交**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `template<typename T> bool Submit(T&& task)` | 提交任务 (异步) | 线程安全 (无锁) |
| `template<typename T> bool SubmitWithPriority(T&& task, MessagePriority priority)` | 提交任务 (指定优先级) | 线程安全 (无锁) |
| `template<typename T> void SubmitSync(T&& task)` | 提交任务 (同步执行) | 线程安全 |

**查询**:

| 方法签名 | 说明 | 线程安全性 |
|---------|------|-----------|
| `bool IsRunning() const` | 检查是否运行中 | 线程安全 |
| `WorkerPoolStats GetStats() const` | 获取统计信息 | 线程安全 |

**示例**:
```cpp
struct TaskA { int id; };
using MyPayload = std::variant<TaskA>;

osp::WorkerPoolConfig cfg;
cfg.name = "demo";
cfg.worker_num = 4;

osp::WorkerPool<MyPayload> pool(cfg);
pool.RegisterHandler<TaskA>([](const TaskA& t, const auto&) {
    printf("task id: %d\n", t.id);
});
pool.Start();
pool.Submit(TaskA{1});
pool.Shutdown();
```

---

## 总结

本文档覆盖了 newosp 项目基础层和核心通信层的 11 个模块：

1. **platform.hpp** - 平台检测、编译器提示、断言、时钟工具
2. **vocabulary.hpp** - expected, optional, FixedFunction, FixedString, FixedVector, ScopeGuard
3. **log.hpp** - 轻量级日志系统
4. **inicpp.hpp** - INI 配置文件解析 (header-only)
5. **config.hpp** - 多格式配置解析 (INI/JSON/YAML)
6. **mem_pool.hpp** - 固定块内存池
7. **semaphore.hpp** - 计数/二值/POSIX 信号量
8. **spsc_ringbuffer.hpp** - 无锁 SPSC 环形缓冲
9. **bus.hpp** - 无锁 MPSC 消息总线
10. **node.hpp** - 轻量级通信节点
11. **worker_pool.hpp** - 工作线程池

所有模块均为 header-only，兼容 `-fno-exceptions -fno-rtti`，适用于嵌入式 ARM-Linux 平台。
