# OrcaSlicer 云端切片引擎

一个用于云端部署的最小化无头切片器。将包含切片配置的3MF文件转换为G-code，无需GUI环境。

## 概述

云端切片引擎（`orca-slice-engine`）是一个命令行工具，专为服务端切片操作设计。它读取包含3D模型和切片预设的3MF文件，通过OrcaSlicer的切片管道处理，输出可用于3D打印的G-code。

### 核心特性

- **无头运行**：无需显示器或GUI依赖
- **内嵌配置**：直接从3MF文件读取切片设置
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
└── resources/
    └── profiles/              # 打印机/材料预设
        ├── Snapmaker/
        ├── BBL/
        └── ...
```

## 使用方法

### 基本语法

```bash
./orca-slice-engine input.3mf [选项]
```

### 命令行选项

| 选项 | 说明 |
|------|------|
| `-o, --output <文件>` | 输出G-code文件路径（默认：输入文件同目录，扩展名改为`.gcode`） |
| `-r, --resources <目录>` | 资源目录，包含打印机预设文件 |
| `-v, --verbose` | 启用详细日志（trace级别） |
| `-h, --help` | 显示帮助信息 |

### 使用示例

```bash
# 基本切片 - 输出到 model.gcode
./orca-slice-engine model.3mf

# 指定输出位置
./orca-slice-engine model.3mf -o /output/path.gcode

# 指定资源目录
./orca-slice-engine model.3mf -r /opt/orca-slicer/resources

# 启用详细日志用于调试
./orca-slice-engine model.3mf -v

# 使用环境变量指定资源目录
export ORCA_RESOURCES=/opt/orca-slicer/resources
./orca-slice-engine model.3mf
```

### 退出码

| 代码 | 常量 | 说明 |
|------|------|------|
| 0 | `EXIT_OK` | 成功 |
| 1 | `EXIT_INVALID_ARGS` | 命令行参数无效 |
| 2 | `EXIT_FILE_NOT_FOUND` | 输入文件不存在 |
| 3 | `EXIT_LOAD_ERROR` | 加载3MF文件失败 |
| 4 | `EXIT_SLICING_ERROR` | 切片过程失败 |
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

if __name__ == '__main__':
    app.run()
```

### Docker 部署

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libgl1-mesa-glx \
    libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

COPY orca-slice-engine /usr/local/bin/
COPY resources /opt/orca-slicer/resources

ENV ORCA_RESOURCES=/opt/orca-slicer/resources

ENTRYPOINT ["orca-slice-engine"]
```

构建和运行：

```bash
# 构建镜像
docker build -t orca-slice-engine .

# 运行切片
docker run --rm -v $(pwd):/data orca-slice-engine /data/model.3mf -o /data/output.gcode
```

## 故障排除

### 常见问题

**"No objects found in 3MF file"（3MF文件中没有对象）**

3MF文件为空或已损坏。用OrcaSlicer GUI打开验证内容。

**"Resources directory not found"（找不到资源目录）**

引擎无法定位打印机预设。确保 `resources/profiles/` 目录存在且可访问。

**"Failed to load 3MF file"（加载3MF文件失败）**

文件格式不支持或配置不兼容。检查3MF是否从兼容的OrcaSlicer版本导出。

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

### 性能考虑

- **内存**：复杂模型可能需要 2-4 GB RAM
- **CPU**：多线程切片使用所有可用核心（通过TBB）
- **磁盘**：临时文件写入系统临时目录

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

## GitHub Actions 构建

项目包含GitHub Actions workflow，可自动构建Linux可执行文件。

### 触发方式

| 方式 | 说明 |
|------|------|
| 手动触发 | Actions → Build Cloud Slice Engine → Run workflow |
| Push | main、release/*、2.2.3 分支修改相关文件时 |
| PR | 到 main 分支的 Pull Request |

### 下载产物

1. 进入 Actions 页面
2. 选择完成的 workflow run
3. 在 Artifacts 区域下载 `orca-slice-engine-linux-x64-*`

### 产物内容

```
orca-slice-engine/
├── orca-slice-engine      # 可执行文件
└── resources/
    └── profiles/          # 打印机预设
```
