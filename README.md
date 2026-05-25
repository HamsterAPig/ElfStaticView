# ElfStaticView

ElfStaticView 是一个离线 ELF / DWARF 静态变量浏览工具，提供图形界面与命令行两种入口，用于查看静态变量、导出快照以及辅助分析符号与地址信息。

## 当前能力

- 打开 ELF 文件并浏览静态变量结构
- 使用 GUI 方式查看变量树、日志、版本信息和更新状态
- 使用 CLI 扫描或导出分析结果
- 将当前分析结果导出为 JSON 快照，并重新载入快照
- 通过 `elf-static-view.yaml` 保存部分界面与更新检查配置

## 仓库结构

- `src/elf`：ELF / DWARF 读取与符号表相关实现
- `src/analysis`：分析模型与快照序列化
- `src/ui`：Dear ImGui 图形界面、配置读写、版本检查
- `src/logging`：日志入口
- `tests`：单元测试与 ELF fixture 生成逻辑
- `3rdparty`：第三方依赖子模块
- `.github/workflows`：GitHub Actions 发布流程

## 依赖与前提

构建前请先拉取子模块：

```powershell
git submodule update --init --recursive
```

项目当前使用 CMake + C++20，默认依赖以下组件：

- CMake 3.24+
- 支持 C++20 的编译器
- Git
- OpenGL 开发环境
- Ninja、Visual Studio 2022 或 MinGW Makefiles 生成器
- Python 3 解释器（测试 fixture patch 脚本使用标准库实现）

测试目标会在构建阶段生成 Linux ELF fixture。按照当前 `tests/CMakeLists.txt` 的真实实现，还需要本机 `PATH` 或 CMake cache 中可用的 `clang`、`clang++`、`gcc`、`g++`、`objcopy`、`llvm-objcopy`、`llvm-dwp` 和 `lld`。如工具不在 `PATH`，可在配置时显式传入 `-DELF_STATIC_VIEW_CLANG=...`、`-DELF_STATIC_VIEW_LLVM_OBJCOPY=...` 等 cache 变量。

## 构建

### 使用 Ninja

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build
```

### 使用 Visual Studio 2022

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## 测试

Ninja 构建目录：

```powershell
ctest --test-dir build --output-on-failure
```

Visual Studio Release 构建目录：

```powershell
ctest --test-dir build --build-config Release --output-on-failure
```

## 运行

默认直接启动 GUI：

```powershell
.\build\elf-static-view.exe
```

也可以显式指定 `ui` 命令并附带启动文件：

```powershell
.\build\elf-static-view.exe ui path\to\sample.elf
```

CLI 支持两个主要命令：

```powershell
.\build\elf-static-view-cli.exe scan path\to\sample.elf --show-runtime-only
.\build\elf-static-view-cli.exe dump path\to\sample.elf --format json --expand-depth 8
```

常用参数：

- `--show-runtime-only`：只保留运行期可见信息
- `--only-static-known`：只输出静态已知内容
- `--symbol <name>`：按符号名过滤
- `--expand-depth <n>`：控制展开深度
- `--address-bias <value>`：指定地址偏移
- `--format text|json`：控制 `dump` 输出格式

## 配置文件

程序会读取与可执行文件同目录的 `elf-static-view.yaml`。当前已经实际接线的配置项包括：

```yaml
updates:
  check_uri: https://api.github.com/repos/HamsterAPig/ElfStaticView/releases/latest
  repository_url: https://github.com/HamsterAPig/ElfStaticView

copy:
  address_base: hex
  strip_hex_prefix: false

ui:
  refresh_rate: 60

address_bias:
  write_back: false
```

说明：

- `updates.check_uri`：覆盖默认版本检查地址
- `updates.repository_url`：覆盖仓库主页地址
- `copy.address_base`：复制地址时使用 `hex`、`dec`、`oct`、`bin`
- `copy.strip_hex_prefix`：十六进制复制时是否去掉 `0x`
- `ui.refresh_rate`：界面刷新频率
- `address_bias.write_back`：是否将地址偏移写回配置
- `load_policy.enable_background_loading`：GUI 打开 ELF 时是否后台加载，默认开启
- `load_policy.default_static_storage_only`：是否默认只保留静态存储变量，默认开启
- `load_policy.exclude_formal_parameters`：是否默认跳过函数形参，默认开启
- `load_policy.exclude_runtime_only_variables`：是否默认跳过运行时变量，默认开启
- `load_policy.compile_unit_path_rules`：按编译单元源码路径过滤的 gitignore 风格规则
- `load_policy.max_expand_depth`：默认展开深度，默认 `6`；`0` 表示不限制展开层深
- `load_policy.lazy_expand_children`：是否启用子节点按需展开，默认开启；关闭后按 `max_expand_depth` 一次性构建子节点
- `load_policy.enable_parse_metrics`：是否展示解析指标，当前会在 Inspector 面板展示粗粒度耗时与过滤计数

示例：

```yaml
load_policy:
  enable_background_loading: true
  default_static_storage_only: true
  exclude_formal_parameters: true
  exclude_runtime_only_variables: true
  compile_unit_path_rules: |
    **/CMSIS/**
    **/HAL/**
    **/Drivers/**
    **/Middlewares/**
    !**/Core/**
    !**/App/**
  max_expand_depth: 6
  lazy_expand_children: true
  enable_parse_metrics: true
```

说明：

- `compile_unit_path_rules` 采用 gitignore 风格，支持 `*`、`**`、`?` 和 `!`
- 命中排除规则的编译单元会在 DWARF 遍历前直接跳过
- 默认策略面向嵌入式静态地址分析场景，优先缩短大 ELF 的加载时间

## 发布

仓库已包含 Windows 发布流程：`.github/workflows/release.yml` 会在推送 `v*` tag 时构建、测试并上传 GUI / CLI 可执行文件到 GitHub Release。

如果你需要手工整理发布说明，可参考 `CHANGELOG-v0.1.2-to-current.md`。

## 许可证

本仓库按 GPL-3.0-or-later 发布，详见根目录 `LICENSE`。

第三方依赖以子模块形式维护，许可证文件位于各自目录内，例如：

- `3rdparty/spdlog/LICENSE`
- `3rdparty/yaml-cpp/LICENSE`
- `3rdparty/libdwarf-code/src/lib/libdwarf/COPYING`
