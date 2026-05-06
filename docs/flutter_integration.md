# Flutter 桌面嵌入技术文档

## 目录

- [Part 1: 集成概述（所有人）](#part-1-集成概述)
  - [1.1 目标](#11-目标)
  - [1.2 整体架构](#12-整体架构)
  - [1.3 各平台嵌入原理](#13-各平台嵌入原理)
  - [1.4 通信机制：MethodChannel](#14-通信机制methodchannel)
  - [1.5 构建流程](#15-构建流程)
  - [1.6 项目文件结构](#16-项目文件结构)
- [Part 2: C++ 开发人员手册](#part-2-c-开发人员手册)
  - [2.1 快速上手：添加一个新的 Flutter 页面](#21-快速上手添加一个新的-flutter-页面)
  - [2.2 架构详解](#22-架构详解)
  - [2.3 Dispatcher 调度表](#23-dispatcher-调度表)
  - [2.4 生命周期管理](#24-生命周期管理)
  - [2.5 线程安全](#25-线程安全)
  - [2.6 添加新平台](#26-添加新平台)
  - [2.7 构建系统：Flutter 依赖如何工作](#27-构建系统flutter-依赖如何工作)
- [Part 3: Flutter 开发人员手册](#part-3-flutter-开发人员手册)
  - [3.1 项目结构](#31-项目结构)
  - [3.2 入口函数](#32-入口函数)
  - [3.3 MethodChannel 通信](#33-methodchannel-通信)
  - [3.4 页面生命周期](#34-页面生命周期)
  - [3.5 构建 App.framework](#35-构建-appframework)
  - [3.6 调试技巧](#36-调试技巧)
  - [3.7 常见问题](#37-常见问题)
- [附录 A: Channel 命名规范](#附录-a-channel-命名规范)
- [附录 B: 参考链接](#附录-b-参考链接)

---

## Part 1: 集成概述

### 1.1 目标

将 Snapmaker Orca 的 UI 从 WebView 逐步迁移到 Flutter 原生渲染。

**为什么这样做？**

- WebView 启动慢，首屏白屏时间长
- Web 技术栈和原生控件的 UI 风格难以统一
- Flutter 提供跨平台一致的 Skia 渲染，性能接近原生

**当前状态：**

- macOS 实现完整（FlutterMacOS.framework）
- Windows / Linux 骨架已完成，待实机验证
- 以独立测试 Tab（tpFlutterTest）运行，不影响现有功能

### 1.2 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                    MainFrame (wxWidgets)                 │
│                                                         │
│  ┌──────────────────────────────────────────────────┐  │
│  │  FlutterEngineHost (进程级, 一个)                  │  │
│  │  - start() / stop()                              │  │
│  │  - createView(entrypoint, channelName) → View     │  │
│  │                                                  │  │
│  │  负责管理 Flutter Engine 的生命周期。              │  │
│  │  macOS: 一个 FlutterDartProject, 多个 Engine      │  │
│  │  Win:   每个 View 一个独立的 Engine               │  │
│  └──────────────┬───────────────────────────────────┘  │
│                 │ createView() 创建                     │
│                 ▼                                      │
│  ┌──────────────────────────────────────────────────┐  │
│  │  FlutterViewHost (每个页面一个)                    │  │
│  │  - embedInto(void* nativeHandle)                 │  │
│  │  - invokeMethod(method, arguments)               │  │
│  │  - setMethodCallHandler(handler)                 │  │
│  │                                                  │  │
│  │  对应一个 Flutter View/ViewController。           │  │
│  │  每个 View 有独立的 MethodChannel。               │  │
│  └──────────────┬───────────────────────────────────┘  │
│                 │ 被 wxWindow 持有                     │
│                 ▼                                      │
│  ┌──────────────────────────────────────────────────┐  │
│  │  FlutterPanel : wxWindow                         │  │
│  │  - startView(engine, entrypoint, channel)         │  │
│  │  - setHandler(handler)                           │  │
│  │  - view() → FlutterViewHost*                     │  │
│  │                                                  │  │
│  │  wxWidgets 窗口控件，持有 ViewHost。              │  │
│  └──────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘

通信链路:
  Dart  ←──MethodChannel──→  FlutterViewHost  ←──Dispatcher──→  C++ 业务逻辑
                                  │
                                  └── 各平台实现不同，但接口统一
```

**关键设计决策：**

- **两层分离**：Engine（进程级）和 View（页面级）分开，支持多个 Flutter 页面共享同一个 Engine
- **平台抽象**：`FlutterEngineHost` / `FlutterViewHost` 是纯虚接口，每个平台有自己的实现文件
- **Dispatcher 模式**：MethodChannel 的 handler 通过一个链式调度表注册，比 switch-case 更简洁

### 1.3 各平台嵌入原理

Flutter 桌面嵌入的核心思路：**将 Flutter 的原生窗口/View 作为子窗口嵌入到 wxWidgets 窗口中**。

| 平台 | Flutter 原生窗口类型 | 嵌入方式 | 关键 API |
|------|---------------------|---------|---------|
| macOS | NSView (`FlutterViewController.view`) | `addSubview` + autoresizingMask | `FlutterMacOS.framework` (Objective-C) |
| Windows | HWND | `SetParent` + `SetWindowPos` | `flutter_windows.h` (C API) |
| Linux | X11 Window | `XReparentWindow` + `XResizeWindow` | `flutter_linux.h` (C API) + X11 |

#### macOS 详细流程

```
1. FlutterEngineHostMacOS::start()
   → 创建 FlutterDartProject（加载 App.framework 中的 Dart AOT 产物）

2. FlutterEngineHostMacOS::createView(entrypoint, channelName)
   → 创建 FlutterEngine（设置 entrypoint）
   → engine.runWithEntrypoint(nsEntry)  // 启动 Dart 代码
   → 创建 FlutterViewHostMacOS(engine, channelName)
     → 创建 FlutterViewController（关键：设置 engine.viewController）
     → 注册 MethodChannel

3. FlutterPanel::startView()
   → 调用 host.createView()
   → 调用 view->embedInto(GetHandle())
     → [parentView addSubview:flutterViewController.view]
     → 设置 frame 和 autoresizingMask
```

**macOS 关键点**：必须设置 `engine.viewController = controller`，否则 engine 不知道要把渲染帧发送给哪个 view controller，导致黑屏。

#### Windows 详细流程

```
1. FlutterEngineHostWin::start()
   → 空操作（Windows 引擎在 createView 时创建）

2. createView(entrypoint, channelName)
   → FlutterDesktopEngineCreate(&props)
   → FlutterDesktopEngineRun(engine, entrypoint)
   → FlutterDesktopViewControllerCreate(800, 600, engine)
   → 创建 FlutterViewHostWin(controller, engine, channelName)
     → 通过 FlutterDesktopEngineGetMessenger 注册 message callback

3. embedInto(parentHwnd)
   → FlutterDesktopViewGetHWND(view)  // 获取 Flutter 窗口句柄
   → SetParent(childHwnd, parentHwnd)  // Windows 子窗口重父
   → SetWindowPos(...)                 // 调整大小填满父窗口
```

**Windows 关键点**：使用 `FlutterDesktopViewControllerRef` 而非直接操作 HWND。message callback 是异步的，需要通过 `FlutterDesktopMessengerSendResponse` 回复。

#### Linux 详细流程

```
1. FlutterEngineHostLinux::start()
   → 空操作

2. createView(entrypoint, channelName)
   → FlutterDesktopEngineCreate(&props)
   → FlutterDesktopEngineRun(engine, entrypoint)
   → FlutterDesktopViewCreate(800, 600, engine)
   → 创建 FlutterViewHostLinux(view, engine, channelName)

3. embedInto(parentHandle)
   → parentHandle 是 GtkWidget*（来自 wxWindow::GetHandle()）
   → gtk_widget_get_window → GDK Window → X11 Window ID
   → FlutterDesktopViewGetId(view) → Flutter 的 X11 Window ID
   → XReparentWindow(display, childXid, parentXid, 0, 0)
   → XResizeWindow + XMapWindow
```

**Linux 关键点**：需要 GDK/X11 互操作。wxWidgets on Linux 底层是 GTK，`GetHandle()` 返回 `GtkWidget*`，需要穿透 GDK 拿到 X11 Window ID。

### 1.4 通信机制：MethodChannel

C++ 和 Dart 之间的通信完全通过 Flutter 的 **MethodChannel** 机制：

```
Dart 侧                                    C++ 侧
══════════════════════════                ══════════════════════════

// 调用 C++ 方法
channel.invokeMethod(          ─────→    Dispatcher::handler()
  'getVersion', args)                   查找 "getVersion" → 执行函数
                                  ─────→   reply("orca-1.0")
// 得到返回值
result = "orca-1.0"

// C++ 主动调用 Dart
channel.setMethodCallHandler(   ←─────  m_view->invokeMethod(
  (call) { ... })                         "onPageState", "active")
```

**数据格式**：所有参数和返回值都是字符串。通常用 JSON 格式，但 C++ 侧**不做自动 JSON 解析**——Dart 侧调用时传入 JSON 字符串，C++ 侧的 handler 收到的是原始字符串，需要自行解析（如果需要）。

**限制**：同一个 Engine 中，同一个 channel 名称只能注册一次。如果多个页面需要通信，使用不同的 channel 名称。

### 1.5 构建流程

```
Flutter 项目                                 Orca 项目
══════════════                               ══════════

1. flutter build macos --debug       ──→    resources/flutter_app/App.framework/
   (生成 Dart AOT 产物)                    (或通过 FLUTTER_APP_BUILD_DIR 指定)

                                          2. build_release_macos.sh -d  (构建依赖)
                                             Flutter.cmake 从 Flutter SDK 拷贝:
                                               FlutterMacOS.framework → deps/build/.../FlutterMacOS.framework

                                          3. build_release_macos.sh -s  (构建 slicer)
                                             CMake 从 CMAKE_PREFIX_PATH 找到 framework
                                             编译 flutter_host_macos.mm
                                             链接 FlutterMacOS.framework

                                          4. build_release_macos.sh (打包)
                                             复制 FlutterMacOS.framework → .app/Contents/Frameworks/
                                             复制 App.framework         → .app/Contents/Frameworks/
                                             设置 rpath: @executable_path/../Frameworks
```

**CI 构建**：
- `subosito/flutter-action@v2` 安装 Flutter SDK（设置 `FLUTTER_HOME` 环境变量）
- `flutter precache` 下载桌面引擎产物
- `Flutter.cmake` 自动检测 `FLUTTER_HOME`，从缓存目录拷贝引擎库

### 1.6 项目文件结构

```
deps/Flutter/
  Flutter.cmake               # deps 构建：从 Flutter SDK 拷贝引擎库到 CMAKE_PREFIX_PATH

resources/flutter_app/
  App.framework/              # Dart AOT 编译产物（Flutter 项目构建输出）
                              # 包含 kernel_blob.bin, App 可执行文件等

src/slic3r/GUI/Flutter/
  flutter_host.h              # 抽象接口：FlutterEngineHost + FlutterViewHost
  flutter_host_macos.mm       # macOS 实现（Objective-C++, FlutterMacOS.framework）
  flutter_host_win.cpp        # Windows 实现（Flutter Windows C API）
  flutter_host_linux.cpp      # Linux 实现（Flutter Linux C API + X11）
  FlutterPanel.hpp            # wxWidgets 窗口封装 + Dispatcher（MethodChannel 调度表）
  FlutterPanel.cpp            # 实现
```

---

## Part 2: C++ 开发人员手册

### 2.1 快速上手：添加一个新的 Flutter 页面

假设你要添加一个设备控制页面（`tpDevice`）。

**Step 1: 在 MainFrame.hpp 中添加成员变量**

```cpp
// 在 TabPosition 枚举中添加新的 Tab 索引
enum TabPosition {
    tpHome = 0,
    // ... 其他 tab ...
    tpFlutterTest = 9,
    tpDevice     = 10,   // 新增
};

// 在成员变量区域添加
FlutterPanel*      m_device_panel{nullptr};
FlutterViewHost*   m_device_view{nullptr};  // 可选，如果其他地方不需要直接引用
```

**Step 2: 在 MainFrame.cpp 中创建 Panel 和 View**

找到创建 UI 的地方（通常是构造函数中 `m_tabpanel->AddPage` 所在区域），添加：

```cpp
// 创建设备页面 FlutterPanel
m_device_panel = new FlutterPanel(m_tabpanel);
m_tabpanel->AddPage(m_device_panel, _L("Device"),
    std::string("tab_device_active"),
    std::string("tab_device_active"), false);

// 在 CallAfter 中启动 Flutter View（确保 Panel 已就绪）
CallAfter([this] {
    if (!m_device_panel->startView(m_flutter_engine, "deviceMain", "snapmaker/device")) {
        BOOST_LOG_TRIVIAL(error) << "[Flutter] Device panel startView failed";
        return;
    }

    // 注册 MethodChannel 方法
    Dispatcher d;
    d.on("getDeviceList", [](auto args, auto reply) {
        // 查询设备列表，返回 JSON
        std::string devices = R"([{"id":1,"name":"Printer A"}])";
        reply(devices);
    })
    .on("connectDevice", [](auto args, auto reply) {
        // args 是来自 Dart 的 JSON，如 {"deviceId": 1}
        // 执行连接逻辑...
        reply(R"({"ok": true})");
    });
    m_device_panel->setHandler(d.handler());
});
```

**Step 3: 在 PAGE_CHANGED 事件中添加生命周期通知**

找到 `MainFrame::OnPageChanged`（或类似的 tab 切换事件处理函数）：

```cpp
// 离开设备页面时通知
if (prev_monitored_tab == tpDevice && sel != tpDevice) {
    if (m_device_panel && m_device_panel->view()) {
        m_device_panel->view()->invokeMethod("onPageState", "inactive");
    }
}

// 进入设备页面时通知
if (sel == tpDevice) {
    if (m_device_panel && m_device_panel->view()) {
        m_device_panel->view()->invokeMethod("onPageState", "active");
    }
    prev_monitored_tab = tpDevice;
}
```

**Step 4: 在 shutdown 中确保清理**

Engine 的 `stop()` 会自动销毁所有 View。Panel 会被 wxWidgets 销毁。

FlutterPanel 的 wxWindow 析构会自动释放 `m_view`（unique_ptr）。

### 2.2 架构详解

#### FlutterEngineHost（进程级）

```
职责：
  - 初始化 / 销毁 Flutter Engine
  - 创建 FlutterViewHost 实例

生命周期：
  MainFrame 构造 → createFlutterEngine() → engine->start()
  MainFrame 析构 → engine->stop() → delete engine

平台差异：
  macOS: start() 创建 FlutterDartProject，它是渲染的基础设施。
         每个 createView() 创建一个新的 FlutterEngine 实例。
         FlutterViewController 通过 engine.viewController 与 engine 关联。

  Windows: start() 是空操作。
           每个 createView() 创建独立的 FlutterDesktopEngine。
           Engine 生命周期绑定到 View。

  Linux: 同 Windows，每个 View 一个 Engine。
```

#### FlutterViewHost（页面级）

```
职责：
  - 将 Flutter 内容嵌入到原生窗口中
  - Dart ↔ C++ 通信

接口：
  embedInto(void* parentHandle)    → 嵌入到父窗口（参数是平台原生句柄）
  invokeMethod(method, arguments)  → C++ → Dart 调用
  setMethodCallHandler(handler)    → 注册 Dart → C++ 的回调

所有权：
  std::unique_ptr<FlutterViewHost> m_view;  // FlutterPanel 独占所有权
  析构时自动清理平台资源（ViewController、HWND、X11 Window）
```

#### FlutterPanel（wxWidgets 层）

```
继承：wxWindow

职责：
  - 作为 wxWidgets 的窗口控件，可以被添加到 Notebook（Tab 控件）
  - 持有 FlutterViewHost
  - 在窗口大小变化时通知 Flutter

关键方法：
  startView(engine, entrypoint, channel) → 创建 View 并嵌入
  setHandler(handler)                    → 设置 MethodChannel 回调
  view()                                 → 获取 View 指针（用于 invokeMethod）
```

### 2.3 Dispatcher 调度表

Dispatcher 是 MethodChannel 回调的注册中心。采用链式 API：

```cpp
Dispatcher d;
d.on("getVersion", [](auto args, auto reply) {
    // Dart 调用 channel.invokeMethod("getVersion", "")
    // args = Dart 传来的字符串参数
    reply("orca-1.0");  // 返回给 Dart 的字符串
})
.on("sendMessage", [](auto args, auto reply) {
    BOOST_LOG_TRIVIAL(info) << "[Flutter] Received: " << args;
    reply("C++ received: " + args);
})
.on("incrementCounter", [](auto args, auto reply) {
    // 解析参数，执行业务逻辑
    int value = std::stoi(args);
    reply(std::to_string(value + 1));
});

// 注册到 Panel
m_panel->setHandler(d.handler());
```

**Dispatcher 的内部实现：**

```cpp
class Dispatcher {
    std::unordered_map<std::string, Fn> m_map;  // method → function

    FlutterViewHost::MethodCallHandler handler() {
        auto map = m_map;  // 按值捕获！关键：Dispatcher 可以安全析构
        return [map](const std::string& method,
                     const std::string& args,
                     FlutterViewHost::Reply reply) {
            auto it = map.find(method);
            if (it != map.end())
                it->second(args, reply);  // 找到 → 执行
            else
                reply("");  // 未找到 → 返回空字符串，不抛异常
        };
    }
};
```

**为什么按值捕获 map：**
- `handler()` 返回的 lambda 被传递给 FlutterViewHost，后者生命周期可能很长
- Dispatcher 通常在栈上创建，method handler 注册完就析构了
- 按值捕获 map 确保 lambda 独立拥有数据，不依赖 Dispatcher 的生命周期
- **不要**在 handler 中捕获 `this` 指针，避免 use-after-free

**参数说明：**

| 参数 | 类型 | 说明 |
|------|------|------|
| args | `const std::string&` | Dart 传来的参数字符串（通常是 JSON） |
| reply | `Reply = function<void(const string&)>` | 回调函数，调用它来返回结果给 Dart |

### 2.4 生命周期管理

#### Engine 生命周期

```
MainFrame 构造
  → m_flutter_engine = createFlutterEngine("", "").release();
  → m_flutter_engine->start();
      // Engine 启动，加载 Dart AOT 产物

... 应用运行中，可以创建多个 View ...

MainFrame::shutdown()
  → m_flutter_engine->stop();
      // 关闭所有 Engine，销毁 Dart 运行时
  → delete m_flutter_engine;
      // 释放 Engine Host
```

#### View 生命周期

```
创建:
  FlutterPanel::startView(engine, entrypoint, channel)
    → engine->createView(entrypoint, channel)
        → 创建平台 Engine/ViewController
        → 运行 Dart entrypoint
        → 返回 unique_ptr<FlutterViewHost>
    → view->embedInto(GetHandle())
        → 将 Flutter View 嵌入到 wxWindow 中

Tab 切换:
  PAGE_CHANGED 事件
    → 离开时: view->invokeMethod("onPageState", "inactive")
    → 进入时: view->invokeMethod("onPageState", "active")
    Flutter 侧自行处理可见性变化

销毁:
  FlutterPanel::~FlutterPanel() (wxWindow 析构)
    → m_view.reset() (unique_ptr 自动析构)
        → 平台资源清理
```

#### Tab 切换的完整流程

```
用户点击 Tab "Device"
  → 触发 wxNotebook PAGE_CHANGED 事件
  → MainFrame 中的事件处理:
      1. 检查之前是哪个 tab (prev_monitored_tab)
         如果之前的 tab 需要通知，发送 "inactive"
      2. 检查新选中的 tab
         如果新 tab 是 Flutter 页面，发送 "active"
      3. 更新 prev_monitored_tab

注意：不要在 PAGE_CHANGED 里做阻塞操作。
      使用 CallAfter 延迟非关键操作。
```

### 2.5 线程安全

**基本规则：所有 Flutter 相关操作都必须在主线程（wxWidgets UI 线程）执行。**

Flutter Engine 本身不是线程安全的。MethodChannel 的回调也在主线程触发。

如果你需要在后台线程处理数据，然后更新 Flutter UI：

```cpp
// 后台线程完成工作后
CallAfter([this] {
    if (m_panel && m_panel->view()) {
        m_panel->view()->invokeMethod("onDataReady", jsonResult);
    }
});
```

### 2.6 添加新平台

假设要添加一个新的平台，需要：

**Step 1: 创建 `flutter_host_myplatform.cpp`**

```cpp
// flutter_host_myplatform.cpp
class FlutterViewHostMyPlatform : public FlutterViewHost {
    // 实现四个纯虚方法:
    void embedInto(void* parentView) override { /* 平台特定嵌入逻辑 */ }
    void invokeMethod(const std::string& method, const std::string& arguments) override { ... }
    void setMethodCallHandler(MethodCallHandler handler) override { ... }
};

class FlutterEngineHostMyPlatform : public FlutterEngineHost {
    bool start() override { /* 初始化引擎 */ }
    void stop() override { /* 销毁引擎 */ }
    std::unique_ptr<FlutterViewHost> createView(...) override { /* 创建 View */ }
};

std::unique_ptr<FlutterEngineHost> createFlutterEngine(...) {
    return std::make_unique<FlutterEngineHostMyPlatform>();
}
```

**Step 2: 将源文件加入 slic3r/CMakeLists.txt 的平台段**

```cmake
if (MY_PLATFORM)
    list(APPEND SLIC3R_GUI_SOURCES
        GUI/Flutter/flutter_host_myplatform.cpp
    )
endif()
```

**Step 3: 在 deps/Flutter/Flutter.cmake 添加平台分支**

```cmake
elseif (MY_PLATFORM)
    # 指定引擎产物路径和复制目标
    set(_flutter_lib "${_engine_cache}/myplatform-x64/libflutter_myplatform.so")
    # ...
    add_custom_target(dep_Flutter ALL ...)
```

**Step 4: 在 src/CMakeLists.txt 添加链接**

```cmake
elseif (MY_PLATFORM)
    target_link_libraries(Snapmaker_Orca
        "${CMAKE_PREFIX_PATH}/lib/libflutter_myplatform.so")
```

### 2.7 构建系统：Flutter 依赖如何工作

Flutter 引擎库通过 `deps/` 系统与项目集成，而非使用系统绝对路径。

**deps/Flutter/Flutter.cmake 的工作流程：**

```
1. 查找 Flutter SDK
  优先顺序: FLUTTER_SDK_PATH cmake 参数
          → FLUTTER_HOME 环境变量
          → Homebrew (macOS): /opt/homebrew/Caskroom/flutter/*/
          → $PATH 中的 flutter 命令

2. 定位引擎产物
  macOS:   $SDK/bin/cache/artifacts/engine/darwin-x64/FlutterMacOS.xcframework/
  Windows: $SDK/bin/cache/artifacts/engine/windows-x64/client_wrapper/
  Linux:   $SDK/bin/cache/artifacts/engine/linux-x64/client_wrapper/

3. 拷贝到 deps 安装目录
  macOS:   FlutterMacOS.framework → ${DESTDIR}/FlutterMacOS.framework
  Windows: .h .dll .lib           → ${DESTDIR}/include/  lib/  bin/
  Linux:   .h .so                 → ${DESTDIR}/include/  lib/
```

**在 slicer 构建时引用：**

`CMAKE_PREFIX_PATH` 指向 deps 安装目录。CMakeLists.txt 中检查文件是否存在，如果存在就链接：

```cmake
# macOS 示例
if(EXISTS "${CMAKE_PREFIX_PATH}/FlutterMacOS.framework")
    target_link_libraries(Snapmaker_Orca "-F${CMAKE_PREFIX_PATH}" "-framework FlutterMacOS")
    target_link_options(Snapmaker_Orca PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
endif()
```

如果 deps 中没有 Flutter，会打印 warning 但**不会导致编译失败**——Flutter Panel 代码仍然编译，只是运行时不会显示 Flutter 内容。

---

## Part 3: Flutter 开发人员手册

### 3.1 项目结构

Flutter App 源码位于 `lava_app/lava_app/lava-orca/`。

```
lava-orca/lib/
  main.dart                  # 主入口 + 初始化逻辑
  features/                  # 业务功能模块
    device_control/
    device_connections/
    wcp/
  src/
    bridge/                  # C++ ↔ Dart 桥接层
      orca_bridge.dart       # MethodChannel 封装
    viewmodels/              # 状态管理
    pages/                   # 各页面实现
    config/                  # 常量 / 配置
```

### 3.2 入口函数

桌面嵌入不会调用默认的 `main()` 函数（除非 entrypoint 指定为 `"main"`）。每个 Flutter 页面可以有自己的 entrypoint。

**规则：**

非 `main` 的入口函数**必须**添加 `@pragma('vm:entry-point')` 注解：

```dart
// main.dart
@pragma('vm:entry-point')
void homeMain() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const MyHomeApp());
}

@pragma('vm:entry-point')
void deviceMain() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const DeviceApp());
}

// 默认入口（entrypoint 为 "main" 或空时调用）
void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const MyApp());
}
```

**C++ 侧指定 entrypoint：**

```cpp
// 当 entrypoint 为 "main" 或空时，Flutter 调用 main()
m_panel->startView(engine, "main", "snapmaker/orca");

// 当 entrypoint 为 "deviceMain" 时，Flutter 调用 deviceMain()
m_panel->startView(engine, "deviceMain", "snapmaker/device");
```

**注意**：如果没有 `@pragma('vm:entry-point')` 注解，Flutter Engine 找不到该函数，启动会失败（`runWithEntrypoint` 返回 false）。

### 3.3 MethodChannel 通信

#### Dart → C++（调用 C++ 方法）

```dart
import 'package:flutter/services.dart';

class OrcaBridge {
  static const _channel = MethodChannel('snapmaker/orca');

  // 调用 C++ 方法
  static Future<String> getVersion() async {
    final result = await _channel.invokeMethod('getVersion', '');
    return result.toString();
  }

  // 带参数调用
  static Future<String> sendMessage(String msg) async {
    return await _channel.invokeMethod('sendMessage', msg);
  }
}
```

对应的 C++ 侧必须在 Dispatcher 中注册同名方法：

```cpp
d.on("getVersion", [](auto, auto reply) { reply("orca-1.0"); })
 .on("sendMessage", [](auto args, auto reply) { reply("received: " + args); });
```

**如果 C++ 没有注册该方法**，Dart 侧会收到异常（`MissingPluginException`）。

#### C++ → Dart（C++ 主动调用 Dart）

**Dart 侧：注册 handler**

```dart
class OrcaBridge {
  static const _channel = MethodChannel('snapmaker/orca');

  static void setupHandlers() {
    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'onPageState':
          final state = call.arguments.toString();
          // state: "active" | "inactive"
          _handlePageState(state);
          break;
        case 'onThemeChange':
          final theme = call.arguments.toString();
          _handleThemeChange(theme);
          break;
        default:
          throw MissingPluginException();
      }
    });
  }
}
```

**C++ 侧：调用**

```cpp
// 在 PAGE_CHANGED 事件中
m_view->invokeMethod("onPageState", "active");
m_view->invokeMethod("onPageState", "inactive");

// 在其他时机主动通知 Dart
m_view->invokeMethod("onThemeChange", "dark");
```

#### 数据格式约定

参数和返回值都是字符串。推荐使用 JSON：

```dart
// Dart → C++
final args = jsonEncode({'deviceId': 1, 'action': 'connect'});
await channel.invokeMethod('connectDevice', args);
```

```cpp
// C++ 侧收到的是 JSON 字符串
d.on("connectDevice", [](auto args, auto reply) {
    // args = {"deviceId": 1, "action": "connect"}
    // 需要手动解析 JSON（使用 nlohmann/json 或手动解析）
    reply(R"({"ok": true, "message": "connected"})");
});
```

### 3.4 页面生命周期

Flutter Panel 的可见性由 C++ 侧的 tab 切换控制。当用户切到/切出 Flutter tab 时，C++ 会调用：

```dart
// 页面变为可见
→ channel 收到 "onPageState" + "active"

// 页面变为不可见
→ channel 收到 "onPageState" + "inactive"
```

**Flutter 侧应该做的：**

```dart
void _handlePageState(String state) {
  if (state == 'active') {
    // 恢复轮询、动画、定时器等
    _startPolling();
  } else if (state == 'inactive') {
    // 暂停非必要的后台操作
    _stopPolling();
  }
}
```

**重要**：页面不可见时 Flutter Engine 仍然在运行，渲染帧会继续产生。暂停不必要的操作可以减少 CPU 占用。

### 3.5 构建 App.framework

App.framework 是 Dart AOT 编译产物，包含 Dart 代码编译后的机器码。它必须放在 `.app/Contents/Frameworks/` 中才能被 Flutter Engine 加载。

#### 构建步骤

```bash
# 1. 在 Flutter 项目中
cd lava_app/lava_app/lava-orca

# Debug 模式构建（桌面不支持热重载）
flutter build macos --debug

# 2. 找到生成的 App.framework
find build -name "App.framework" -maxdepth 5 -type d
# 通常在: build/macos/Build/Products/Debug/App.framework

# 3. 复制到 Orca 项目
cp -R build/macos/Build/Products/Debug/App.framework \
     ../../../OrcaSlicer/resources/flutter_app/App.framework

# 4. 重新构建 Orca（仅 slicer 部分）
cd ../../../OrcaSlicer
./build_release_macos.sh -s
```

#### 可选：通过环境变量指定构建目录

```bash
export FLUTTER_APP_BUILD_DIR=/path/to/flutter/build/macos/Build/Products/Debug
./build_release_macos.sh
```

构建脚本会优先使用 `FLUTTER_APP_BUILD_DIR` 下的 `App.framework`，如果不存在则回退到 `resources/flutter_app/`。

#### Windows / Linux 对应物

- **Windows**：`flutter build windows` 生成的可执行文件和数据文件
- **Linux**：`flutter build linux` 生成的 `data/` 和 `lib/` 目录

对应的放置位置和加载方式在各平台的 build 脚本中处理。

### 3.6 调试技巧

#### 查看日志

C++ 侧使用 `BOOST_LOG_TRIVIAL` 输出日志。可以在 handler 中添加日志：

```cpp
d.on("getVersion", [](auto args, auto reply) {
    BOOST_LOG_TRIVIAL(info) << "[Flutter] getVersion called, args=" << args;
    reply("orca-1.0");
});
```

Dart 侧使用 `print()` 或日志框架：

```dart
print('[Flutter] Received onPageState: $state');
```

#### 排查 MethodChannel 不通

1. **确认 channel 名称完全一致**：C++ 和 Dart 的 channel 名称区分大小写
2. **确认方法名完全一致**：`invokeMethod` 的方法名和 Dispatcher 的 `.on("name", ...)` 中的名字必须匹配
3. **确认 C++ handler 已注册**：Dispatcher 是否调用了 `.on(method, ...)`

#### 排查黑屏问题（macOS）

1. 确认 `engine.viewController = controller` 已设置
2. 确认 `controller.view.wantsLayer = YES` 已设置
3. 确认 `App.framework` 在 `.app/Contents/Frameworks/` 中
4. 确认 `FlutterMacOS.framework` 在 `.app/Contents/Frameworks/` 中
5. 确认 rpath 设置：`otool -l Snapmaker_Orca | grep -A2 LC_RPATH`

#### 排查崩溃

常见崩溃原因：
- Dispatcher handler 中捕获了 `this`，但对象已被销毁
- 在非主线程调用 Flutter API
- `FlutterPanel` 在 `startView()` 之前就被使用

### 3.7 常见问题

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| 黑屏（macOS） | `engine.viewController` 未设置 | 设置 `engine.viewController = controller` |
| 黑屏（macOS） | `wantsLayer` 未设置 | 设置 `controller.view.wantsLayer = YES` |
| "MissingPluginException" | C++ 没注册该方法 | 确认 Dispatcher 中 `.on("methodName", ...)` |
| Channel 无响应 | 名称不匹配 | 确认 C++ 和 Dart channel 名称完全一致（区分大小写） |
| 页面显示空白 | entrypoint 不存在 | 检查 Dart 侧是否有 `@pragma('vm:entry-point')` 标注 |
| crash on MethodChannel | Dispatcher use-after-free | `handler()` 按值捕获 map，不要在 handler lambda 中捕获 `this` |
| dyld: Library not loaded | rpath 不正确 | macOS rpath: `@executable_path/../Frameworks` |
| 引擎初始化失败 | App.framework 缺失 | 确保 App.framework 在 app bundle 的 Frameworks 中 |
| 构建失败 | Flutter SDK 未安装 | 安装 Flutter SDK 或设置 `FLUTTER_SDK_PATH` cmake 参数 |

---

## 附录 A: Channel 命名规范

Channel 名称格式为 `"snapmaker/<模块>"`：

| Channel | 入口函数 | 用途 |
|---------|---------|------|
| `snapmaker/orca` | `main` | 主页面 |
| `snapmaker/home` | `homeMain` | 首页（当前测试入口） |
| `snapmaker/device` | `deviceMain` | 设备控制 |
| `snapmaker/print` | `printMain` | 打印控制 |

**一个 Engine 只能注册一次同名 channel**。如果多个页面共享 Engine，使用不同的 channel 名称。

---

## 附录 B: 参考链接

- [Flutter Desktop Embedding C API](https://github.com/flutter/flutter/tree/main/shell/platform)
- [Flutter macOS Embedding](https://github.com/flutter/flutter/tree/main/shell/platform/darwin/macos)
- [Flutter MethodChannel 文档](https://docs.flutter.dev/platform-integration/platform-channels)
- [wxWidgets Documentation](https://docs.wxwidgets.org/)
