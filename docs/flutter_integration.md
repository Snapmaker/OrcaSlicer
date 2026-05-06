# Flutter 桌面嵌入完整技术文档

## 目录

- [1. 整体架构](#1-整体架构)
  - [1.1 为什么需要两层设计](#11-为什么需要两层设计)
  - [1.2 类层次结构](#12-类层次结构)
  - [1.3 数据流全链路](#13-数据流全链路)
- [2. 核心类详解](#2-核心类详解)
  - [2.1 FlutterEngineHost（引擎宿主）](#21-flutterenginehost引擎宿主)
  - [2.2 FlutterViewHost（视图宿主）](#22-flutterviewhost视图宿主)
  - [2.3 FlutterPanel（wxWidgets 窗口）](#23-flutterpanelwxwidgets-窗口)
  - [2.4 Dispatcher（方法调度表）](#24-dispatcher方法调度表)
- [3. 各平台实现原理](#3-各平台实现原理)
  - [3.1 macOS](#31-macos)
  - [3.2 Windows](#32-windows)
  - [3.3 Linux](#33-linux)
- [4. MethodChannel 通信机制](#4-methodchannel-通信机制)
  - [4.1 通信链路](#41-通信链路)
  - [4.2 回调线程模型](#42-回调线程模型)
  - [4.3 JSON 数据格式](#43-json-数据格式)
- [5. 构建系统](#5-构建系统)
  - [5.1 deps 构建](#51-deps-构建)
  - [5.2 slicer 构建](#52-slicer-构建)
  - [5.3 打包](#53-打包)
- [6. C++ 开发指南](#6-c-开发指南)
  - [6.1 添加新页面](#61-添加新页面)
  - [6.2 页面间通信](#62-页面间通信)
  - [6.3 引擎与页面隔离](#63-引擎与页面隔离)
- [7. Flutter 开发指南](#7-flutter-开发指南)
  - [7.1 项目结构](#71-项目结构)
  - [7.2 入口函数](#72-入口函数)
  - [7.3 MethodChannel 通信](#73-methodchannel-通信)
  - [7.4 构建 App.framework](#74-构建-appframework)
  - [7.5 完整 Demo 源码](#75-完整-demo-源码)
- [8. CI 集成](#8-ci-集成)
- [9. 常见问题排查](#9-常见问题排查)

---

## 1. 整体架构

### 1.1 为什么需要两层设计

把 Flutter 嵌入桌面应用，本质上要解决两个独立的问题：

| 问题 | 解决者 | 生命周期 |
|------|--------|---------|
| Flutter Engine 怎么启动/销毁？Dart 代码在哪运行？ | **FlutterEngineHost** | 进程级，一个 |
| Flutter 渲染画面怎么嵌入到某个具体的窗口里？ | **FlutterViewHost** | 每个页面一个 |

分开的原因是：多个页面可以共用一套 Dart 运行时（同一个 App.framework），但每个页面的渲染目标是不同的窗口、走不同的入口函数、用不同的 MethodChannel。

### 1.2 类层次结构

```
┌─────────────────────────────────────────────────────────────────┐
│                          MainFrame                              │
│                                                                 │
│  成员变量:                                                       │
│    FlutterEngineHost* m_flutter_engine;    // 引擎，一个         │
│    FlutterPanel*      m_home_panel;        // 首页窗口           │
│    FlutterPanel*      m_device_panel;      // 设备页窗口         │
│                                                                 │
│  流程:                                                          │
│    构造时:                                                       │
│      m_flutter_engine = createFlutterEngine("", "").release()   │
│      m_flutter_engine->start()                                  │
│                                                                 │
│    创建页面:                                                     │
│      m_home_panel = new FlutterPanel(tab)                       │
│      m_home_panel->startView(m_flutter_engine,                   │
│                              "homeMain", "snapmaker/home")       │
│                                                                 │
│    销毁时:                                                       │
│      m_flutter_engine->stop()                                   │
│      delete m_flutter_engine                                    │
└─────────────────────────────────────────────────────────────────┘

FlutterEngineHost (抽象接口, flutter_host.h)
├── FlutterEngineHostMacOS  (flutter_host_macos.mm)
├── FlutterEngineHostWin    (flutter_host_win.cpp)
└── FlutterEngineHostLinux  (flutter_host_linux.cpp)

FlutterViewHost (抽象接口, flutter_host.h)
├── FlutterViewHostMacOS   (flutter_host_macos.mm)
├── FlutterViewHostWin     (flutter_host_win.cpp)
└── FlutterViewHostLinux   (flutter_host_linux.cpp)

FlutterPanel : wxWindow
└── 持有 std::unique_ptr<FlutterViewHost> m_view
```

### 1.3 数据流全链路

下面以 "Dart 调用 C++ 获取版本号" 为例，追踪完整调用链：

```
┌──────────────────────────────────────────────────────────────────────┐
│ Dart 代码 (运行在 Flutter Engine 内)                                  │
│                                                                      │
│   const channel = MethodChannel('snapmaker/home');                   │
│   final result = await channel.invokeMethod('getVersion', '');       │
│                                          │                           │
└──────────────────────────────────────────│───────────────────────────┘
                                           │ Flutter 内部机制
                                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│ FlutterViewHostMacOS::channel (FlutterMethodChannel, ObjC)            │
│                                                                      │
│   methodChannelWithName:@"snapmaker/home"                            │
│   → 收到 method="getVersion", arguments=""                           │
│   → 调用 setMethodCallHandler 注册的回调                              │
│                                          │                           │
└──────────────────────────────────────────│───────────────────────────┘
                                           │
                                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│ FlutterViewHostMacOS::setMethodCallHandler 注册的回调 (ObjC block)    │
│                                                                      │
│   handler(call.method, args, reply_function);                        │
│   // 即: handler("getVersion", "", [](result){...})                  │
│                                          │                           │
└──────────────────────────────────────────│───────────────────────────┘
                                           │
                                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Dispatcher::handler() 返回的 lambda (C++)                             │
│                                                                      │
│   在 unordered_map 中查找 "getVersion"                                │
│   → 找到了 → 调用对应的 Fn                                            │
│   → Fn: [](auto args, auto reply) { reply("orca-1.0"); }            │
│                                          │                           │
└──────────────────────────────────────────│───────────────────────────┘
                                           │ reply("orca-1.0")
                                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│ FlutterMethodChannel 的 result callback (ObjC)                       │
│                                                                      │
│   fResult(@"orca-1.0");  // 把结果传回 Dart                          │
│                                          │                           │
└──────────────────────────────────────────│───────────────────────────┘
                                           │
                                           ▼
                              Dart 收到返回值 "orca-1.0"
```

**C++ 主动调用 Dart 的方向相反但链路相同**：C++ 调用 `m_view->invokeMethod("onPageState", "active")` → 经过 FlutterMethodChannel → Dart 的 `setMethodCallHandler` 收到回调。

---

## 2. 核心类详解

### 2.1 FlutterEngineHost（引擎宿主）

**定义** (`flutter_host.h`):

```cpp
class FlutterEngineHost {
public:
    virtual ~FlutterEngineHost() = default;

    virtual bool start() = 0;    // 启动引擎
    virtual void stop() = 0;     // 销毁引擎，释放所有 View

    // 工厂方法：创建一个新页面
    virtual std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,   // Dart 入口函数名
        const std::string& channelName   // MethodChannel 名称
    ) = 0;
};
```

**工厂函数**（同一文件底部，各平台实现文件提供）:

```cpp
std::unique_ptr<FlutterEngineHost> createFlutterEngine(
    const std::string& assetsPath,  // 未使用（nil 让 Flutter 自动查找）
    const std::string& icuDataPath  // 未使用
);
```

**macOS 实现要点** (`flutter_host_macos.mm`):

```
start():
  → 创建 FlutterDartProject（加载 App.framework 中的 Dart AOT 产物）
  → 这个 project 是所有 engine 的模板

createView(entrypoint, channelName):
  → 用 project 创建 FlutterEngine（设置名称 "embed"）
  → 如果 entrypoint 不是 "main"，转成 NSString
  → 调用 [engine runWithEntrypoint:nsEntry]
     → 内部启动 Dart isolate，执行对应的 @pragma('vm:entry-point') 函数
  → 创建 FlutterViewHostMacOS(engine, channelName)
     → 内部创建 FlutterViewController
     → engine.viewController = controller   <-- 关键：不设这个就黑屏
     → 注册 MethodChannel
  → 返回 unique_ptr<FlutterViewHost>

stop():
  → 对所有 engine 调用 [engine shutDownEngine]
  → 清空 engines 列表
```

**Windows 实现要点** (`flutter_host_win.cpp`):

```
start(): 空操作（Windows 引擎在 createView 时创建）

createView(entrypoint, channelName):
  → FlutterDesktopEngineCreate(&props)   // 创建引擎
  → FlutterDesktopEngineRun(engine, ep)  // 运行 engine
  → FlutterDesktopViewControllerCreate(800, 600, engine)  // 创建 ViewController
  → 创建 FlutterViewHostWin(controller, engine, channelName)
  → 返回 unique_ptr<FlutterViewHost>
```

**Linux 实现**：与 Windows 类似，使用 C API (`FlutterDesktopEngineCreate`, `FlutterDesktopViewCreate` 等)。

### 2.2 FlutterViewHost（视图宿主）

**定义** (`flutter_host.h`):

```cpp
class FlutterViewHost {
public:
    // Reply: C++ → Dart 的回调函数
    using Reply = std::function<void(const std::string& result)>;

    // MethodCallHandler: Dart → C++ 的回调
    using MethodCallHandler = std::function<void(
        const std::string& method,       // 方法名，如 "getVersion"
        const std::string& arguments,    // JSON 参数
        Reply reply                      // 回复回调
    )>;

    virtual ~FlutterViewHost() = default;

    // 将 Flutter 内容嵌入到父窗口中
    // parentView: 平台原生句柄（NSView*/HWND/GtkWidget*）
    virtual void embedInto(void* parentView) = 0;

    // C++ → Dart 调用方法
    virtual void invokeMethod(const std::string& method,
                              const std::string& arguments) = 0;

    // 设置 Dart → C++ 的回调（注册 MethodChannel handler）
    virtual void setMethodCallHandler(MethodCallHandler handler) = 0;
};
```

**macOS 内部结构**:

```
FlutterViewHostMacOS 持有:
  ├── FlutterViewController* controller  — 管理 Flutter 渲染的 NSViewController
  │     └── controller.view              — NSView，Flutter 的渲染目标
  └── FlutterMethodChannel* channel      — Dart ↔ ObjC 通信通道

embedInto(parentView):
  → [parentView addSubview:controller.view]
  → 设置 frame = parentView.bounds
  → 设置 autoresizingMask（随父窗口缩放）

invokeMethod(method, arguments):
  → [channel invokeMethod:nsMethod arguments:nsArgs]
  → Flutter 内部将方法调用派发给 Dart 侧

setMethodCallHandler(handler):
  → [channel setMethodCallHandler:^(call, result) {
        handler(call.method, args, reply);
    }]
```

**Windows 内部结构**:

```
FlutterViewHostWin 持有:
  ├── FlutterDesktopViewControllerRef m_controller  — ViewController
  ├── FlutterDesktopEngineRef m_engine              — Engine 引用
  ├── FlutterDesktopMessengerRef m_messenger        — 消息通道
  └── MethodCallHandler m_handler                   — C++ 业务回调

embedInto(parentView):
  → FlutterDesktopViewGetHWND(view)  → childHwnd
  → SetParent(childHwnd, parentHwnd)  // Win32 API：子窗口重父
  → SetWindowPos(...)                 // 调整大小

messageCallback (静态函数):
  → 收到 Dart 消息 → 调用 m_handler(method, args, reply)
  → reply 通过 FlutterDesktopMessengerSendResponse 回传
```

**Linux 内部结构**:

```
FlutterViewHostLinux 持有:
  ├── FlutterDesktopViewRef m_view           — Flutter View
  ├── FlutterDesktopEngineRef m_engine       — 引擎引用
  ├── FlutterDesktopMessengerRef m_messenger — 消息通道
  └── MethodCallHandler m_handler            — C++ 业务回调

embedInto(parentHandle):
  → parentHandle 是 wxWindow::GetHandle() → GtkWidget*
  → gtk_widget_get_window → GdkWindow
  → gdk_x11_window_get_xid → parentXid (X11 窗口 ID)
  → FlutterDesktopViewGetId → childXid
  → XReparentWindow(display, childXid, parentXid, 0, 0)
  → XResizeWindow + XMapWindow
```

### 2.3 FlutterPanel（wxWidgets 窗口）

**定义** (`FlutterPanel.hpp`):

```cpp
class FlutterPanel : public wxWindow {
public:
    FlutterPanel(wxWindow* parent);

    // 启动 Flutter 视图：创建 ViewHost 并嵌入到自己的窗口中
    bool startView(FlutterEngineHost* engine,
                   const std::string& entrypoint,
                   const std::string& channelName);

    FlutterViewHost* view() { return m_view.get(); }

    void setHandler(FlutterViewHost::MethodCallHandler handler);

protected:
    std::unique_ptr<FlutterViewHost> m_view;  // 独占所有权
};
```

**实现** (`FlutterPanel.cpp`):

```cpp
FlutterPanel::FlutterPanel(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxFULL_REPAINT_ON_RESIZE) {
    SetBackgroundColour(*wxBLACK);  // 黑色背景：Flutter 未渲染时能看到
}

bool FlutterPanel::startView(FlutterEngineHost* engine,
                              const std::string& entrypoint,
                              const std::string& channelName) {
    m_view = engine->createView(entrypoint, channelName);  // 1. 创建 View
    if (!m_view) return false;
    m_view->embedInto(GetHandle());  // 2. 嵌入到本窗口
    return true;
}

void FlutterPanel::setHandler(MethodCallHandler handler) {
    if (m_view) m_view->setMethodCallHandler(std::move(handler));
}
```

**关键点**：
- `GetHandle()` 在不同平台返回不同东西：macOS 返回 `NSView*`，Windows 返回 `HWND`，Linux 返回 `GtkWidget*`
- `m_view` 是 `unique_ptr`，Panel 销毁时自动释放 ViewHost
- `startView()` 必须在 Panel 已经被加入父窗口后调用（否则 embedInto 嵌入的父窗口不存在）

### 2.4 Dispatcher（方法调度表）

**定义** (`FlutterPanel.hpp`):

```cpp
class Dispatcher {
public:
    using Fn = std::function<void(
        const std::string& args,           // Dart 传来的参数
        FlutterViewHost::Reply reply       // 回复回调
    )>;

    // 链式注册，返回自身引用
    Dispatcher& on(const std::string& method, Fn fn) {
        m_map[method] = std::move(fn);
        return *this;
    }

    // 生成 MethodCallHandler，按值捕获 map
    FlutterViewHost::MethodCallHandler handler() {
        auto map = m_map;  // 按值拷贝！Dispatcher 销毁后仍有效
        return [map](const std::string& method,
                     const std::string& args,
                     FlutterViewHost::Reply reply) {
            auto it = map.find(method);
            if (it != map.end())
                it->second(args, reply);
            else
                reply("");  // 未注册的方法返回空字符串
        };
    }

private:
    std::unordered_map<std::string, Fn> m_map;
};
```

**为什么按值捕获 map？**

```cpp
// ❌ 错误: Dispatcher 在栈上，handler 注册后被销毁
Dispatcher d;
d.on("getVersion", ...);
panel->setHandler(d.handler());
// d 析构 → 如果 handler 持有了 d 的引用 → 悬空指针 → crash

// ✅ 正确: handler() 内部 auto map = m_map; 按值拷贝
// lambda 独立拥有 map 的副本，d 析构不影响
```

---

## 3. 各平台实现原理

### 3.1 macOS

**依赖**: `FlutterMacOS.framework`（从 Flutter SDK 获取）

**嵌入流程**:

```
wxWindow::GetHandle() 返回 NSView* (wxWidgets 内部的 wxNSView)
  ↓
FlutterViewHostMacOS::embedInto(NSView* parentView)
  ↓
[parentView addSubview: flutterView]
  ↓
flutterView.frame = parentView.bounds
flutterView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable
```

**FlutterViewController 的创建与 engine 关联**:

```objc
// FlutterViewHostMacOS 构造函数中
controller = [[FlutterViewController alloc] initWithEngine:engine
                                                     nibName:nil bundle:nil];
controller.mouseTrackingMode = kFlutterMouseTrackingModeInActiveApp;
controller.view.wantsLayer = YES;
engine.viewController = controller;  // ← 最关键的一行

// 不设 engine.viewController = controller → engine 不知道渲染目标 → 黑屏
```

**MethodChannel 注册**:

```objc
channel = [FlutterMethodChannel
    methodChannelWithName:@"snapmaker/home"
         binaryMessenger:engine.binaryMessenger];

[channel setMethodCallHandler:^(FlutterMethodCall* call, FlutterResult fResult) {
    // call.method   → "getVersion"
    // call.arguments → "" (字符串或 nil)
    // fResult       → block，调用它把结果返回 Dart
    handler(call.method, args, reply);
}];
```

**引擎管理**:

```
FlutterDartProject (一个，所有 Engine 共享)
  → 加载 App.framework 中的 Dart AOT 产物

FlutterEngine (每个 View 一个)
  → initWithName:@"embed" project:project allowHeadlessExecution:YES
  → runWithEntrypoint: 启动对应的 Dart entrypoint 函数

多个 FlutterEngine 之间通过 project 共享 AOT 代码
但各自有独立的 Dart isolate → 变量/状态隔离
```

### 3.2 Windows

**依赖**: `flutter_windows.h` + `flutter_windows.dll/.lib` + `flutter_engine.dll`

**嵌入流程**:

```
wxWindow::GetHandle() 返回 HWND
  ↓
FlutterViewHostWin::embedInto(HWND parentHwnd)
  ↓
FlutterDesktopViewGetHWND(view) → childHwnd
  ↓
SetParent(childHwnd, parentHwnd)
  ↓
GetClientRect(parentHwnd, &rect)
SetWindowPos(childHwnd, NULL, 0, 0,
             rect.right, rect.bottom,
             SWP_NOZORDER | SWP_SHOWWINDOW)
```

**引擎创建**:

```cpp
FlutterDesktopEngineProperties props = {};
props.assets_path = nullptr;   // 让 Flutter 自动查找 AOT 数据
props.icu_data_path = nullptr;

FlutterDesktopEngineRef engine = FlutterDesktopEngineCreate(&props);
FlutterDesktopEngineRun(engine, entrypoint);

// ViewController 包装 engine 和初始尺寸
FlutterDesktopViewControllerRef controller =
    FlutterDesktopViewControllerCreate(800, 600, engine);
```

**消息回调**（静态函数 + userData）:

```cpp
static void messageCallback(const FlutterDesktopMessage* msg, void* userData) {
    auto* self = static_cast<FlutterViewHostWin*>(userData);
    // msg->channel   → "snapmaker/home"
    // msg->message   → 二进制数据
    // msg->message_size

    self->m_handler(method, args, reply);

    // reply 通过 FlutterDesktopMessengerSendResponse 回传
}
```

### 3.3 Linux

**依赖**: `flutter_linux.h` + `libflutter_linux_gtk.so` + `libflutter_engine.so` + X11

**嵌入流程**:

```
wxWindow::GetHandle() 返回 GtkWidget*
  ↓
FlutterViewHostLinux::embedInto(GtkWidget* parentWidget)
  ↓
gtk_widget_get_window(parentWidget) → GdkWindow*
  ↓
gdk_x11_window_get_xid(parentGdkWindow) → Window (X11 ID)
gdk_x11_display_get_xdisplay(...) → Display*
  ↓
FlutterDesktopViewGetId(m_view) → childXid
  ↓
XReparentWindow(display, childXid, parentXid, 0, 0)
XResizeWindow(display, childXid, w, h)
XMapWindow(display, childXid)
XFlush(display)
```

**为什么需要穿透 GDK → X11**：
- wxWidgets on Linux 底层使用 GTK2/GTK3
- GetHandle() 返回 GtkWidget*
- Flutter Linux embedding 使用 X11 窗口
- 必须通过 GDK API 拿到底层 X11 Window ID 才能 reparent

---

## 4. MethodChannel 通信机制

### 4.1 通信链路

```
Dart 端                               C++ 端
══════════                            ══════════

// ── Dart → C++ ──

channel.invokeMethod(       ───→   FlutterMethodChannel 收到
  'getVersion', '{}')             → handler("getVersion", "{}", reply)
                                  → Dispatcher 查找 "getVersion"
                                  → Fn(args, reply) 执行
                                  → reply("orca-1.0")
                                  → FlutterMethodChannel 传回结果
result = "orca-1.0"         ←───

// ── C++ → Dart ──

channel.setMethodCallHandler(  ←─── m_view->invokeMethod(
  (call) {                           "onPageState", "active")
    if (call.method == 'onPageState')
      handleState(call.arguments);
  }
)
```

### 4.2 回调线程模型

**所有 Flutter MethodChannel 回调都在主线程（UI 线程）执行**。

```cpp
// 后台线程需要操作 Flutter 时，必须回到主线程
std::thread([this] {
    // 后台工作...
    std::string result = doHeavyWork();

    // 回到主线程更新 Flutter
    CallAfter([this, result] {
        if (m_panel && m_panel->view())
            m_panel->view()->invokeMethod("onDataReady", result);
    });
}).detach();
```

`CallAfter` 是 wxWidgets 提供的方法，等价于 `wxTheApp->CallAfter([](){...})`，确保 lambda 在主线程的事件循环中执行。

### 4.3 JSON 数据格式

C++ 和 Dart 之间传递的是**字符串**，通常约定使用 JSON：

```dart
// Dart → C++
final args = jsonEncode({'deviceId': 1, 'action': 'connect'});
await channel.invokeMethod('connectDevice', args);
```

```cpp
// C++ 侧收到原始 JSON 字符串
d.on("connectDevice", [](auto args, auto reply) {
    // args = "{\"deviceId\": 1, \"action\": \"connect\"}"
    // 需要手动解析（用 nlohmann/json 或手动解析）
    reply(R"({"ok": true})");
});
```

**C++ 侧不做自动 JSON 解析**，字符串原封不动传递。如果需要结构化数据，C++ 开发者自行解析。

---

## 5. 构建系统

### 5.1 deps 构建（`deps/Flutter/Flutter.cmake`）

`Flutter.cmake` 负责把 Flutter 引擎产物从 Flutter SDK 拷贝到 deps 安装目录。

**Flutter SDK 自动检测**（优先级从高到低）:

1. CMake 参数 `-DFLUTTER_SDK_PATH=/path/to/flutter`
2. 环境变量 `FLUTTER_HOME`（CI 中 `subosito/flutter-action@v2` 自动设置）
3. macOS Homebrew: `/opt/homebrew/Caskroom/flutter/*/flutter`
4. `$PATH` 中的 `flutter` 命令 → 向上两级目录

**产物位置**:

| 平台 | SDK 中位置 | 拷贝目标（DESTDIR） |
|------|-----------|-------------------|
| macOS | `$SDK/bin/cache/artifacts/engine/darwin-x64/FlutterMacOS.xcframework/macos-arm64_x86_64/FlutterMacOS.framework` | `${DESTDIR}/FlutterMacOS.framework` |
| Windows | `$SDK/bin/cache/artifacts/engine/windows-x64/client_wrapper/` + `windows-x64/` | `${DESTDIR}/include/flutter/`, `${DESTDIR}/lib/`, `${DESTDIR}/bin/` |
| Linux | `$SDK/bin/cache/artifacts/engine/linux-x64/flutter_linux/` + `linux-x64/` | `${DESTDIR}/include/flutter/`, `${DESTDIR}/lib/`, `${DESTDIR}/bin/` |

如果 SDK 或产物不存在 → `message(FATAL_ERROR "Run 'flutter precache --platform' first.")`

### 5.2 slicer 构建

**`src/slic3r/CMakeLists.txt`**:

```cmake
include(EnsureFlutterDeps)
# ...
if (WIN32)
    list(APPEND SLIC3R_GUI_SOURCES GUI/Flutter/flutter_host_win.cpp)
    # ...
    ensure_flutter_deps()
    target_include_directories(libslic3r_gui PRIVATE "${CMAKE_PREFIX_PATH}/include/flutter")
    target_link_libraries(libslic3r_gui "${CMAKE_PREFIX_PATH}/lib/flutter_windows.lib")
endif()
```

**`cmake/modules/EnsureFlutterDeps.cmake`** — CMake configure 时的兜底:

```
ensure_flutter_deps() 逻辑:
  1. Flutter 产物在 CMAKE_PREFIX_PATH 里？ → 什么都不做
  2. 不在，但 Flutter SDK 可用？ → 从 SDK 拷贝产物到 CMAKE_PREFIX_PATH
  3. 都不行？ → FATAL_ERROR，说明缺什么
```

有了这个模块，即使 deps 缓存里没有 Flutter（比如旧 CI 缓存），只要机器上有 Flutter SDK，configure 时就能自动补齐。

**`src/CMakeLists.txt`** — 主程序链接:

```cmake
include(EnsureFlutterDeps)
# ...
if (APPLE)
    ensure_flutter_deps()
    target_link_libraries(Snapmaker_Orca "-F${CMAKE_PREFIX_PATH}" "-framework FlutterMacOS")
    target_link_options(Snapmaker_Orca PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
```

`rpath` 设为 `@executable_path/../Frameworks` 是为了在 `.app` bundle 中正确找到 framework：

```
Snapmaker Orca.app/
  Contents/
    MacOS/
      snapmaker-orca          ← 可执行文件
    Frameworks/
      FlutterMacOS.framework  ← 引擎 framework（@executable_path/../Frameworks 指向这里）
      App.framework           ← Dart AOT 产物
```

### 5.3 打包（macOS `build_release_macos.sh`）

构建脚本的最后阶段：

```bash
# 从 deps 目录拷贝 FlutterMacOS.framework 到 app bundle
cp -Rf "${DEPS}/usr/local/FlutterMacOS.framework" \
       "${APP_FRAMEWORKS_DIR}/FlutterMacOS.framework"

# 从项目资源或 FLUTTER_APP_BUILD_DIR 拷贝 App.framework
if [ -n "${FLUTTER_APP_BUILD_DIR}" ]; then
    cp -Rf "${FLUTTER_APP_BUILD_DIR}/App.framework" "${APP_FRAMEWORKS_DIR}/"
elif [ -d "${PROJECT_DIR}/resources/flutter_app/App.framework" ]; then
    cp -Rf "${PROJECT_DIR}/resources/flutter_app/App.framework" "${APP_FRAMEWORKS_DIR}/"
fi
```

---

## 6. C++ 开发指南

### 6.1 添加新页面（完整步骤）

**Step 1: 声明 tab 位置** (`MainFrame.hpp`)

```cpp
enum TabPosition {
    tpHome        = 0,
    tp3DEditor    = 1,
    // ...
    tpFlutterTest = 9,
    tpDevice      = 10,  // 新增
};

// 成员变量
FlutterPanel* m_device_panel{nullptr};
```

**Step 2: 创建 Panel 和 View** (MainFrame 构造函数中，放在 `CallAfter` 里)

```cpp
// 创建 wxWidgets 窗口
m_device_panel = new FlutterPanel(m_tabpanel);
m_tabpanel->AddPage(m_device_panel, _L("Device"), "...icon...", "...icon...", false);

// FlutterPanel 被显示后才能真正嵌入（wxWidgets 延迟创建原生窗口）
CallAfter([this] {
    // startView 内部: engine->createView() → view->embedInto(panel->GetHandle())
    if (!m_device_panel->startView(m_flutter_engine, "deviceMain", "snapmaker/device")) {
        BOOST_LOG_TRIVIAL(error) << "[Flutter] Device panel startView failed";
        return;
    }

    // 注册 C++ → Dart 的回调方法
    Dispatcher d;
    d.on("getDeviceList", [](auto args, auto reply) {
        std::string devices = R"([{"id":1,"name":"Printer A"}])";
        reply(devices);
    })
    .on("connectDevice", [](auto args, auto reply) {
        // args: {"deviceId": 1}
        // 执行连接逻辑...
        reply(R"({"ok": true})");
    });
    m_device_panel->setHandler(d.handler());
});
```

**Step 3: 处理 Tab 切换** (PAGE_CHANGED 事件)

```cpp
void MainFrame::onTabChanged(wxNotebookEvent& event) {
    int sel = event.GetSelection();
    static int prev_monitored_tab = -1;

    // 离开设备页
    if (prev_monitored_tab == tpDevice && sel != tpDevice) {
        if (m_device_panel && m_device_panel->view())
            m_device_panel->view()->invokeMethod("onPageState", "inactive");
    }

    // 进入设备页
    if (sel == tpDevice) {
        if (m_device_panel && m_device_panel->view())
            m_device_panel->view()->invokeMethod("onPageState", "active");
        prev_monitored_tab = tpDevice;
    }
}
```

**Step 4: 清理**（MainFrame::shutdown()）

engine 的 `stop()` 会关闭 engine 并隐式销毁所有关联的 View。Panel 由 wxWidgets 自动销毁，`unique_ptr<FlutterViewHost>` 自动释放。不需要额外操作。

### 6.2 页面间通信

两个 Flutter 页面之间不直接通信（各自有独立的 Dart isolate）。通信必须经过 C++ 中转。

```cpp
// 首页点击了某个设备 → 切换到设备页并传入设备 ID
d.on("openDevice", [this](auto args, auto reply) {
    // args: {"deviceId": 5}
    m_tabpanel->SetSelection(MainFrame::tpDevice);
    m_device_panel->view()->invokeMethod("onDeviceSelected", args);
    reply(R"({"ok": true})");
});
```

**注意**：必须在 `view()` 存在时才能调用 `invokeMethod`。如果设备页还没创建，需要先创建再通知。

### 6.3 引擎与页面隔离

**一个 Engine 可以创建多个 View（页面），但每个 View 有独立的 Dart isolate，变量不共享。**

```
m_flutter_engine
  ├── createView("homeMain", "snapmaker/home")   → Dart 运行 homeMain()
  ├── createView("deviceMain", "snapmaker/device") → Dart 运行 deviceMain()
  └── createView("printMain", "snapmaker/print")   → Dart 运行 printMain()
```

**每个 View 必须使用不同的 channel 名称**。如果在同一个 engine 上用相同的 channel 名注册两次，第二次会覆盖第一次。

| entrypoint | channel | Dart 函数 |
|-----------|---------|----------|
| `"homeMain"` | `"snapmaker/home"` | `homeMain()` |
| `"deviceMain"` | `"snapmaker/device"` | `deviceMain()` |
| `"main"` 或空 | `"snapmaker/orca"` | `main()` |

---

## 7. Flutter 开发指南

### 7.1 项目结构

```
lava_app/lava_app/lava-orca/lib/
  main.dart                  # 主入口
  features/
    device_control/          # 设备控制模块
    device_connections/
    wcp/
  src/
    bridge/                  # C++ ↔ Dart 桥接层
      orca_bridge.dart       # MethodChannel 封装
    viewmodels/              # 状态管理
    pages/
    config/
```

### 7.2 入口函数

桌面嵌入**不调用**默认的 `main()`，而是调用 C++ 指定的 entrypoint：

```cpp
m_panel->startView(engine, "deviceMain", "snapmaker/device");
//                          ↑ entrypoint     ↑ channel
```

非 `main` 的入口**必须**加注解（否则 Dart AOT 编译会把它 tree-shake 掉）：

```dart
// ✅ 正确：带注解
@pragma('vm:entry-point')
void deviceMain() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const DeviceApp());
}

// ❌ 错误：没有注解，AOT 编译后找不到
void deviceMain() {
  runApp(const DeviceApp());
}
```

### 7.3 MethodChannel 通信

**Dart → C++**:

```dart
import 'package:flutter/services.dart';

class DeviceBridge {
  static const _channel = MethodChannel('snapmaker/device');

  // 调用 C++ 方法，返回 Future
  static Future<List<Device>> getDeviceList() async {
    final result = await _channel.invokeMethod('getDeviceList', '');
    final List<dynamic> list = jsonDecode(result);
    return list.map((d) => Device.fromJson(d)).toList();
  }

  static Future<bool> connectDevice(int deviceId) async {
    final args = jsonEncode({'deviceId': deviceId});
    final result = await _channel.invokeMethod('connectDevice', args);
    return jsonDecode(result)['ok'] == true;
  }
}
```

**C++ → Dart（接收 C++ 的主动调用）**:

```dart
class DeviceBridge {
  static void setupHandlers() {
    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'onPageState':
          final state = call.arguments as String;
          // state: "active" | "inactive"
          if (state == 'active') _startPolling();
          if (state == 'inactive') _stopPolling();
          break;
        case 'onDeviceSelected':
          final data = jsonDecode(call.arguments);
          _navigateToDevice(data['deviceId']);
          break;
        default:
          throw MissingPluginException();
      }
    });
  }
}
```

**setupHandlers 必须在 C++ 注册 handler 之前调用吗？** 不需要。MethodChannel 的 handler 是持久的——一旦注册，后续的所有调用都会触发。Dart 侧在 `initState` 或 `main()` 中尽早调用 `setupHandlers()` 即可。

### 7.4 构建 App.framework

```bash
# 1. 构建 Flutter 项目
cd lava_app/lava_app/lava-orca
flutter build macos --debug

# 2. 找到产物
find build -name "App.framework" -maxdepth 5 -type d
# → build/macos/Build/Products/Debug/App.framework

# 3. 拷贝到 Orca 资源目录
cp -R build/macos/Build/Products/Debug/App.framework \
     ../../../OrcaSlicer/resources/flutter_app/

# 4. 重新编译 Orca
cd ../../../OrcaSlicer
./build_release_macos.sh -s
```

或者设置环境变量指定构建目录：

```bash
export FLUTTER_APP_BUILD_DIR=/path/to/build/macos/Build/Products/Debug
./build_release_macos.sh
```

### 7.5 完整 Demo 源码

下面是 `flutter_wx_demo/flutter_app/lib/main.dart` 的完整源码，C++ 侧就是和这套代码配合验证通过的：

```dart
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

// ===================================================================
// Entrypoints: C++ creates each view with a specific entrypoint name.
//   homeMain    → HomePanelApp  (snapmaker/home)
//   prepareMain → PreparePanelApp (snapmaker/prepare)
// ===================================================================

@pragma('vm:entry-point')
void main() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void homeMain() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void prepareMain() => runApp(const PreparePanelApp());

// ============================ Shared Panel Widget ===========================

class PanelApp extends StatelessWidget {
  final String title;
  final Color color;
  final String channelName;
  const PanelApp(this.title, this.color, this.channelName, {super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData(colorSchemeSeed: color, useMaterial3: true),
      home: PanelPage(title, color, channelName),
    );
  }
}

class PanelPage extends StatefulWidget {
  final String title, channelName;
  final Color color;
  const PanelPage(this.title, this.color, this.channelName, {super.key});

  @override
  State<PanelPage> createState() => _PanelPageState();
}

class _PanelPageState extends State<PanelPage> {
  late final MethodChannel _channel;
  int _counter = 0;
  final List<String> _messages = [];
  final TextEditingController _sendCtrl = TextEditingController();
  String _cxxVersion = '';

  @override
  void initState() {
    super.initState();
    _channel = MethodChannel(widget.channelName);
    _channel.setMethodCallHandler(_onMethodCall);
    _requestVersion();
  }

  Future<void> _requestVersion() async {
    try {
      final v = await _channel.invokeMethod<String>('getVersion');
      if (v != null && mounted) setState(() => _cxxVersion = v);
    } catch (_) {
      if (mounted) setState(() => _cxxVersion = '(not connected)');
    }
  }

  Future<dynamic> _onMethodCall(MethodCall call) async {
    switch (call.method) {
      case 'onMessage':
        final msg = call.arguments as String;
        if (mounted) {
          setState(() {
            _messages.insert(0, '[C++] $msg');
            if (_messages.length > 50) _messages.removeLast();
          });
        }
        return null;
      case 'onCounterUpdate':
        if (mounted) setState(() => _counter = call.arguments as int);
        return null;
      default:
        throw MissingPluginException();
    }
  }

  void _askCpp() async {
    try {
      final val =
          await _channel.invokeMethod<int>('incrementCounter', _counter);
      if (mounted && val != null) setState(() => _counter = val);
    } catch (_) {}
  }

  void _sendToCpp() async {
    final text = _sendCtrl.text.trim();
    if (text.isEmpty) return;
    try {
      final reply =
          await _channel.invokeMethod<String>('sendMessage', text);
      if (mounted && reply != null) {
        setState(() {
          _messages.insert(0, '[C++ reply] $reply');
          if (_messages.length > 50) _messages.removeLast();
        });
      }
    } catch (e) {
      if (mounted) {
        setState(() {
          _messages.insert(
              0, '[err] ${e.toString().substring(0, 60)}');
        });
      }
    }
    _sendCtrl.clear();
  }

  @override
  Widget build(BuildContext context) {
    final isDark = Theme.of(context).brightness == Brightness.dark;
    return Scaffold(
      backgroundColor:
          isDark ? const Color(0xFF1A1A2E) : const Color(0xFFF0F0F5),
      body: Column(
        children: [
          // Header
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(12),
            color: widget.color.withValues(alpha: 0.15),
            child: Row(
              children: [
                Icon(Icons.circle, size: 10, color: widget.color),
                const SizedBox(width: 8),
                Text(widget.title,
                    style: TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: widget.color)),
                const Spacer(),
                if (_cxxVersion.isNotEmpty)
                  Chip(
                      label: Text('C++ $_cxxVersion',
                          style: const TextStyle(fontSize: 10))),
              ],
            ),
          ),
          // Counter
          Padding(
            padding: const EdgeInsets.all(12),
            child: Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  IconButton.filled(
                      onPressed: () => setState(() => _counter--),
                      icon: const Icon(Icons.remove)),
                  Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    child: Text('$_counter',
                        style: const TextStyle(
                            fontSize: 32, fontWeight: FontWeight.bold)),
                  ),
                  IconButton.filled(
                      onPressed: () => setState(() => _counter++),
                      icon: const Icon(Icons.add)),
                  const SizedBox(width: 12),
                  FilledButton.tonal(
                      onPressed: _askCpp,
                      child: const Text('Ask C++')),
                ]),
          ),
          const Divider(height: 1),
          // Messages
          Expanded(
            child: _messages.isEmpty
                ? const Center(
                    child: Text('No messages yet',
                        style:
                            TextStyle(color: Colors.grey, fontSize: 12)))
                : ListView.builder(
                    itemCount: _messages.length,
                    itemBuilder: (_, i) => ListTile(
                      dense: true,
                      leading: Icon(Icons.message,
                          size: 14, color: widget.color),
                      title: Text(_messages[i],
                          style: const TextStyle(fontSize: 11)),
                    ),
                  ),
          ),
          const Divider(height: 1),
          // Input
          Padding(
            padding: const EdgeInsets.all(8),
            child: Row(children: [
              Expanded(
                child: TextField(
                  controller: _sendCtrl,
                  style: const TextStyle(fontSize: 13),
                  decoration: const InputDecoration(
                    hintText: 'Send to C++...',
                    border: OutlineInputBorder(),
                    isDense: true,
                    contentPadding: EdgeInsets.symmetric(
                        horizontal: 10, vertical: 8),
                  ),
                  onSubmitted: (_) => _sendToCpp(),
                ),
              ),
              const SizedBox(width: 6),
              FilledButton(
                  onPressed: _sendToCpp, child: const Text('Send')),
            ]),
          ),
        ],
      ),
    );
  }
}

// ============================ Entrypoint Wrappers ===========================

class HomePanelApp extends StatelessWidget {
  const HomePanelApp({super.key});
  @override
  Widget build(BuildContext context) =>
      const PanelApp('Home Panel', Colors.blue, 'snapmaker/home');
}

class PreparePanelApp extends StatelessWidget {
  const PreparePanelApp({super.key});
  @override
  Widget build(BuildContext context) =>
      const PanelApp('Prepare Panel', Colors.teal, 'snapmaker/prepare');
}
```

**关键点**：

1. 每个入口函数都用 `@pragma('vm:entry-point')` 注解，防止 Dart AOT tree-shake
2. C++ 通过 `startView(engine, "homeMain", "snapmaker/home")` 指定入口函数名和 channel 名
3. `PanelApp` 是复用的 UI 组件，不同入口传不同颜色和 channel 名即可区分页面

---

## 8. CI 集成

### 缓存流程

```
build_check_cache.yml:
  → 计算 cache key = hashFiles('deps/**')
  → 查缓存 → valid-cache = true/false

build_deps.yml:
  → if valid-cache == false:
      → 安装 Flutter SDK（subosito/flutter-action@v2）
      → flutter precache --platform
      → 构建 deps（Flutter.cmake 拷贝引擎产物）
      → 保存缓存
  → if valid-cache == true:
      → 跳过 deps 构建

build_orca.yml:
  → 恢复 deps 缓存
  → 构建 slicer
      → CMake configure: ensure_flutter_deps()
          → deps 里有 Flutter？直接用
          → deps 里没有，但 Flutter SDK 在？自动拷贝
          → 都没有？FATAL_ERROR
```

`EnsureFlutterDeps.cmake` 模块确保即使 deps 缓存来自添加 Flutter 之前的旧构建，只要 CI 安装了 Flutter SDK（通过 `flutter-action`），CMake configure 阶段会自动补齐缺失的 Flutter 产物。

### Flutter SDK 安装（三平台统一）

```yaml
- uses: subosito/flutter-action@v2
  with:
    channel: 'stable'
    # 自动设置 FLUTTER_HOME 环境变量

- run: flutter precache --macos   # macOS
- run: flutter precache --windows # Windows
- run: flutter precache --linux   # Linux
```

---

## 9. 常见问题排查

| 现象 | 原因 | 排查/解决 |
|------|------|----------|
| Flutter 区域显示黑色 | `engine.viewController = controller` 未设置 | 检查 `flutter_host_macos.mm` 构造函数是否有这一行 |
| Flutter 区域显示黑色 | `controller.view.wantsLayer = YES` 未设置 | 同上 |
| Flutter 区域显示黑色 | `App.framework` 不在 bundle 中 | 检查 `.app/Contents/Frameworks/` 是否有 `App.framework` |
| "MissingPluginException" | C++ Dispatcher 没注册该方法 | 检查 `d.on("方法名", ...)` 是否存在 |
| Channel 无反应 | C++ 和 Dart 的 channel 名称不一致 | 两边都打印 channel 名称对比 |
| 页面空白 | entrypoint 函数不存在或没加注解 | Dart 侧检查 `@pragma('vm:entry-point')` |
| 崩溃在 MethodChannel 回调 | Dispatcher use-after-free | 确认 `handler()` 按值捕获 map，lambda 中没有裸 this |
| dyld: Library not loaded | rpath 错误 | `otool -l snapmaker-orca \| grep -A2 LC_RPATH` |
| Windows: 找不到 flutter_windows.h | deps 中没有 Flutter 产物 | 安装 Flutter SDK + `flutter precache --windows` + 重编 deps |
| Linux: 找不到 flutter_linux.h | deps 中没有 Flutter 产物 | 安装 Flutter SDK + `flutter precache --linux` + 重编 deps |
| 编译时 FATAL_ERROR: Flutter SDK not found | 机器上没有 Flutter SDK | 安装 Flutter SDK 或设置 `FLUTTER_HOME` 环境变量 |
| 多个 Flutter 页面同时显示 | 正常，每个页面独立 engine isolate | Tab 切换时注意调用 invokeMethod 通知页面状态 |
