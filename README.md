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
- Ninja 或 Visual Studio 2022 生成器

测试目标会在构建阶段生成 Linux ELF fixture。按照当前 `tests/CMakeLists.txt` 的真实实现，还需要本机可用的 `clang`、`clang++` 和 `lld`；在 Windows 下会优先尝试 `D:/ProgramData/ClionComplier/ClangLLVM/bin`。

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

## 发布

仓库已包含 Windows 发布流程：`.github/workflows/release.yml` 会在推送 `v*` tag 时构建、测试并上传 GUI / CLI 可执行文件到 GitHub Release。

如果你需要手工整理发布说明，可参考 `CHANGELOG-v0.1.2-to-current.md`。

## 许可证

本仓库按 GPL-3.0-or-later 发布，详见根目录 `LICENSE`。

第三方依赖以子模块形式维护，许可证文件位于各自目录内，例如：

- `3rdparty/spdlog/LICENSE`
- `3rdparty/yaml-cpp/LICENSE`
- `3rdparty/libdwarf-code/src/lib/libdwarf/COPYING`

