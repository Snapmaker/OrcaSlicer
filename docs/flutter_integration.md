# Flutter 桌面嵌入技术文档

## 目录

- [1. 当前实现概述](#1-当前实现概述)
- [2. 架构设计](#2-架构设计)
- [3. C++ 开发人员手册](#3-c-开发人员手册)
- [4. Flutter 开发人员手册](#4-flutter-开发人员手册)

---

## 1. 当前实现概述

### 1.1 目标

将 Snapmaker Orca 的首页从 WebView 渲染替换为 Flutter 原生渲染，提升首屏性能与 UI 一致性。

### 1.2 技术路线

- **桌面嵌入**：通过 Flutter Desktop Embedding C API 将 Flutter Engine 嵌入 wxWidgets 窗口。
- **依赖管理**：Flutter 引擎库通过 `deps/` 系统管理，不依赖系统绝对路径。
- **跨平台**：macOS / Windows / Linux 三平台支持，统一 C++ 抽象接口。

### 1.3 文件结构

```
src/slic3r/GUI/Flutter/
  flutter_host.h              # 抽象接口：FlutterEngineHost + FlutterViewHost
  flutter_host_macos.mm       # macOS 实现（Objective-C++, FlutterMacOS.framework）
  flutter_host_win.cpp        # Windows 实现（Flutter Windows C API）
  flutter_host_linux.cpp      # Linux 实现（Flutter Linux C API + X11）
  FlutterPanel.hpp            # wxWidgets 窗口封装 + Dispatcher（MethodChannel 调度表）
  FlutterPanel.cpp            # 实现

deps/Flutter/
  Flutter.cmake               # deps 构建：从 Flutter SDK 拷贝引擎库到 deps 目录

resources/flutter_app/
  App.framework/              # Flutter Dart AOT 编译产物（由 Flutter 项目构建生成）
```

### 1.4 关键 CMake 变量

| 变量 | 说明 |
|------|------|
| `FLUTTER_SDK_PATH` | Flutter SDK 根路径，deps 构建时传入 |
| `FLUTTER_APP_BUILD_DIR` | Flutter App 构建输出目录，build 脚本从中取 App.framework |
| `CMAKE_PREFIX_PATH` | deps 安装路径，Flutter 引擎库位于此处 |

### 1.5 构建流程

```
1. build_release_macos.sh -d    # deps: 从 Flutter SDK 拷贝 FlutterMacOS.framework → deps/
2. build_release_macos.sh -s    # slicer: CMake 从 CMAKE_PREFIX_PATH 找到 framework
                                #         编译后拷贝 framework 到 .app/Contents/Frameworks/
```

---

## 2. 架构设计

### 2.1 两层模型

```
┌──────────────────────────────────────────────────┐
│  FlutterEngineHost (进程级, 一个)                  │
│  - start() / stop()                              │
│  - createView(entrypoint, channelName) → View     │
│                                                  │
│  macOS: FlutterDartProject → 多个 FlutterEngine   │
│  Win:   FlutterDesktopEngineProperties            │
│  Linux: FlutterDesktopEngineProperties            │
└──────────────┬───────────────────────────────────┘
               │ createView() 创建
               ▼
┌──────────────────────────────────────────────────┐
│  FlutterViewHost (每个 Panel 一个)                 │
│  - embedInto(void* nativeHandle)                 │
│  - invokeMethod(method, arguments)               │
│  - setMethodCallHandler(handler)                 │
│                                                  │
│  各平台包装原生 ViewController/View               │
└──────────────┬───────────────────────────────────┘
               │ embedInto()
               ▼
┌──────────────────────────────────────────────────┐
│  FlutterPanel : wxWindow                         │
│  - startView(engine, entrypoint, channelName)    │
│  - setHandler(handler)                           │
│  - 内部持有 unique_ptr<FlutterViewHost>           │
└──────────────────────────────────────────────────┘
```

### 2.2 Dispatcher — MethodChannel 调度表

```cpp
Dispatcher d;
d.on("getVersion", [](auto args, auto reply) {
    reply("orca-1.0");
})
.on("openFile", [](auto args, auto reply) {
    // args: JSON string from Dart
    // reply: return JSON string to Dart
    reply(R"({"ok": true})");
});

// 将 handler 注册到 Panel
m_flutter_panel->setHandler(d.handler());
```

**重要**：`handler()` 返回的 lambda 按值捕获 map，Dispatcher 可以在栈上创建，安全析构。

### 2.3 各平台嵌入方式

| 平台 | 原生窗口 | 嵌入方式 |
|------|---------|---------|
| macOS | NSView | `[parentView addSubview:flutterView]` + autoresizingMask |
| Windows | HWND | `SetParent(flutterHwnd, parentHwnd)` + SetWindowPos |
| Linux | GtkWidget / X11 Window | `XReparentWindow` + XResizeWindow |

### 2.4 Tab 生命周期

```
用户切换到 Flutter Tab
  → PAGE_CHANGED: sel == tpFlutterTest
    → m_flutter_panel->view()->invokeMethod("onPageState", "active")

用户切出 Flutter Tab
  → PAGE_CHANGED: sel != tpFlutterTest && prev == tpFlutterTest
    → m_flutter_panel->view()->invokeMethod("onPageState", "inactive")

应用退出
  → MainFrame::shutdown()
    → m_flutter_engine->stop()
    → delete m_flutter_engine
```

---

## 3. C++ 开发人员手册

### 3.1 快速上手：添加新的 MethodChannel 方法

1. 找到 `MainFrame.cpp` 中创建 Dispatcher 的位置（约第 1207 行）。
2. 添加新的 `.on()` 调用：

```cpp
Dispatcher d;
d.on("getVersion", [](auto, auto reply) { reply("orca-1.0"); })
 .on("yourNewMethod", [](auto args, auto reply) {
     // args 是从 Dart 传来的 JSON 字符串
     // 做你的业务逻辑...
     reply(R"({"result": "ok"})");  // 返回 JSON 给 Dart
 });
```

3. Flutter 侧调用 `channel.invokeMethod('yourNewMethod', jsonArgs)`。

### 3.2 添加新的 FlutterPanel

```cpp
// 1. 创建 Panel（如果还没有）
auto* panel = new FlutterPanel(m_tabpanel);
m_tabpanel->AddPage(panel, _L("My Tab"), ...);

// 2. 创建 View（在 CallAfter 中，确保 Panel 已就绪）
CallAfter([this, panel] {
    if (!panel->startView(m_flutter_engine, "entryPointName", "my/channel")) {
        BOOST_LOG_TRIVIAL(error) << "startView failed";
        return;
    }

    Dispatcher d;
    d.on("method1", [](auto args, auto reply) { ... })
     .on("method2", [](auto args, auto reply) { ... });
    panel->setHandler(d.handler());
});
```

### 3.3 Entrypoint 说明

| entrypoint | 对应 Dart 函数 | 说明 |
|-----------|---------------|------|
| `"main"` | `main()` | 默认入口 |
| `"homeMain"` | `homeMain()` | 首页专用（当前测试入口） |

**规则**：如果 entrypoint 不是 `"main"`，Dart 侧必须有 `@pragma('vm:entry-point')` 标注的函数。

### 3.4 Channel 命名规范

Channel 名称格式为 `"模块/页面名"`，例如：
- `"snapmaker/home"` — 首页
- `"snapmaker/device"` — 设备页面
- `"snapmaker/print"` — 打印页面

**一个 Engine 只能注册一次同名 channel**。如果多个 Panel 要共享 Engine，需要不同的 channel 名称。

### 3.5 创建新的平台 FlutterEngineHost

如果需要添加新平台（如 Fuchsia），需要实现：

```cpp
class FlutterEngineHostMyPlatform : public FlutterEngineHost {
    bool start() override;     // 初始化引擎
    void stop() override;      // 销毁引擎
    std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,
        const std::string& channelName) override;
};
```

并在 `createFlutterEngine` 工厂函数中添加平台分支。

### 3.6 添加 Flutter 依赖（deps 系统）

`deps/Flutter/Flutter.cmake` 负责从 Flutter SDK 拷贝引擎库到 deps 目录。

**添加新平台依赖**：在 `Flutter.cmake` 中添加 `elseif (MY_PLATFORM)` 分支，指定：
- 引擎头文件路径
- 引擎库文件路径
- 复制目标路径 (`${DESTDIR}/include/`, `${DESTDIR}/lib/`, `${DESTDIR}/bin/`)

**CI 构建**：使用 `subosito/flutter-action@v2` 安装 Flutter SDK，deps 自动检测 `FLUTTER_HOME` 环境变量。

### 3.7 macOS: 更新 App.framework

App.framework 是 Flutter App 的 AOT 编译产物，放在 `resources/flutter_app/App.framework`。

更新方式：
```bash
# 方式 1：如果有 Flutter App 源码
cd lava_app/lava_app/lava-orca
flutter build macos --debug
cp -R build/macos/Build/Products/Debug/App.framework \
     ../../../OrcaSlicer/resources/flutter_app/

# 方式 2：指定构建目录
export FLUTTER_APP_BUILD_DIR=/path/to/flutter/build/output
./build_release_macos.sh -s
```

### 3.8 常见问题

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| 黑屏 | `engine.viewController` 未设置 | macOS 实现必须设置 `engine.viewController = controller` |
| "C++ not connected" | Channel 名称不匹配 | C++ 和 Dart 的 channel 名称必须完全一致 |
| crash on MethodChannel | Dispatcher use-after-free | `handler()` 按值捕获 map，不要捕获 `this` |
| dyld: Library not loaded | rpath 不正确 | macOS rpath 设为 `@executable_path/../Frameworks` |
| 引擎初始化失败 | App.framework 缺失 | 确保 App.framework 在 app bundle 的 Frameworks 中 |

---

## 4. Flutter 开发人员手册

### 4.1 项目结构

Flutter App 源码位于 `lava_app/lava_app/lava-orca/`，使用 Melos monorepo 管理。

```
lava-orca/lib/
  main.dart                  # 主入口 + 路由 + 初始化
  features/                  # 业务功能模块
    device_control/
    device_connections/
    wcp/
  src/
    bridge/                  # C++ ↔ Dart 桥接层
    viewmodels/              # 状态管理
    pages/                   # 页面
    config/                  # 常量/配置
```

### 4.2 入口函数要求

桌面嵌入会调用特定的 entrypoint。非 `main` 的入口函数**必须**添加注解：

```dart
@pragma('vm:entry-point')
void homeMain() {
  // 首页入口逻辑
  runApp(MyHomeApp());
}
```

C++ 侧调用时传入 entrypoint 名称：
```cpp
m_flutter_panel->startView(m_flutter_engine, "homeMain", "snapmaker/home");
```

### 4.3 与 C++ 通信（MethodChannel）

#### Dart 侧

```dart
import 'package:flutter/services.dart';

class OrcaBridge {
  static const channel = MethodChannel('snapmaker/home');

  // 调用 C++ 方法
  static Future<String> getVersion() async {
    final result = await channel.invokeMethod('getVersion', '');
    return result.toString();
  }

  // 调用 C++ 方法 + 传参（JSON 字符串）
  static Future<String> sendMessage(String msg) async {
    return await channel.invokeMethod('sendMessage', msg);
  }

  // 接收 C++ 的主动调用
  static void setupHandlers() {
    channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'onPageState':
          final state = call.arguments.toString();
          // 处理页面状态变化 (active / inactive)
          break;
        case 'onThemeChange':
          // 处理主题变化
          break;
        default:
          throw MissingPluginException();
      }
    });
  }
}
```

#### C++ 侧（已注册的方法）

C++ 通过 `Dispatcher` 注册处理方法，用 `invokeMethod` 主动调用 Dart：

```cpp
// C++ 接收 Dart 调用
d.on("getVersion", [](auto, auto reply) { reply("orca-1.0"); });

// C++ 主动调用 Dart
m_view->invokeMethod("onPageState", "active");
```

### 4.4 页面生命周期

Flutter Panel 的可见性由 Notebook tab 切换控制。当用户切换 tab 时，C++ 会调用：

- `invokeMethod("onPageState", "active")` — 页面进入可见状态
- `invokeMethod("onPageState", "inactive")` — 页面进入不可见状态

Flutter 侧应监听这些事件并适当处理（如暂停/恢复轮询、动画等）。

### 4.5 Channel 命名规范

Channel 名称格式为 `"snapmaker/<模块>"`：

| Channel | 用途 |
|---------|------|
| `snapmaker/home` | 首页 |
| `snapmaker/device` | 设备控制 |
| `snapmaker/print` | 打印控制 |

**注意**：Channel 名称在 C++ 和 Dart 两侧必须完全一致，否则 MethodChannel 无法建立连接。

### 4.6 构建与调试

#### 桌面 AOT 构建

```bash
cd lava_app/lava_app/lava-orca

# Debug 模式（热重载不支持，需重新构建）
flutter build macos --debug

# 产物位置
# macOS: build/macos/Build/Products/Debug/App.framework
# 注意：实际会生成在 build/macos/Build/Products/Debug/ 但可能路径不同
# 查找: find build -name "App.framework" -type d
```

#### 构建后复制到 Orca 项目

```bash
# 找到 App.framework
APP_FW=$(find build -name "App.framework" -maxdepth 5 -type d | head -1)

# 复制到 Orca 项目资源目录
cp -R "$APP_FW" ../../../OrcaSlicer/resources/flutter_app/App.framework

# 重新编译 Orca（仅 slicer）
cd ../../../OrcaSlicer
./build_release_macos.sh -s
```

#### 调试技巧

1. **查看日志**：Orca 控制台会输出 `[Flutter]` 前缀的日志。
2. **C++ ↔ Dart 通信**：在 Dispatcher handler 中添加 `BOOST_LOG_TRIVIAL` 打印参数。
3. **黑屏问题**：确认 App.framework 在 `.app/Contents/Frameworks/` 中。
4. **MethodChannel 不通**：确认 C++ 和 Dart 的 channel 名称完全一致。

### 4.7 新增 MethodChannel 方法（Dart → C++）

1. 在 C++ 侧注册 handler：
   ```cpp
   d.on("newMethod", [](auto args, auto reply) {
       // 处理逻辑
       reply("result_json");
   });
   ```

2. 在 Dart 侧调用：
   ```dart
   final result = await channel.invokeMethod('newMethod', 'args_json');
   ```

### 4.8 新增 C++ → Dart 的主动调用

1. 在 Dart 侧注册 handler：
   ```dart
   channel.setMethodCallHandler((call) async {
     if (call.method == 'onNewEvent') {
       // 处理事件
     }
   });
   ```

2. 在 C++ 侧调用：
   ```cpp
   m_view->invokeMethod("onNewEvent", "event_data_json");
   ```

### 4.9 常见问题

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| "MissingPluginException" | C++ 没注册该方法 | 检查 Dispatcher 中是否 `.on("method", ...)` |
| Channel 无响应 | 名称不匹配 | 确认 C++ 和 Dart channel 名称完全一致 |
| 页面显示空白 | entrypoint 不存在 | 检查 Dart 侧是否有对应的 `@pragma('vm:entry-point')` 函数 |
| 参数解析错误 | JSON 格式问题 | Dart → C++ 参数是字符串，C++ 不做 JSON 解析，按原字符串传递 |
