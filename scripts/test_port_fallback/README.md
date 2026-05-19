# Port 13619 Fallback Test

验证 `downloadFileFromOrca` 在 HTTP 服务器端口漂移时不再超时的 fix。

## 背景

OrcaSlicer 默认 HTTP 服务端口为 13619。当该端口被占用（如前次崩溃残留），服务器会自动切换到 13620+。此前 Flutter 侧 `downloadFileFromOrca` 硬编码了 13619，导致请求超时 `Failed to fetch`。

## 测试原理

1. 故意占住 13619 端口
2. 启动 OrcaSlicer，验证其自动切换到其他端口
3. 确认 Flutter UI 正常加载，文件下载不再超时

## 各平台用法

### macOS

```bash
./scripts/test_port_fallback/test_port_fallback_mac.sh [path/to/Snapmaker Orca.app]
```

不传参数默认找 `build/arm64/Snapmaker_Orca/Snapmaker Orca.app`。

依赖：`nc`（系统自带）、`lsof`（系统自带）。

### Linux

```bash
./scripts/test_port_fallback/test_port_fallback_linux.sh [path/to/orca-slicer]
```

不传参数默认找 `build/OrcaSlicer_apprel`。

依赖：`nc`（`sudo apt install netcat-openbsd`）、`lsof` 或 `ss`（系统自带）。

### Windows

```cmd
REM 推荐：双击 run_port_fallback_test.bat 直接运行
REM 或在终端中：
scripts\test_port_fallback\run_port_fallback_test.bat [path\to\orca-slicer.exe]

REM 或在 PowerShell 中运行：
.\scripts\test_port_fallback\test_port_fallback_win.ps1 [path\to\orca-slicer.exe]
```

不传参数自动搜索 `build\Snapmaker_Orca\snapmaker-orca.exe`、`build\OrcaSlicer_apprel.exe` 或 `build\src\Release\orca-slicer.exe`。

无外部依赖，使用 PowerShell 原生 `TcpListener`。

## 验证项

- [ ] 日志出现 `Original port 13619 is in use, switching to port XXXX`
- [ ] Flutter UI 首页/准备页正常显示
- [ ] 加载模型并切片后，文件下载操作不再报 `Failed to fetch`

## 清理

测试完成后按 Enter，脚本自动释放 13619 端口。
