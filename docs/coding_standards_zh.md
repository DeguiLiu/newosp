# newosp 编码规范与质量保障

> 从 [design_zh.md](design_zh.md) 拆分
> 版本: 1.0
> 日期: 2026-02-14

---

## 1. 代码风格

基于 Google C++ Style Guide，使用 `.clang-format` 和 `CPPLINT.cfg`:

- 缩进 2 空格，行宽 120，Attach 花括号
- 指针左对齐 (`int* ptr`)，命名空间不缩进
- Include 排序: 主头文件 > 项目头文件 > C 封装 > C++ 标准库

## 2. 命名约定

| 元素 | 风格 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `AsyncBus`, `TimerScheduler` |
| 函数 (公有) | PascalCase | `Publish()`, `ProcessBatch()` |
| 变量/成员 | snake_case / snake_case_ | `sender_id`, `running_` |
| 常量/枚举值 | kPascalCase | `kCacheLineSize`, `kSuccess` |
| HSM 事件枚举 | k\<Module\>Evt\<Name\> | `kSvcEvtStart`, `kDiscEvtNodeFound` |
| 宏 | OSP_UPPER_CASE | `OSP_LOG_INFO`, `OSP_ASSERT` |
| 模板参数 | PascalCase | `PayloadVariant`, `BlockSize` |

## 3. CI 流水线

| 阶段 | 内容 |
|------|------|
| build-and-test | Ubuntu, Debug + Release |
| build-with-options | `-fno-exceptions -fno-rtti` 兼容性 |
| sanitizers | ASan, TSan, UBSan |
| code-quality | clang-format + cpplint |

## 4. 测试策略

- 框架: Catch2 v3.5.2
- 每模块独立测试文件: `test_<module>.cpp`
- 覆盖目标: 基础 API + 边界条件 + 多线程场景
- Sanitizer 验证: 所有测试在 ASan/TSan/UBSan 下通过
- 当前: 758+ test cases
