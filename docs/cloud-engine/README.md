# OrcaSlicer 云端切片引擎

一个用于云端部署的最小化无头切片器。将包含切片配置的3MF文件转换为G-code，无需GUI环境。

## 概述

云端切片引擎（`orca-slice-engine`）是一个命令行工具，专为服务端切片操作设计。它读取包含3D模型和切片预设的3MF文件，通过OrcaSlicer的切片管道处理，输出可用于3D打印的G-code。

### 核心特性

- **无头运行**：无需显示器或GUI依赖
- **内嵌配置**：直接从3MF文件读取切片设置
- **多盘支持**：支持指定盘切片或全盘批量切片
- **多种输出格式**：支持纯G-code或G-code.3MF容器格式
- **进度报告**：输出切片进度，便于云端集成
- **跨平台**：支持 Linux x64、macOS 和 Windows

## 安装部署

### 从源码构建

前置条件：
- CMake 3.13+
- C++17 编译器
- 已构建的依赖库（见主项目构建说明）

```bash
# 克隆并先构建依赖
./build_linux.sh -d

# 只构建切片引擎（无GUI）
cmake -B build -DSLIC3R_GUI=OFF
cmake --build build --target orca-slice-engine

# 可执行文件位置：
# build/src/orca-slice-engine/orca-slice-engine
```

### 部署包结构

云端部署需要的文件结构：

```
orca-slice-engine/
├── orca-slice-engine          # 可执行文件
├── lib/                       # 共享库（可选，用于移植性）
└── resources/
    ├── profiles/              # 打印机/材料预设
    │   ├── Snapmaker/
    │   ├── BBL/
    │   └── ...
    └── info/                  # 喷嘴信息等
```

## 使用方法

### 基本语法

```bash
./orca-slice-engine input.3mf [选项]
```

### 命令行选项

| 选项 | 说明 |
|------|------|
| `-o, --output <文件>` | 输出文件路径（不含扩展名）。单盘输出 `{file}.gcode` 或 `{file}.gcode.3mf`；全盘输出 `{file}.gcode.3mf` |
| `-p, --plate <id>` | 指定切片的盘号。使用数字 (1, 2, 3...) 指定单盘，`all` 或省略表示全盘（默认：全盘） |
| `-f, --format <格式>` | 输出格式：`gcode`（纯文本）或 `gcode.3mf`（3MF容器）。默认：gcode.3mf。全盘强制 gcode.3mf |
| `-r, --resources <目录>` | 资源目录，包含打印机预设文件 |
| `-v, --verbose` | 启用详细日志（trace级别） |
| `-h, --help` | 显示帮助信息 |

### 使用示例

#### 单盘切片

```bash
# 切片第1盘 -> 输出 model-p1.gcode.3mf（默认格式）
./orca-slice-engine model.3mf -p 1

# 切片第2盘 -> 输出 model-p2.gcode.3mf
./orca-slice-engine model.3mf -p 2

# 切片第1盘，输出纯文本 gcode 格式
./orca-slice-engine model.3mf -p 1 -f gcode
# -> model-p1.gcode

# 切片第1盘，指定输出路径
./orca-slice-engine model.3mf -p 1 -o /output/result
# -> /output/result.gcode.3mf
```

#### 全盘切片

```bash
# 全盘切片 -> 输出 model.gcode.3mf（包含所有盘的G-code）
./orca-slice-engine model.3mf

# 显式指定全盘
./orca-slice-engine model.3mf -p all

# 全盘切片，指定输出路径
./orca-slice-engine model.3mf -o /output/result
# -> /output/result.gcode.3mf
```

#### 其他选项

```bash
# 指定资源目录
./orca-slice-engine model.3mf -r /opt/orca-slicer/resources

# 启用详细日志用于调试
./orca-slice-engine model.3mf -v

# 使用环境变量指定资源目录
export ORCA_RESOURCES=/opt/orca-slicer/resources
./orca-slice-engine model.3mf
```

### 输出文件命名规则

| 命令 | 输出文件 |
|------|---------|
| `-p 1` | `{input}-p1.gcode.3mf` |
| `-p 1 -f gcode` | `{input}-p1.gcode` |
| `-p 1 -o output` | `output.gcode.3mf` |
| `-p 1 -o output -f gcode` | `output.gcode` |
| 无 `-p` 或 `-p all` | `{input}.gcode.3mf` |
| `-o result`（全盘） | `result.gcode.3mf` |

### 输出格式说明

| 格式 | 说明 | 使用场景 |
|------|------|---------|
| `gcode` | 纯文本G-code文件 | 单盘切片，直接发送到打印机 |
| `gcode.3mf` | 3MF容器格式，内嵌G-code | 多盘切片，需要保留元数据，或单盘需要3MF格式时 |

**注意**：全盘切片时强制使用 `gcode.3mf` 格式，因为需要将多个G-code文件打包到一个容器中。

### 退出码

| 代码 | 常量 | 说明 |
|------|------|------|
| 0 | `EXIT_OK` | 成功 |
| 1 | `EXIT_INVALID_ARGS` | 命令行参数无效（如无效盘号、全盘时指定gcode格式等） |
| 2 | `EXIT_FILE_NOT_FOUND` | 输入文件不存在 |
| 3 | `EXIT_LOAD_ERROR` | 加载3MF文件失败 |
| 4 | `EXIT_SLICING_ERROR` | 切片过程失败（任一盘失败即中断） |
| 5 | `EXIT_EXPORT_ERROR` | 导出G-code失败 |

## 配置说明

### 资源目录

引擎需要打印机/材料预设文件。通过以下方式指定位置：

1. **命令行参数**：`-r /path/to/resources`
2. **环境变量**：`ORCA_RESOURCES=/path/to/resources`
3. **相对路径**：将 `resources/` 文件夹放在可执行文件旁边

优先级：命令行 > 环境变量 > 相对路径

### 3MF 文件要求

输入的3MF文件必须包含：

- **3D模型**：至少一个网格对象
- **内嵌配置**：切片设置（层高、填充、支撑等）
- **打印机配置**：目标打印机定义
- **盘数据**（多盘时）：各盘的对象分配信息

从OrcaSlicer GUI导出的3MF文件会自动包含所有必要配置。

### 进度输出

引擎向标准输出生成进度信息：

```
[Progress] 25% - 正在处理层...
[Status] 正在生成支撑材料
[Progress] 50% - 正在生成G-code
```

格式说明：
- `[Progress] <百分比>% - <描述>`：可量化的进度
- `[Status] <描述>`：无百分比的状态更新

多盘切片时会显示当前处理的盘号：

```
=== Processing plate 1 ===
[Progress] 25% - ...
=== Processing plate 2 ===
[Progress] 25% - ...
```

## 集成示例

### REST API 示例（Python Flask）

```python
from flask import Flask, request, send_file
import subprocess
import tempfile
import os

app = Flask(__name__)

@app.route('/slice', methods=['POST'])
def slice_model():
    # 保存上传的3MF文件
    input_file = tempfile.NamedTemporaryFile(suffix='.3mf', delete=False)
    input_file.write(request.files['model'].read())
    input_file.close()

    # 生成输出路径
    output_file = input_file.name.replace('.3mf', '.gcode')

    # 运行切片引擎
    result = subprocess.run([
        '/opt/orca-slicer/orca-slice-engine',
        input_file.name,
        '-o', output_file,
        '-r', '/opt/orca-slicer/resources'
    ], capture_output=True, text=True)

    if result.returncode != 0:
        return {'error': result.stderr}, 400

    # 返回G-code
    return send_file(output_file, mimetype='text/plain')

@app.route('/slice/plate/<int:plate_id>', methods=['POST'])
def slice_plate(plate_id):
    """切片指定盘"""
    input_file = tempfile.NamedTemporaryFile(suffix='.3mf', delete=False)
    input_file.write(request.files['model'].read())
    input_file.close()

    output_base = input_file.name.replace('.3mf', '')

    result = subprocess.run([
        '/opt/orca-slicer/orca-slice-engine',
        input_file.name,
        '-p', str(plate_id),
        '-o', output_base,
        '-r', '/opt/orca-slicer/resources'
    ], capture_output=True, text=True)

    if result.returncode != 0:
        return {'error': result.stderr}, 400

    # 返回G-code
    output_file = f"{output_base}.gcode"
    return send_file(output_file, mimetype='text/plain')

if __name__ == '__main__':
    app.run()
```

### Docker 部署

```dockerfile
FROM ubuntu:20.04

RUN apt-get update && apt-get install -y \
    libgl1-mesa-glx \
    libglib2.0-0 \
    libjpeg8 \
    libfreetype6 \
    libbz2-1.0 \
    && rm -rf /var/lib/apt/lists/*

COPY orca-slice-engine /usr/local/bin/
COPY lib/*.so* /usr/local/lib/
COPY resources /opt/orca-slicer/resources

ENV ORCA_RESOURCES=/opt/orca-slicer/resources
ENV LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

ENTRYPOINT ["orca-slice-engine"]
```

构建和运行：

```bash
# 构建镜像
docker build -t orca-slice-engine .

# 运行全盘切片
docker run --rm -v $(pwd):/data orca-slice-engine /data/model.3mf -o /data/output

# 运行单盘切片
docker run --rm -v $(pwd):/data orca-slice-engine /data/model.3mf -p 1 -o /data/output
```

## 故障排除

### 常见问题

**"No objects found in 3MF file"（3MF文件中没有对象）**

3MF文件为空或已损坏。用OrcaSlicer GUI打开验证内容。

**"Plate X not found in 3MF file"（指定的盘不存在）**

指定的盘号超出范围。不使用 `-p` 参数运行命令，引擎会显示可用的盘号。

**"All-plate slicing requires gcode.3mf format"（全盘切片需要gcode.3mf格式）**

全盘切片时不能指定 `-f gcode`。省略 `-f` 参数或使用 `-f gcode.3mf`。

**"Resources directory not found"（找不到资源目录）**

引擎无法定位打印机预设。确保 `resources/profiles/` 目录存在且可访问。

**"Failed to load 3MF file"（加载3MF文件失败）**

文件格式不支持或配置不兼容。检查3MF是否从兼容的OrcaSlicer版本导出。

**"libjpeg.so.8: cannot open shared object file"**

系统缺少必要的共享库。使用打包版本（包含 lib/ 目录）或安装对应库。

### 详细日志

使用 `-v` 参数启用详细日志：

```bash
./orca-slice-engine model.3mf -v 2>&1 | tee slice.log
```

这将输出trace级别信息，包括：
- 配置加载过程
- 模型处理步骤
- 切片参数
- 导出详情
- 多盘处理进度

### 性能考虑

- **内存**：复杂模型可能需要 2-4 GB RAM，多盘切片会累加
- **CPU**：多线程切片使用所有可用核心（通过TBB）
- **磁盘**：临时文件写入系统临时目录，全盘切片会产生多个临时G-code文件

## 技术细节

### 构建配置

切片引擎使用 `SLIC3R_GUI=OFF` 构建，排除wxWidgets和GUI依赖：

```cmake
cmake -B build -DSLIC3R_GUI=OFF
cmake --build build --target orca-slice-engine
```

### 依赖库

引擎链接以下库：
- `libslic3r`：核心切片库
- `Boost`：文件系统、日志、程序选项
- `TBB`：并行处理
- `cereal`：序列化

无需GUI库（wxWidgets、OpenGL）。

### 多盘切片实现

多盘切片的工作流程：

1. 加载3MF文件，解析所有盘数据
2. 遍历每个盘：
   - 创建Print对象
   - 应用模型和配置
   - 执行切片（process）
   - 导出G-code到临时目录
3. 将所有G-code打包到3MF容器
4. 输出最终的 gcode.3mf 文件

错误处理：任一盘切片失败，整个操作立即中断并返回错误码。

## GitHub Actions 构建

项目包含GitHub Actions workflow，可自动构建Linux可执行文件。

### 触发方式

| 方式 | 说明 |
|------|------|
| 手动触发 | Actions → Build Cloud Slice Engine → Run workflow |
| Push | main、release/*、engine-build 分支修改相关文件时 |
| PR | 到 main 分支的 Pull Request |

### 下载产物

1. 进入 Actions 页面
2. 选择完成的 workflow run
3. 在 Artifacts 区域下载 `orca-slice-engine-linux-x64-*`

### 产物内容

```
orca-slice-engine/
├── orca-slice-engine      # 可执行文件
├── run.sh                 # 启动脚本（设置LD_LIBRARY_PATH）
├── lib/                   # 共享库
└── resources/
    ├── profiles/          # 打印机预设
    └── info/              # 喷嘴信息
```
