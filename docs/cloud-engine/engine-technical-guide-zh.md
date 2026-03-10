# OrcaSlicer 云端切片引擎：技术指南

**版本：** 01.10.01.50
**分支：** engine-build
**最后更新：** 2026-03-10

---

## 目录

1. [概述](#1-概述)
2. [快速开始](#2-快速开始)
3. [命令行接口](#3-命令行接口)
4. [核心切片流程](#4-核心切片流程)
5. [盘布局系统](#5-盘布局系统)
6. [构建系统](#6-构建系统)
7. [引擎与GUI对比](#7-引擎与gui对比)
8. [API集成指南](#8-api集成指南)
9. [故障排查](#9-故障排查)
10. [开发指南](#10-开发指南)

---

## 1. 概述

### 1.1 简介

`orca-slice-engine` 是从 OrcaSlicer 项目中提取的无头命令行切片工具。它接受包含3D模型和嵌入式切片配置的3MF文件，运行完整的 OrcaSlicer 切片管道，并在无需任何图形环境的情况下生成G代码输出。

该引擎专为服务端和云端部署场景设计，适用于：
- 无显示器或桌面环境的环境
- 必须通过REST API或作业队列以编程方式触发切片
- 需要可重现、确定性的G代码输出
- 多个切片作业必须在容器化环境中并发运行

### 1.2 设计目标

| 目标 | 描述 |
|------|------|
| 无头运行 | 无GUI、无显示服务器、无需OpenGL上下文 |
| 配置嵌入 | 所有切片设置从输入3MF文件读取 |
| 多盘支持 | 按ID切片单个盘或一次性切片所有盘 |
| GUI一致性 | G代码输出与GUI输出在数学上完全相同 |
| 跨平台 | 在Linux x64、macOS和Windows上构建和运行 |
| 最小占用 | 无wxWidgets、无OpenGL；仅libslic3r和Boost |

### 1.3 与GUI应用程序的关系

引擎和GUI应用程序共享相同的核心切片库（`libslic3r`）。所有切片算法、G代码生成逻辑和配置处理都是相同的。引擎不是重新实现——它是围绕GUI使用的相同 `Print::process()` 和 `Print::export_gcode()` 调用的薄命令行包装器。

```
┌─────────────────────────────────────────────────────────────┐
│                    OrcaSlicer 项目                          │
│                                                             │
│  ┌─────────────────────┐    ┌──────────────────────────┐   │
│  │  GUI 应用程序       │    │  云端切片引擎             │   │
│  │  (Snapmaker_Orca)   │    │  (orca-slice-engine)      │   │
│  │                     │    │                           │   │
│  │  wxWidgets          │    │  CLI 参数解析器           │   │
│  │  OpenGL 渲染        │    │  进度报告器               │   │
│  │  交互式 UI          │    │  3MF 打包器               │   │
│  └──────────┬──────────┘    └────────────┬──────────────┘   │
│             │                            │                  │
│             └──────────┬─────────────────┘                  │
│                        ▼                                    │
│           ┌────────────────────────┐                        │
│           │       libslic3r        │                        │
│           │  (共享核心库)          │                        │
│           │                        │                        │
│           │  Print::process()      │                        │
│           │  Print::export_gcode() │                        │
│           │  Model::read_from_file │                        │
│           │  store_bbs_3mf()       │                        │
│           └────────────────────────┘                        │
└─────────────────────────────────────────────────────────────┘
```

**关键点：** `libslic3r` 中的任何切片算法更改或错误修复都会自动应用于 GUI 和引擎。

---

## 2. 快速开始

### 2.1 前置要求

| 组件 | 要求 |
|------|------|
| CMake | 3.13 或更高版本（Windows 上最高 3.31.x） |
| C++ 编译器 | 支持 C++17（GCC 7+、Clang 5+、MSVC 2019+） |
| 构建工具 | Ninja（推荐）或 Make |
| 操作系统 | Linux（Ubuntu 20.04+）、macOS 12+、Windows 10+ |

### 2.2 从源代码构建

#### 步骤 1：克隆仓库

```bash
git clone https://github.com/Snapmaker/OrcaSlicer.git
cd OrcaSlicer
git checkout engine-build
```

#### 步骤 2：构建依赖项（无 GUI）

**Linux:**
```bash
mkdir -p deps/build
cmake -S deps -B deps/build -G Ninja \
  -DFLATPAK=ON \
  -DDESTDIR="$(pwd)/deps/build/destdir"
cmake --build deps/build
```

**macOS:**
```bash
mkdir -p deps/build
cmake -S deps -B deps/build \
  -DFLATPAK=ON \
  -DDESTDIR="$(pwd)/deps/build/destdir"
cmake --build deps/build
```

**Windows (Visual Studio 2022):**
```bash
mkdir deps\build
cmake -S deps -B deps\build -G "Visual Studio 17 2022" -A x64 ^
  -DDESTDIR="%CD%\deps\build\destdir"
cmake --build deps\build --config Release
```

#### 步骤 3：配置并构建引擎

```bash
# Linux / macOS
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(pwd)/deps/build/destdir" \
  -DSLIC3R_STATIC=1 \
  -DSLIC3R_GUI=OFF \
  -DORCA_TOOLS=OFF \
  -DBUILD_TESTS=OFF

cmake --build build --target orca-slice-engine -j$(nproc)
```

```bash
# Windows
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH="%CD%\deps\build\destdir" ^
  -DSLIC3R_STATIC=1 ^
  -DSLIC3R_GUI=OFF
cmake --build build --target orca-slice-engine --config Release
```

编译后的二进制文件位于：
- Linux/macOS: `build/src/orca-slice-engine/orca-slice-engine`
- Windows: `build\src\orca-slice-engine\Release\orca-slice-engine.exe`

### 2.3 基本使用示例

```bash
# 切片项目中的所有盘 → 输出 model.gcode.3mf
./orca-slice-engine model.3mf

# 仅切片盘 1 → 输出 model-p1.gcode.3mf
./orca-slice-engine model.3mf -p 1

# 将盘 1 切片为纯 G 代码 → 输出 model-p1.gcode
./orca-slice-engine model.3mf -p 1 -f gcode

# 指定自定义输出路径 → 输出 /tmp/result.gcode.3mf
./orca-slice-engine model.3mf -o /tmp/result

# 使用特定的资源目录
./orca-slice-engine model.3mf -r /opt/orca/resources

# 启用详细日志以进行调试
./orca-slice-engine model.3mf -v
```

### 2.4 Docker 部署示例

**Dockerfile:**
```dockerfile
FROM ubuntu:22.04

# 安装最小运行时依赖
RUN apt-get update && apt-get install -y \
    libgl1-mesa-glx \
    libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

# 复制引擎及其捆绑库
COPY orca-slice-engine /usr/local/bin/
COPY lib/ /usr/local/lib/
COPY resources/ /opt/orca/resources/

# 设置库路径和资源
ENV ORCA_RESOURCES=/opt/orca/resources
ENV LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

ENTRYPOINT ["orca-slice-engine"]
```

**构建并运行：**
```bash
# 构建 Docker 镜像
docker build -t orca-slice-engine .

# 切片所有盘
docker run --rm \
  -v "$(pwd)/models:/data" \
  orca-slice-engine /data/model.3mf -o /data/output

# 切片单个盘
docker run --rm \
  -v "$(pwd)/models:/data" \
  orca-slice-engine /data/model.3mf -p 1 -o /data/output
```

---

## 3. 命令行接口

### 3.1 语法

```
orca-slice-engine input.3mf [OPTIONS]
```

### 3.2 选项参考

| 选项 | 长格式 | 参数 | 默认值 | 描述 |
|------|--------|------|--------|------|
| `-o` | `--output` | `<path>` | (从输入派生) | 输出文件基础路径，不含扩展名 |
| `-p` | `--plate` | `<id\|all>` | `all` | 要切片的盘 ID（1、2、3...）或 `all` |
| `-f` | `--format` | `<fmt>` | `gcode.3mf` | 输出格式：`gcode` 或 `gcode.3mf` |
| `-r` | `--resources` | `<dir>` | 自动检测 | 资源目录路径 |
| `-t` | `--timestamp` | `<ts>` | 系统时间 | 用于可重现输出的固定时间戳 |
| `-v` | `--verbose` | — | 关闭 | 启用跟踪级别日志 |
| `-h` | `--help` | — | — | 显示帮助信息并退出 |

### 3.3 选项详情

#### `-o, --output <path>`

指定输出文件路径基础。根据格式和盘选择，会自动附加适当的扩展名（`.gcode` 或 `.gcode.3mf`）。

省略时，输出文件将放置在与输入文件相同的目录中，使用输入文件的主干作为基础名称。

```bash
# 输入：/data/my_model.3mf
# 盘 1 的默认输出：/data/my_model-p1.gcode.3mf
./orca-slice-engine /data/my_model.3mf -p 1

# 显式输出：/output/result.gcode.3mf
./orca-slice-engine /data/my_model.3mf -p 1 -o /output/result
```

#### `-p, --plate <id>`

选择要切片的盘。使用正整数（1、2、3...）表示特定盘，或使用 `all`（或完全省略该选项）切片所有盘。

引擎会验证请求的盘 ID 是否存在于 3MF 文件中。如果不存在，它会列出所有可用的盘 ID 并以代码 1 退出。

```bash
./orca-slice-engine model.3mf -p 1      # 仅盘 1
./orca-slice-engine model.3mf -p all    # 所有盘（显式）
./orca-slice-engine model.3mf           # 所有盘（默认）
```

#### `-f, --format <fmt>`

支持两种格式：

| 格式 | 描述 | 使用场景 |
|------|------|----------|
| `gcode` | 纯文本 G 代码文件 | 单盘；直接固件上传 |
| `gcode.3mf` | 带嵌入式 G 代码的 3MF 容器 | 多盘；元数据保留；Snapmaker 工作流 |

**约束：** 切片所有盘时（`-p all` 或省略 `-p`），无论 `-f` 如何，格式都会强制为 `gcode.3mf`。在全盘切片时指定 `-f gcode` 会导致错误。

#### `-r, --resources <dir>`

引擎需要来自 `resources/` 目录的打印机和材料配置文件。解析顺序：

1. `-r` 命令行参数（最高优先级）
2. `ORCA_RESOURCES` 环境变量
3. 可执行文件旁边的 `resources/` 子目录（自动检测）

引擎在验证资源路径时会检查 `profiles/` 和 `printers/` 子目录。

#### `-t, --timestamp <ts>`

设置嵌入在 G 代码头部的固定时间戳，覆盖系统时钟。这使得可重现、确定性的 G 代码输出对测试和 CI 验证很有用。

格式：`YYYY-MM-DD HH:MM:SS`

```bash
./orca-slice-engine model.3mf -t "2024-01-01 12:00:00"
```

### 3.4 输出文件命名规则

| 命令 | 输出文件 |
|------|----------|
| `engine model.3mf` | `model.gcode.3mf` |
| `engine model.3mf -p 1` | `model-p1.gcode.3mf` |
| `engine model.3mf -p 1 -f gcode` | `model-p1.gcode` |
| `engine model.3mf -p 1 -o /out/r` | `/out/r.gcode.3mf` |
| `engine model.3mf -p 1 -o /out/r -f gcode` | `/out/r.gcode` |
| `engine model.3mf -o /out/r` | `/out/r.gcode.3mf` |

### 3.5 退出代码

| 代码 | 常量 | 含义 |
|------|------|------|
| `0` | `EXIT_OK` | 成功 — 输出文件已写入 |
| `1` | `EXIT_INVALID_ARGS` | 无效参数（错误的盘 ID、冲突的格式、未知选项） |
| `2` | `EXIT_FILE_NOT_FOUND` | 输入文件不存在 |
| `3` | `EXIT_LOAD_ERROR` | 解析或加载 3MF 文件失败 |
| `4` | `EXIT_SLICING_ERROR` | 至少一个盘的切片失败 |
| `5` | `EXIT_EXPORT_ERROR` | 写入输出 G 代码或包失败 |

退出代码 `4` 会在第一个失败的盘上立即中止。成功处理的盘的临时文件会在退出前删除。

### 3.6 进度输出

引擎在切片期间向标准输出写入结构化进度消息：

```
[Progress] 25% - Processing layers
[Status] Generating support material
[Progress] 50% - Generating G-code
[Progress] 100% - Done
```

格式：
- `[Progress] <N>% - <text>` — 带百分比的量化进度
- `[Status] <text>` — 无百分比的定性状态更新

切片多个盘时，每个盘前面都有一个分隔符：

```
=== Processing plate 1 ===
[Progress] 10% - ...
=== Processing plate 2 ===
[Progress] 10% - ...
```

日志消息（来自 Boost.Log）会输出到标准输出，带有严重性前缀：`[info]`、`[warning]`、`[error]`、`[debug]`（仅详细模式）。

---

## 4. 核心切片流程

引擎为每次调用执行七个阶段的线性流程。对于多盘作业，阶段 4-6 会为每个盘重复。

```
┌─────────────────────────────────────────────────────────────────┐
│  阶段 1：参数解析                                               │
│  验证 CLI 参数、解析路径、选择输出格式                          │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  阶段 2：3MF 加载                                               │
│  Model::read_from_file() — 加载几何体 + 嵌入式配置              │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  阶段 3：盘准备                                                 │
│  识别盘、验证盘 ID、计算盘原点                                  │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  阶段 4：切片  [每个盘]                                         │
│  设置可打印标志、Print::apply()、Print::process()               │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  阶段 5：G 代码导出  [每个盘]                                   │
│  Print::export_gcode() — 生成 .gcode 到临时文件                 │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  阶段 6：3MF 打包  (仅 gcode.3mf 格式)                         │
│  store_bbs_3mf() — 将所有 G 代码打包到输出容器                  │
└───────────────────────────────┬─────────────────────────────────┘
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  阶段 7：清理                                                   │
│  删除临时 .gcode 文件，使用 quick_exit(0) 退出                  │
└─────────────────────────────────────────────────────────────────┘
```

### 4.1 阶段 1：参数解析 (main.cpp:133-234)

所有命令行参数在一次遍历中解析。验证检查包括：

- 输入文件的存在性和磁盘上的存在
- 盘 ID：必须是正整数或 `"all"`
- 格式/盘组合：`gcode` 格式与全盘切片不兼容
- 未知选项：触发使用消息和退出代码 1

参数解析后，格式会自动更正：如果未指定盘且未显式设置格式，则选择 `gcode.3mf`。

### 4.2 阶段 2：3MF 加载 (main.cpp:299-373)

引擎使用以下策略标志调用 `Model::read_from_file()`：

```cpp
LoadStrategy strategy = LoadStrategy::LoadModel    // 加载 3D 网格几何体
                      | LoadStrategy::LoadConfig   // 加载嵌入式切片配置
                      | LoadStrategy::AddDefaultInstances  // 确保实例存在
                      | LoadStrategy::LoadAuxiliary;       // 加载辅助数据
```

此调用填充：
- `model` — 所有带有网格数据的 `ModelObject` 实例
- `config` — 带有所有切片设置的 `DynamicPrintConfig`
- `plate_data` — 描述每个盘对象分配的 `PlateData*` 向量
- `project_presets` — 嵌入在 3MF 中的预设
- `file_version` — 原始 OrcaSlicer 版本的 `Semver`

**回退行为：** 如果 3MF 不包含盘数据（例如，简单的非 BBL 3MF），引擎会创建一个带有 `plate_index = 1` 的合成 `PlateData` 条目，将整个模型表示为一个盘。

### 4.3 阶段 3：盘准备 (main.cpp:375-519)

此阶段处理盘选择和原点计算。

**盘选择：** 向量 `plates_to_process` 填充要切片的盘的 ID。对于单盘模式，它包含一个元素。对于全盘模式，它从 `plate_data` 中的每个条目填充。

**盘原点计算：** 对于要处理的每个盘，引擎使用与 GUI 的 `PartPlateList::compute_shape_position()` 相同的公式计算 3D 原点向量。有关完整算法，请参见[第 5 节](#5-盘布局系统)。

**可打印实例过滤：** 对于每个盘，引擎仅将属于该盘的实例标记为可打印。这是通过将 `ModelInstance::loaded_id` 与存储在 `PlateData::obj_inst_map` 中的 `identify_id` 值进行匹配来完成的：

```cpp
inst->printable = on_current_plate;
inst->print_volume_state = on_current_plate
    ? ModelInstancePVS_Inside
    : ModelInstancePVS_Fully_Outside;
```

### 4.4 阶段 4：切片 (main.cpp:444-556)

为每个盘创建一个新的 `Print` 对象。它配置为：
- `print.set_plate_index(plate_index)` — 基于 0 的盘索引
- `print.set_plate_origin(plate_origin)` — 计算的 3D 原点
- `print.set_gcode_timestamp(custom_timestamp)` — 可选的固定时间戳

然后运行切片流程：

```cpp
// 将配置和模型应用于打印
auto apply_status = print.apply(model, config);

// 执行完整的切片流程
print.process();
```

`libslic3r/Print.cpp` 中的 `Print::process()` 协调所有切片步骤：层生成、周边生成、填充、支撑材料、冷却和 G 代码序列规划。

**错误处理：** `process()` 抛出的任何 `std::exception` 都会导致立即清理所有临时文件并以代码 `EXIT_SLICING_ERROR` (4) 退出。

### 4.5 阶段 5：G 代码导出 (main.cpp:560-598)

切片后，通过以下方式导出 G 代码：

```cpp
std::string result = print.export_gcode(gcode_output, &slice_result.gcode_result, nullptr);
```

对于 `gcode.3mf` 格式（或多盘），G 代码会写入系统临时目录中的临时文件：

```
/tmp/plate_<id>.gcode
```

对于单盘 `gcode` 格式，G 代码直接写入最终输出路径。

导出后，引擎从 `Print` 对象收集：
- `PrintStatistics::total_weight` — 耗材重量（克）
- `Print::is_support_used()` — 是否生成了任何支撑
- `GCodeProcessorResult` — 打印时间估计、缩略图数据、延时摄影标志

### 4.6 阶段 6：3MF 打包 (main.cpp:601-791)

当输出格式为 `gcode.3mf` 时，所有每个盘的 G 代码文件都使用 `store_bbs_3mf()` 组装到单个 3MF 容器中。

引擎在打包前为每个 `PlateData` 条目预填充切片元数据：

| 字段 | 来源 |
|------|------|
| `gcode_file` | 临时 .gcode 文件的路径 |
| `is_sliced_valid` | `true` |
| `printer_model_id` | 来自配置 `printer_model` |
| `nozzle_diameters` | 来自配置 `nozzle_diameter` |
| `gcode_prediction` | 来自 `GCodeProcessorResult` 的打印时间（秒） |
| `gcode_weight` | 来自 `PrintStatistics` 的耗材重量 |
| `toolpath_outside` | 来自 `GCodeProcessorResult` |
| `timelapse_warning_code` | 来自 `GCodeProcessorResult` |
| `is_support_used` | 来自 `Print::is_support_used()` |
| `is_label_object_enabled` | 来自 `GCodeProcessorResult` |
| `slice_filaments_info` | 通过 `pd->parse_filament_info()` 解析 |

保存策略标志与 GUI 行为匹配：

```cpp
params.strategy = SaveStrategy::Silence      // 抑制详细输出
                | SaveStrategy::SplitModel   // 匹配 GUI 导出行为
                | SaveStrategy::WithGcode    // 包含 G 代码文件
                | SaveStrategy::SkipModel    // 减小大小（无网格数据）
                | SaveStrategy::Zip64;       // 支持大文件（>4 GB）
```

**缩略图处理：** 引擎传递空的缩略图数据向量。GUI 通过 OpenGL 渲染生成缩略图，这在无头模式下不可用。生成的 `gcode.3mf` 具有所有元数据但没有预览图像。

### 4.7 阶段 7：清理 (main.cpp:796-809)

在 `temp_files` 向量中跟踪的所有临时 `.gcode` 文件都会被删除。然后进程通过 `std::quick_exit(EXIT_OK)` 终止，绕过第三方库中可能不稳定的全局析构函数。

---

## 5. 盘布局系统

### 5.1 概念

OrcaSlicer 盘系统在虚拟 2D 网格中排列多个打印床。每个盘占据一个矩形区域，相邻盘之间有 20% 的间隙。每个模型实例的世界坐标都是相对于其盘在此网格中的原点定义的。

G 代码坐标系是绝对的，因此引擎必须计算每个盘的原点并在 G 代码生成期间应用它。引擎的盘布局逻辑在数学上与 GUI 的 `PartPlateList` 相同。

### 5.2 网格布局算法

盘排列在近似正方形的网格中。列数从项目中的盘总数计算：

```cpp
inline int compute_colum_count(int count) {
    float value = sqrt((float)count);
    float round_value = round(value);
    return (value > round_value) ? (round_value + 1) : round_value;
}
```

列数示例：

| 总盘数 | 列数 | 布局 |
|--------|------|------|
| 1 | 1 | 1×1 |
| 2 | 2 | 1×2 |
| 3 | 2 | 2×2（最后一个单元格为空） |
| 4 | 2 | 2×2 |
| 5 | 3 | 2×3（最后一个单元格为空） |
| 6 | 3 | 2×3 |
| 9 | 3 | 3×3 |

3 盘项目的可视化布局：

```
┌──────────────┬──────────────┐
│              │              │
│   盘 0      │   盘 1      │  行 0
│  (index=0)  │  (index=1)  │
│              │              │
├──────────────┼──────────────┤
│              │              │
│   盘 2      │   (空)      │  行 1
│  (index=2)  │             │
│              │              │
└──────────────┴──────────────┘
    列 0            列 1

  plate_stride_x ─────────────▶
```

### 5.3 盘原点计算

给定盘的基于 0 的索引，原点计算如下：

**步骤 1：从配置确定盘尺寸**

```cpp
// 从 printable_area 配置读取边界框
BoundingBoxf bbox;
for (const Vec2d& pt : printable_area_opt->values) {
    bbox.merge(pt);
}

// 减去床轴尖端半径以匹配 GUI
const double BED_AXES_TIP_RADIUS = 2.5 * 0.5;  // = 1.25 mm
double plate_width = bbox.size().x() - BED_AXES_TIP_RADIUS;
double plate_depth = bbox.size().y() - BED_AXES_TIP_RADIUS;
```

**步骤 2：计算带 20% 间隙的步幅**

```cpp
const double LOGICAL_PART_PLATE_GAP = 0.2;
double plate_stride_x = plate_width  * (1.0 + LOGICAL_PART_PLATE_GAP);
double plate_stride_y = plate_depth  * (1.0 + LOGICAL_PART_PLATE_GAP);
```

**步骤 3：将索引映射到行/列**

```cpp
// 关键：使用 3MF 中的总盘数，而不是正在处理的盘数
const int plate_cols = compute_colum_count(static_cast<int>(plate_data.size()));
int row = plate_index / plate_cols;
int col = plate_index % plate_cols;
```

**步骤 4：计算原点向量**

```cpp
Vec3d plate_origin(
    col * plate_stride_x,    // X：向右列为正
    -row * plate_stride_y,   // Y：向下行为负
    0                        // Z：始终为零
);
```

**示例（3 个盘，270×270 mm 床）：**

| 盘索引 | 行 | 列 | 原点 X | 原点 Y |
|--------|----|----|--------|--------|
| 0 | 0 | 0 | 0.0 | 0.0 |
| 1 | 0 | 1 | 322.5 | 0.0 |
| 2 | 1 | 0 | 0.0 | -322.5 |

其中：`plate_size = 270 - 1.25 = 268.75 mm`，`stride = 268.75 × 1.2 = 322.5 mm`

### 5.4 GUI 参考实现

引擎实现直接镜像 GUI 的 `PartPlateList::compute_shape_position()`：

```cpp
// GUI: src/slic3r/GUI/PartPlate.cpp:3297-3309
Vec2d PartPlateList::compute_shape_position(int index, int cols)
{
    int row = index / cols;
    int col = index % cols;
    pos(0) = col * plate_stride_x();
    pos(1) = -row * plate_stride_y();
    return pos;
}
```

### 5.5 盘对齐修复（2026-03-10）

在 `main.cpp:493` 中修复了一个关键错误。该错误在从多盘项目切片单个盘时导致 G 代码坐标偏移约 ±324 mm。

**根本原因：** 引擎错误地使用 `plates_to_process.size()`（正在切片的盘数）而不是 `plate_data.size()`（项目中的总盘数）来计算 `plate_cols`。

**错误代码：**
```cpp
const int plate_cols = compute_colum_count(
    static_cast<int>(plates_to_process.size())  // 错误
);
```

**更正代码：**
```cpp
// 关键：使用源 3MF 中的总盘数，而不是 plates_to_process.size()
const int plate_cols = compute_colum_count(
    static_cast<int>(plate_data.size())  // 正确
);
```

**为什么这很重要：** 考虑一个 3 盘项目，其中仅切片盘 1（索引 0）：

| 变量 | 错误 | 正确 |
|------|------|------|
| `plate_cols` | `compute_colum_count(1)` = **1** | `compute_colum_count(3)` = **2** |
| 盘 1 行 | `0 / 1 = 0` | `0 / 2 = 0` |
| 盘 1 列 | `0 % 1 = 0` | `0 % 2 = 0` |
| 盘 1 原点 | (0, 0) ✅（巧合正确） | (0, 0) ✅ |
| 盘 2 行 | `1 / 1 = 1` ❌ | `1 / 2 = 0` ✅ |
| 盘 2 列 | `1 % 1 = 0` ❌ | `1 % 2 = 1` ✅ |
| 盘 2 原点 | **(0, -322.5)** ❌ | **(322.5, 0)** ✅ |

有关完整详细信息和测试脚本，请参见 [plate-alignment-fix.md](./plate-alignment-fix.md)。

---

## 6. 构建系统

### 6.1 CMake 配置

通过设置 `SLIC3R_GUI=OFF` 启用引擎。这在 `src/CMakeLists.txt` 中强制执行：

```cmake
# src/CMakeLists.txt:22-24
if (NOT SLIC3R_GUI)
    add_subdirectory(orca-slice-engine)
endif()
```

当 `SLIC3R_GUI=ON`（GUI 构建）时，引擎目标不会被编译。

**引擎构建的关键 CMake 选项：**

| 选项 | 推荐值 | 效果 |
|------|--------|------|
| `SLIC3R_GUI` | `OFF` | 排除 wxWidgets、OpenGL、GUI 代码 |
| `SLIC3R_STATIC` | `1`（Linux/macOS） | 静态链接依赖项以提高可移植性 |
| `ORCA_TOOLS` | `OFF` | 排除配置文件验证器工具 |
| `BUILD_TESTS` | `OFF` | 排除测试套件 |
| `FLATPAK` | `ON`（依赖项构建） | 从依赖项构建中排除 wxWidgets |

### 6.2 引擎 CMakeLists.txt

引擎目标（`src/orca-slice-engine/CMakeLists.txt`）是最小的：

**源文件：**
- `main.cpp` — 整个引擎逻辑（约 810 行）
- `nanosvg.cpp` — SVG 光栅化实现（无头构建中 libslic3r 需要）

**所需链接库：**

| 库 | 用途 |
|----|------|
| `libslic3r` | 核心切片引擎 |
| `cereal::cereal` | 序列化框架 |
| `boost_libs` | 文件系统、日志、nowide（Windows 上的 Unicode 参数） |
| `TBB::tbb` | 并行切片（Intel Threading Building Blocks） |
| `TBB::tbbmalloc` | TBB 优化的内存分配器 |
| `nanosvg` | 缩略图和床形状的 SVG 支持 |

**平台特定库：**

| 平台 | 附加库 |
|------|--------|
| Windows | `Psapi.lib`、`bcrypt.lib` |
| macOS | `libiconv`、IOKit、CoreFoundation 框架 |
| Linux | `libdl`、`pthreads` |

**无 GUI 依赖项：**
- 无 `wxWidgets`
- 无 `OpenGL` / `GLEW` / `GLFW`
- 无 `libcurl`（不需要网络）
- 无 `OpenCV`

### 6.3 依赖项构建（无头模式）

依赖项构建必须使用 `-DFLATPAK=ON` 来跳过 wxWidgets，引擎不需要它：

```bash
cmake -S deps -B deps/build -G Ninja \
  -DFLATPAK=ON \
  -DDESTDIR="$(pwd)/deps/build/destdir"
```

构建的所需依赖库：
- Boost 1.83.0（system、filesystem、thread、log、log_setup、locale、regex、chrono、atomic、date_time、iostreams、program_options、nowide）
- Intel TBB
- Eigen3（仅头文件）
- Clipper2
- OpenVDB + Blosc
- NLopt
- CGAL（GMP + MPFR）
- OpenSSL
- Cereal
- nlohmann_json
- libpng、freetype、ZLIB
- OpenCASCADE（STEP 格式支持）

### 6.4 CI/CD：GitHub Actions

`.github/workflows/build-cloud-engine.yml` 中的工作流自动化 Linux x64 构建。

**触发条件：**

| 事件 | 分支/条件 |
|------|----------|
| `push` | `main`、`release/*`、`engine-build` — 仅当引擎相关路径更改时 |
| `pull_request` | 目标 `main` — 仅当引擎相关路径更改时 |
| `workflow_dispatch` | 手动触发，任何分支 |

**触发工作流的监控路径：**
- `src/orca-slice-engine/**`
- `src/libslic3r/**`
- `src/libnest2d/**`
- `deps/**`
- `CMakeLists.txt`
- `cmake/**`
- `.github/workflows/build-cloud-engine.yml`

**构建环境：** Ubuntu 22.04（选择 glibc 2.35 兼容性，最大化部署兼容性）。

**依赖项缓存：** 工作流使用所有依赖项 CMakeLists.txt 文件的哈希缓存 `deps/build/destdir` 和 `deps/DL_CACHE`。缓存键：`orca-deps-headless-Linux-<hash>`。

**构件结构：**

```
orca-slice-engine-linux-x64-V<version>.tar.gz
└── orca-slice-engine/
    ├── orca-slice-engine          # ELF 二进制文件（静态链接核心）
    ├── run.sh                     # 设置 LD_LIBRARY_PATH 的包装脚本
    ├── lib/
    │   ├── libstdc++.so.6         # 捆绑以提高兼容性
    │   ├── libjpeg.so.8           # libjpeg-turbo（版本特定）
    │   └── ...                    # 其他非系统共享库
    └── resources/
        ├── profiles/              # 打印机/材料预设（所有供应商）
        └── info/                  # 喷嘴尺寸信息
```

**构件保留：** 30 天。

**下载构件：**
1. 导航到 Actions → Build Cloud Slice Engine
2. 选择已完成的运行
3. 从 Artifacts 部分下载 `orca-slice-engine-linux-x64-*`

---

## 7. 引擎与 GUI 对比

| 方面 | GUI 应用程序 | 切片引擎 |
|------|-------------|----------|
| **入口点** | `src/Snapmaker_Orca.cpp` | `src/orca-slice-engine/main.cpp` |
| **接口** | 交互式 wxWidgets 窗口 | 命令行参数 |
| **输入** | 打开文件对话框、拖放 | `-` 参数：3MF 路径 |
| **配置** | 交互式预设选择 | 嵌入在 3MF 文件中 |
| **显示** | OpenGL 3D 视口 | 无 |
| **缩略图** | 通过 OpenGL 渲染生成 | 空（未生成） |
| **盘原点** | `PartPlateList::compute_shape_position()` | `main.cpp:501-515` 中的相同公式 |
| **盘大小** | `bed.extended_bounding_box()` | `bbox(printable_area) - BED_AXES_TIP_RADIUS` |
| **布局的盘数** | 项目中的总盘数 | `plate_data.size()`（总数） |
| **切片调用** | `Print::process()` | `Print::process()`（相同） |
| **G 代码导出** | `Print::export_gcode()` | `Print::export_gcode()`（相同） |
| **3MF 打包** | `store_bbs_3mf()` | `store_bbs_3mf()`（相同） |
| **SaveStrategy** | `Silence\|SplitModel\|WithGcode\|SkipModel\|Zip64` | 相同标志 |
| **时间戳** | 系统时钟 | 系统时钟或 `-t` 覆盖 |
| **依赖项** | wxWidgets、OpenGL、GLEW、GLFW、OpenCV、libcurl | 仅 libslic3r、Boost、TBB、Cereal |
| **二进制大小** | ~100+ MB | ~20-30 MB |
| **内存占用** | 高（渲染缓冲区、UI 状态） | 低（仅模型 + 切片数据） |
| **并行性** | TBB（切片）+ GPU（渲染） | TBB（仅切片） |
| **平台** | Windows、macOS、Linux | Linux x64、macOS、Windows |
| **退出机制** | `wxApp::OnExit()` | `std::quick_exit()` |

---

## 8. API 集成指南

### 8.1 REST API 示例（Python/Flask）

```python
from flask import Flask, request, send_file, jsonify
import subprocess
import tempfile
import os
import uuid

app = Flask(__name__)

ENGINE_PATH = "/opt/orca/orca-slice-engine"
RESOURCES_PATH = "/opt/orca/resources"


@app.route("/slice", methods=["POST"])
def slice_all_plates():
    """切片上传的 3MF 文件中的所有盘。"""
    if "model" not in request.files:
        return jsonify({"error": "未提供模型文件"}), 400

    job_id = str(uuid.uuid4())
    input_path = f"/tmp/{job_id}.3mf"
    output_base = f"/tmp/{job_id}_output"

    try:
        request.files["model"].save(input_path)

        result = subprocess.run(
            [
                ENGINE_PATH,
                input_path,
                "-o", output_base,
                "-r", RESOURCES_PATH,
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )

        if result.returncode != 0:
            return jsonify({
                "error": "切片失败",
                "stderr": result.stderr,
                "exit_code": result.returncode,
            }), 500

        output_file = f"{output_base}.gcode.3mf"
        return send_file(output_file, mimetype="application/octet-stream",
                         as_attachment=True,
                         download_name=f"{job_id}.gcode.3mf")

    finally:
        # 清理输入文件
        if os.path.exists(input_path):
            os.unlink(input_path)


@app.route("/slice/plate/<int:plate_id>", methods=["POST"])
def slice_single_plate(plate_id: int):
    """切片特定盘并返回 G 代码。"""
    fmt = request.args.get("format", "gcode.3mf")
    if fmt not in ("gcode", "gcode.3mf"):
        return jsonify({"error": "无效格式。使用 'gcode' 或 'gcode.3mf'"}), 400

    job_id = str(uuid.uuid4())
    input_path = f"/tmp/{job_id}.3mf"
    output_base = f"/tmp/{job_id}"

    try:
        request.files["model"].save(input_path)

        result = subprocess.run(
            [
                ENGINE_PATH,
                input_path,
                "-p", str(plate_id),
                "-f", fmt,
                "-o", output_base,
                "-r", RESOURCES_PATH,
            ],
            capture_output=True,
            text=True,
            timeout=300,
        )

        if result.returncode == 1:
            return jsonify({"error": "无效的盘 ID 或参数",
                            "stderr": result.stderr}), 400
        if result.returncode == 2:
            return jsonify({"error": "未找到输入文件"}), 404
        if result.returncode != 0:
            return jsonify({"error": "切片失败",
                            "stderr": result.stderr,
                            "exit_code": result.returncode}), 500

        ext = ".gcode" if fmt == "gcode" else ".gcode.3mf"
        output_file = f"{output_base}{ext}"
        mimetype = "text/plain" if fmt == "gcode" else "application/octet-stream"

        return send_file(output_file, mimetype=mimetype,
                         as_attachment=True,
                         download_name=f"plate_{plate_id}{ext}")
    finally:
        if os.path.exists(input_path):
            os.unlink(input_path)


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
```

### 8.2 异步作业队列示例（Python/Celery）

```python
from celery import Celery
import subprocess
import os

celery_app = Celery("slicer", broker="redis://localhost:6379/0")
ENGINE_PATH = "/opt/orca/orca-slice-engine"


@celery_app.task(bind=True)
def slice_model(self, input_path: str, output_base: str,
                plate_id: int = 0, fmt: str = "gcode.3mf"):
    """异步切片任务。"""
    cmd = [ENGINE_PATH, input_path, "-o", output_base]
    if plate_id > 0:
        cmd.extend(["-p", str(plate_id)])
    cmd.extend(["-f", fmt])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            raise Exception(f"切片失败（退出 {result.returncode}）：{result.stderr}")

        return {"status": "success", "output": output_base}
    except subprocess.TimeoutExpired:
        self.retry(countdown=0, max_retries=0)
        raise Exception("切片在 10 分钟后超时")
```

### 8.3 错误处理最佳实践

始终将退出代码映射到有意义的响应：

```python
EXIT_CODE_MESSAGES = {
    0: "成功",
    1: "无效参数（检查盘 ID 和格式）",
    2: "未找到输入文件",
    3: "加载 3MF 文件失败（损坏或不兼容的版本）",
    4: "切片失败（模型可能存在几何问题）",
    5: "写入输出文件失败（检查磁盘空间和权限）",
}

def run_engine(cmd: list) -> dict:
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return {
        "success": result.returncode == 0,
        "exit_code": result.returncode,
        "message": EXIT_CODE_MESSAGES.get(result.returncode, "未知错误"),
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
```

---

## 9. 故障排查

### 9.1 常见错误消息

**"No objects found in 3MF file"（3MF 文件中未找到对象）**

输入 3MF 不包含 3D 网格对象，或文件已损坏。

解决方案：在 OrcaSlicer GUI 中打开文件以验证它包含可打印模型。必要时重新导出。

---

**"Plate X not found in 3MF file"（3MF 文件中未找到盘 X）**

请求的盘 ID 在项目中不存在。引擎会打印可用盘 ID 的列表。

解决方案：不带 `-p` 运行以查看可用的盘，然后使用有效 ID 重新运行。

---

**"All-plate slicing requires gcode.3mf format"（全盘切片需要 gcode.3mf 格式）**

在没有 `-p` 的情况下指定了 `-f gcode`，这是不允许的。

解决方案：添加 `-p <id>` 以切片特定盘，或删除 `-f gcode` 以使用默认格式。

---

**"Failed to load 3MF file"（加载 3MF 文件失败）**

3MF 文件使用此引擎版本不支持的格式版本或功能，或文件已损坏。

解决方案：检查 3MF 是否从 OrcaSlicer 导出（而不是从 Bambu Studio 或 PrusaSlicer 导出而未转换）。验证文件完整性。

---

**"libjpeg.so.8: cannot open shared object file"（无法打开共享对象文件）**

部署中缺少捆绑的共享库。

解决方案：使用发布构件中的 `run.sh` 包装脚本，而不是直接调用 `orca-slice-engine`。包装器设置 `LD_LIBRARY_PATH` 以包含捆绑的 `lib/` 目录。

---

**"Resources directory may be incomplete"（资源目录可能不完整）**

资源目录存在但缺少 `profiles/` 或 `printers/` 子目录。

解决方案：确保发布构件中的完整 `resources/` 目录存在。引擎需要 `resources/profiles/` 来加载打印机/材料配置。

### 9.2 日志级别

| 级别 | 标志 | 内容 |
|------|------|------|
| `info` | 默认 | 关键里程碑、盘原点、输出路径 |
| `warning` | 默认 | 非致命问题（缺少资源、空盘） |
| `error` | 默认 | 带详细信息的致命错误 |
| `debug` | `-v` | 临时文件操作、配置值 |
| `trace` | `-v` | 来自 libslic3r 的完整切片跟踪 |

启用详细日志并重定向到日志文件：

```bash
./orca-slice-engine model.3mf -v 2>&1 | tee /tmp/slice.log
```

### 9.3 性能说明

| 资源 | 指导 |
|------|------|
| RAM | 典型模型 2-4 GB；复杂多材料模型 6+ GB |
| CPU | 通过 TBB 使用所有可用核心；无法限制，除非使用 `taskset`/`numactl` |
| 磁盘 | 临时文件写入系统临时目录；多盘作业创建 N 个临时 `.gcode` 文件 |
| 并发 | 多个引擎实例可以同时运行；进程之间无共享状态 |

限制并发作业的 CPU 使用：

```bash
# 限制为 4 个核心
taskset -c 0-3 ./orca-slice-engine model.3mf

# 通过 TBB 环境变量仅使用 4 个核心
TBB_THREAD_NUM=4 ./orca-slice-engine model.3mf
```

---

## 10. 开发指南

### 10.1 源代码结构

```
src/orca-slice-engine/
├── main.cpp          # 所有引擎逻辑（约 810 行）
└── nanosvg.cpp       # nanosvg 实现单元（libslic3r 需要）
```

引擎有意设计为单文件实现。所有逻辑都在 `main.cpp` 中。这最大限度地减少了复杂性，使引擎易于理解、审计和修改。

**main.cpp 中的关键部分：**

| 行 | 内容 |
|----|------|
| 1-43 | 包含和 `using namespace Slic3r` |
| 44-64 | 退出代码、`OutputFormat` 枚举、`compute_colum_count()` |
| 66-131 | `print_usage()` 和 `generate_output_path()` |
| 133-209 | `main()`：参数解析循环 |
| 210-234 | 参数验证和格式自动选择 |
| 236-289 | 日志设置和资源目录解析 |
| 291-373 | 3MF 加载和盘数据验证 |
| 375-519 | 盘准备循环：可打印标志、原点计算 |
| 520-599 | 每个盘的切片和 G 代码导出 |
| 601-791 | 带元数据的 3MF 打包 |
| 793-810 | 摘要输出和清理 |

### 10.2 相关源文件

要理解或修改引擎，`libslic3r` 中的这些文件是主要参考：

| 文件 | 相关性 |
|------|--------|
| `src/libslic3r/Print.cpp` | `Print::process()` 和 `Print::export_gcode()` |
| `src/libslic3r/PrintApply.cpp` | 如何将盘原点偏移应用于模型 |
| `src/libslic3r/GCode.cpp` | 使用 `set_gcode_offset()` 生成 G 代码 |
| `src/libslic3r/Format/bbs_3mf.cpp` | 3MF 加载/保存；第 1485 行的 `plate_index` 基于 0 的转换 |
| `src/slic3r/GUI/PartPlate.cpp` | GUI 盘布局参考（第 3260-3309 行） |
| `src/slic3r/GUI/3DBed.cpp` | `extended_bounding_box()` 和 `DefaultTipRadius` |

### 10.3 添加新的 CLI 选项

1. 在声明部分添加新的选项常量或枚举（约第 44-64 行）
2. 在参数解析循环中添加一个 case（第 148-209 行）
3. 如果需要，在循环后添加验证（第 211-234 行）
4. 将值传递给适当的 `Print::` 方法或打包器
5. 更新 `print_usage()`（第 66-91 行）以记录新选项
6. 更新此技术指南和 README

### 10.4 测试切片输出

要验证引擎输出是否与 GUI 输出匹配：

```python
#!/usr/bin/env python3
"""比较引擎和 GUI 输出的 G 代码坐标。"""
import zipfile, re, sys

def first_xy(gcode_3mf, plate=1):
    with zipfile.ZipFile(gcode_3mf) as z:
        gcode = z.read(f"Metadata/plate_{plate}.gcode").decode()
    for line in gcode.splitlines():
        m = re.search(r"G1 .*X([-\d.]+).*Y([-\d.]+)", line)
        if m:
            return float(m.group(1)), float(m.group(2))
    return None, None

engine_x, engine_y = first_xy(sys.argv[1])
gui_x,    gui_y    = first_xy(sys.argv[2])

diff = ((engine_x - gui_x)**2 + (engine_y - gui_y)**2) ** 0.5
print(f"Engine: ({engine_x:.3f}, {engine_y:.3f})")
print(f"GUI:    ({gui_x:.3f}, {gui_y:.3f})")
print(f"Delta:  {diff:.3f} mm  {'PASS' if diff < 0.1 else 'FAIL'}")
```

用法：
```bash
python compare.py engine_output.gcode.3mf gui_output.gcode.3mf
```

### 10.5 添加对新 3MF 功能的支持

引擎使用与 GUI 相同的 `Model::read_from_file()`。如果 GUI 添加了对新 3MF 功能的支持（例如，新的每盘元数据字段），引擎会通过 `libslic3r` 自动继承它——除非该功能需要新的 CLI 选项或更改打包步骤，否则不需要更改 `main.cpp`。

对于应出现在输出 `gcode.3mf` 中的新每盘元数据，将填充代码添加到 `PlateData` 准备循环（`main.cpp` 中的第 643-713 行）。

---

## 附录 A：数学公式

```
# 盘尺寸
plate_width = bbox_width - BED_AXES_TIP_RADIUS    (BED_AXES_TIP_RADIUS = 1.25 mm)
plate_depth = bbox_depth - BED_AXES_TIP_RADIUS

# 盘步幅
plate_stride_x = plate_width * (1 + LOGICAL_PART_PLATE_GAP)   (GAP = 0.2)
plate_stride_y = plate_depth * (1 + LOGICAL_PART_PLATE_GAP)

# 网格布局
plate_cols = compute_colum_count(total_plate_count)
row = plate_index / plate_cols
col = plate_index % plate_cols

# 原点
origin_x = col * plate_stride_x
origin_y = -row * plate_stride_y

# 列数函数
compute_colum_count(n):
    v = sqrt(n)
    r = round(v)
    return r + 1 if v > r else r
```

---

## 附录 B：gcode.3mf 输出的文件结构

引擎生成的 `gcode.3mf` 输出具有以下内部结构：

```
output.gcode.3mf  (ZIP 归档)
├── _rels/
│   └── .rels
├── 3D/
│   └── 3dmodel.model          # 模型几何体（使用 SplitModel 标志时存在）
├── Metadata/
│   ├── model_settings.config  # 切片配置
│   ├── plate_1.gcode          # 盘 1 的 G 代码
│   ├── plate_2.gcode          # 盘 2 的 G 代码（如果是多盘）
│   ├── slice_info.config      # 每盘切片元数据（时间、重量等）
│   └── project_settings.config
└── [Content_Types].xml
```

---

**文档版本：** 1.0
**作者：** 技术撰稿人
**状态：** 完成
