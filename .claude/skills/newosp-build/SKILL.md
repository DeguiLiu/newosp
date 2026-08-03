---
name: newosp-build
description: newosp 工程（纯头文件 C++17 嵌入式库）构建、测试与验证工作流。覆盖 CMake 配置、依赖本地化、单测试运行、ASan/UBSan/TSan 验证。当需要构建本工程、跑测试、验证修复时使用。
---

# newosp 构建与测试

ARM-Linux 纯头文件 C++17 库，测试框架 Catch2 v3。工程根目录：`/home/dgliu/newosp`。

## 依赖本地化（网络受限环境）

工程用 FetchContent 拉取 sockpp/catch2/nlohmann/fkYAML。网络不可达时 cmake 下载失败，需预先解压本地源并指向：

```bash
# 一次性下载（镜像加速）
mkdir -p /tmp/osp_deps && cd /tmp/osp_deps
curl -sL -o sockpp.zip "https://ghfast.top/https://github.com/DeguiLiu/sockpp/archive/refs/heads/master.zip" && unzip -q -o sockpp.zip && mv sockpp-master sockpp
curl -sL -o catch2.zip "https://ghfast.top/https://github.com/catchorg/Catch2/archive/refs/tags/v3.5.2.zip" && unzip -q -o catch2.zip && mv Catch2-3.5.2 catch2
```

## 构建

```bash
cd /home/dgliu/newosp
# 标准 Debug 构建（依赖在本地源时追加 FETCHCONTENT 指向）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DOSP_BUILD_TESTS=ON -DOSP_BUILD_EXAMPLES=OFF \
  -DFETCHCONTENT_SOURCE_DIR_SOCKPP=/tmp/osp_deps/sockpp \
  -DFETCHCONTENT_SOURCE_DIR_CATCH2=/tmp/osp_deps/catch2
cmake --build build -j$(nproc) --target osp_tests
```

## 运行测试

单二进制 `build/tests/osp_tests`，Catch2 按名字/标签过滤：

```bash
# 跑全部
ctest --test-dir build --output-on-failure
# 跑单个 TEST_CASE
./build/tests/osp_tests "AsyncBus publish and process"
# 跑某标签
./build/tests/osp_tests "[bus]"
```

## ASan 验证（UAF/越界）

`catch_discover_tests` 在 ASan 下 POST_BUILD 会失败，因此 ASan 用手动 g++ 编译单个测试文件：

```bash
g++ -std=c++17 -g -fsanitize=address -fno-omit-frame-pointer \
  -I/home/dgliu/newosp/include \
  -I/tmp/osp_deps/catch2/src \
  -I/home/dgliu/newosp/build_asan/_deps/catch2-build/generated-includes \
  /home/dgliu/newosp/tests/<test_file>.cpp \
  -L/home/dgliu/newosp/build_asan/_deps/catch2-build/src \
  -lCatch2Maind -lCatch2d -pthread -lrt \
  -o /tmp/<name>_asan
# 栈 UAF 需显式开启
ASAN_OPTIONS=detect_stack_use_after_return=1 /tmp/<name>_asan "TestName"
```

## 已知基线

- 预存在 2 个环境相关失败：`Subprocess working_dir`（环境）、`load from file constructor`（偶发），非修复范围。
- 头文件修改会触发所有依赖它的测试重编，构建较慢；测试文件修改只重编该文件。

## 关键约束

- 热路径零堆分配；违反即回归
- `-fno-exceptions -fno-rtti` 必须始终兼容（CI build-with-options job 验证）
- 所有修复先写失败测试（RED）再实现（GREEN），见 newosp-tdd skill
