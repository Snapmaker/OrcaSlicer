# OpenSSL 3.5.7 LTS + paho-mqtt 升级：变更说明与测试指南

**审查基线提交**: `761718a5eaf6f52b818af236653f3807d091a613` Fix：top-cover machine end gcode (#584)
**日期**: 2026-08-19
**基线影响范围**: 123 files changed (+15970 / -9767)

---

## 1. 升级概览

本提交基于 `761718a5ea` 完成 OpenSSL 1.1.1w → 3.5.7 LTS 以及 paho-mqtt 升级，并包含相关 P1/P2/P3 修复。

| 组件 | 基线提交前 | 本提交后 | 变更类型 |
|------|------------|------------|----------|
| OpenSSL | 1.1.1w | **3.5.7 LTS** | 构建配置 |
| paho-mqtt-c | 1.3.13 | **1.3.14** | 官方源码/CMake 子集导入 |
| paho-mqtt-cpp | 1.4.0 | **1.5.1** | 构建所需文件更新 |
| MD5 API | 低层 (MD5_Init/Update/Final) | **EVP + 错误处理** | API 迁移与加固 |
| SSLSocket.c | 旧版锁回调始终执行 | **按 OpenSSL 版本守卫** | 本地兼容性补丁 |

OpenSSL 3.5 属于 LTS 分支，官方支持截止时间为 2030-04-08。

---

## 2. 变更明细

### 2.1 MD5 API 迁移（EVP 化）

OpenSSL 3.x 将低层哈希 API（MD5_CTX、MD5_Init、MD5_Update、MD5_Final）标记为 deprecated，编译时产生弃用错误。本次将以下文件迁移到 EVP 高层 API：

| 文件 | 改动 |
|------|------|
| `src/libslic3r/Utils.hpp` | `#include <openssl/md5.h>` 改为 `#include <openssl/evp.h>` |
| `src/libslic3r/utils.cpp` | `bbl_calc_md5()` 重写为 EVP API；新增文件存在性检查避免 boost 异常 |
| `src/libslic3r/Format/bbs_3mf.cpp` | 复用 `bbl_calc_md5()`，计算失败时中止 3MF 导出 |

`CreatePresetsDialog.cpp` 无需修改（已使用 EVP API）。

### 2.2 OpenSSL 3.5.7 LTS 构建配置

| 文件 | 改动 |
|------|------|
| `deps/OpenSSL/OpenSSL.cmake` | URL 改为 openssl-3.5.7.tar.gz，SHA256 更新 |
| deps/OpenSSL/openssl/ | 移除旧手写 CMake 包元数据，使用 3.5.7 官方安装产物 |
| `deps/CMakeLists.txt` | `find_package(OpenSSL 1.1...<3.2)` 改为 `3.0...<4.0` |
| `scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.yml` | OpenSSL source URL 同步更新 |

### 2.3 paho-mqtt-c 1.3.14

- `src/mqtt/externals/paho-mqtt-c/` 按项目构建需要导入官方 v1.3.14 release 的源码/CMake 子集
- `SSLSocket.c` 保留本地 OpenSSL 3.x 兼容补丁：
  - 初始化：`#if OPENSSL_VERSION_NUMBER >= 0x30000000L` 使用 `OPENSSL_init_ssl()`
  - 终止：用 `#if` 守卫 CRYPTO_set_locking_callback、ERR_free_strings、sslLocks 清理

### 2.4 paho-mqtt-cpp 1.5.1

- `src/mqtt/include/mqtt/` 头文件、`src/mqtt/src/` 源文件、`src/mqtt/cmake/` 模块按构建需要更新
- `src/mqtt/CMakeLists.txt` 版本号 1.4.0 改为 1.5.1
- 新增文件：`reason_code.h/cpp`、`server_response.cpp`、`event.h`
- 移除文件：`subscribe_options.cpp`（合并到其他模块）

### 2.5 Windows 链接修复

OpenSSL 3.x 的 `libcrypto.lib` 新增了对 Windows Crypto API（CertOpenStore 等）的依赖：

| 文件 | 改动 |
|------|------|
| `src/libslic3r/CMakeLists.txt` | 新增 `crypt32.lib ws2_32.lib` 链接 |
| `tests/libslic3r/CMakeLists.txt` | 测试链接新增 `crypt32.lib` |

### 2.6 单元测试

| 文件 | 说明 |
|------|------|
| `tests/libslic3r/TestMD5.cpp` | 5 个测试用例：已知内容哈希、64 KiB 边界、空文件、目录输入、不存在文件 |
| `tests/libslic3r/CMakeLists.txt` | 添加 TestMD5.cpp 到编译列表 |

---

## 3. 不涉及改动的文件

| 文件 | 原因 |
|------|------|
| `CreatePresetsDialog.cpp` | 已使用 EVP API |
| `GUI_App.cpp` | HMAC/EVP API 在 3.x 中不变 |
| `Http.cpp` | X509 API 稳定 |
| `SimplyPrint.cpp` | SHA256() 在 3.x 仍可编译（仅弃用警告） |
| `SSWCP.cpp` / `MQTT.cpp` | 本次未改动（属于独立的工作分支） |

---

## 4. 测试指南

### 4.1 单元测试

```powershell
cmake --build build --target libslic3r_tests --config Release
build\tests\libslic3r\Release\libslic3r_tests.exe "[MD5]"
```

**预期**: 5/5 通过（12 assertions）

### 4.2 功能测试（按优先级）

#### P0: SSWCP 打印机连接
- **操作**: 局域网发现并连接 Snapmaker 打印机
- **覆盖**: OpenSSL 3.5.7 TLS 握手 + SHA256 签名
- **预期**: 连接成功，状态实时刷新
- **状态**: 已验证通过

#### P0: 文件上传（MD5 校验）
- **操作**: 上传 G-code / 3MF 到打印机
- **覆盖**: bbl_calc_md5() EVP API 路径
- **预期**: 上传成功，打印机端校验通过
- **状态**: 待验证

#### P0: 3MF 导出与导入
- **操作**: 切片后导出含 G-code 的 .3mf，重新导入
- **覆盖**: bbs_3mf.cpp 的 EVP MD5 路径
- **预期**: 导出成功，MD5 正确写入，导入时校验通过
- **状态**: 待验证

#### P1: 预设更新检查（HTTPS）
- **操作**: Help -> Check for Process Preset Updates
- **覆盖**: libcurl + OpenSSL 3.5.7 HTTPS 请求链路
- **预期**: 弹窗提示"已是最新版本"或"有可用更新"
- **状态**: 已验证通过

#### P1: MQTT 连接稳定性
- **操作**: 连接打印机后保持 10 分钟以上，观察断连重连
- **覆盖**: paho-mqtt-c 1.3.14 + SSLSocket.c 补丁
- **预期**: 连接稳定，断网后自动重连，无 OpenSSL 初始化崩溃
- **状态**: 待验证

#### P2: 创建自定义耗材
- **操作**: 创建自定义耗材预设
- **覆盖**: CreatePresetsDialog.cpp EVP MD5（未改动，依赖 OpenSSL 链接）
- **预期**: filament ID 正确生成（P + 7位 hex）
- **状态**: 待验证

### 4.3 稳定性测试

| 测试项 | 操作 | 预期 |
|--------|------|------|
| 冷启动 | 首次启动应用 | 无 OpenSSL 初始化崩溃 |
| 多次启停 | 连续 3 次启动关闭 | 无全局状态泄漏 |
| 长时间运行 | 连续运行 30 分钟 | 无内存泄漏崩溃 |

### 4.4 跨平台验证

| 平台 | 状态 |
|------|------|
| Windows (MSVC 2022) | 已通过 |
| macOS | 待验证 |
| Linux | 待验证 |

---

## 5. 构建说明

### 5.1 从源码构建（Windows）

```powershell
# 1. 构建 deps（含 OpenSSL 3.5.7 LTS）
cmake -S deps -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target deps --config Release

# 2. 配置主项目
cmake -S . -B build -DCMAKE_PREFIX_PATH="build/destdir/usr/local" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON

# 3. 构建主程序
cmake --build build --target Snapmaker_Orca_app_gui --config Release
```

### 5.2 注意事项

- 首次构建会下载 OpenSSL 3.5.7 源码（约 50.7MB），编译时间约 5-10 分钟
- 如果之前构建过任何 OpenSSL 版本（尤其是 3.2.4 或 1.1.1w），需要清除 `build/dep_OpenSSL-prefix/` 以及 `build/destdir/usr/local/` 中已安装的 OpenSSL 头文件、库、模块和可执行文件；ExternalProject 的既有时间戳不会可靠地识别这些过期产物
- VS 中打开 build/Snapmaker_Orca.sln 即可

---

## 6. 已知限制

1. **SHA256() 弃用警告**: SimplyPrint.cpp 和 SSWCP.cpp 中的 SHA256() 在 OpenSSL 3.x 中产生弃用警告但不影响编译和运行，后续可迁移到 EVP API
2. **服务器版本范围**: 预设更新检查时，如果应用版本超过服务器的 max_support_pc_version，不会触发下载——这是服务器侧配置问题，非 OpenSSL 升级问题
3. **paho-mqtt-c samples**: 示例默认不编译；只有显式设置 `PAHO_BUILD_SAMPLES=ON` 时才会构建并安装对应示例源码，默认应用包不包含 `src/samples`

---

## 7. 回滚方案

```bash
git revert <openssl-3.5.7-commit> --no-edit
```

如果 3.5.7 变更尚未提交，则用 `git restore` 恢复对应文件。回滚后需清除 build 目录中 `dep_OpenSSL-prefix/` 和已安装 OpenSSL 产物，再重新构建 deps。
