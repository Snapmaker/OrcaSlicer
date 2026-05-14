# OrcaSlicer Flutter 嵌入技术文档

> 面向 C++ / Flutter 开发者，介绍 Flutter Engine 嵌入 OrcaSlicer（基于 C++ / wxWidgets / CMake 的 3D 打印切片软件）的架构、通信协议与构建系统。

---

## 第一章：概述与设计动机

### 1.1 为什么在 wxWidgets C++ 应用中嵌入 Flutter

OrcaSlicer 是一个大型、成熟的原生 C++ 3D 打印切片软件，GUI 层基于 wxWidgets。随着产品演进，越来越多的 UI 面板需要丰富的交互体验、流畅动画、跨平台一致的渲染以及快速的 UI 迭代周期——这些恰好是 Flutter 的优势所在。

将 Flutter 嵌入到 wxWidgets 应用意味着：
- **Dart UI 运行在原生 wxWindow 面板内部**——Flutter 渲染的内容作为 wx 控件的一个子窗口存在，与现有 wxWidgets 布局系统无缝共存。
- **渐进式迁移**——可以逐个面板地替换现有 wx 实现，无需一次性重写整个 GUI。
- **跨平台统一**——同一套 Dart 代码在 Windows、macOS、Linux 上产生一致的 UI 表现。
- **保留原生能力**——C++ 层仍然可以直接访问文件系统、网络、硬件和外设。

### 1.2 双层抽象架构

Flutter 嵌入采用**两层抽象**设计：

| 层级 | 类 | 作用域 | 职责 |
|------|-----|--------|------|
| 引擎层 | `FlutterEngineHost` | 进程级单例 | 管理 Flutter Engine 的生命周期（启动、停止、创建视图） |
| 视图层 | `FlutterViewHost` | 面板级（可多实例） | 管理单个 Flutter 视图的嵌入、大小调整、MethodChannel 通信、焦点控制 |

这种分层的核心理由：
- Flutter Engine 是重量级资源，每个进程只需要一个。
- 但一个应用可能有多个面板（例如 notebook 的不同 tab），每个都需要独立的 Dart UI 实例——它们共享引擎，但拥有独立的 ViewController、MethodChannel 和焦点上下文。

### 1.3 高层架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                           wxApp                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                       wxNotebook                              │  │
│  │  ┌─────────────────┐  ┌─────────────────┐                    │  │
│  │  │  FlutterPanel   │  │  FlutterPanel   │   ...更多tab       │  │
│  │  │  (wxWindow子类)  │  │  (wxWindow子类)  │                    │  │
│  │  │  ┌────────────┐ │  │  ┌────────────┐ │                    │  │
│  │  │  │FlutterView │ │  │  │FlutterView │ │                    │  │
│  │  │  │  Host      │ │  │  │  Host      │ │                    │  │
│  │  │  │ (HWND/     │ │  │  │ (HWND/     │ │                    │  │
│  │  │  │  NSView/   │ │  │  │  NSView/   │ │                    │  │
│  │  │  │  GtkWidget) │ │  │  │  GtkWidget) │ │                    │  │
│  │  │  └─────┬──────┘ │  │  └─────┬──────┘ │                    │  │
│  │  └────────┼────────┘  └────────┼────────┘                    │  │
│  └───────────┼────────────────────┼──────────────────────────────┘  │
│              │                    │                                  │
│              ▼                    ▼                                  │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │             FlutterEngineHost (进程唯一)                        │  │
│  │  ┌─────────────────────────────────────────────────────────┐  │  │
│  │  │              Flutter Engine (C API / GObject)            │  │  │
│  │  │  ┌──────────┐  ┌──────────┐  ┌──────────┐               │  │  │
│  │  │  │  Dart VM │  │  Skia    │  │  Shell   │               │  │  │
│  │  │  │  Isolate │  │  Impeller│  │Platform  │               │  │  │
│  │  │  └──────────┘  └──────────┘  │  Thread  │               │  │  │
│  │  │                              └──────────┘               │  │  │
│  │  └─────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────┘  │
│              │                                                       │
│              ▼                                                       │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │               Dart 代码 (flutter_app/)                        │  │
│  │  main.dart  →  runApp(MyApp())                                │  │
│  │  MethodChannel('orca_channel') ↔ C++ Dispatcher               │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.4 平台支持

| 平台 | 原生窗口类型 | Flutter Engine API | 实现文件 |
|------|-------------|-------------------|---------|
| Windows | `HWND` (Win32) | Flutter Windows C API (`flutter_windows.h`) | `flutter_host_win.cpp` |
| macOS | `NSView` (Cocoa) | FlutterMacOS Framework (`FlutterMacOS.h`) | `flutter_host_macos.mm` |
| Linux | `GtkWidget` (GTK3) | Flutter Linux GObject API (`flutter_linux.h`) | `flutter_host_linux.cpp` |

### 1.5 关键设计决策

**决策 1：手动实现 StandardMethodCodec（Windows）**
Windows 平台不使用 Flutter 提供的 C++ wrapper 层的 `MethodChannel`，而是直接调用 C API 并手动编码/解码 StandardMethodCodec。这避免了对 `flutter/cpp_client_wrapper` 的依赖（该 wrapper 在 Flutter 3.38 中已被移除），并给予对类型检测的完全控制（参见 3.3 节）。

**决策 2：延迟嵌入**
`FlutterPanel` 不立即调用 `embedInto()`，而是等待面板获得有效的客户端尺寸（宽/高 > 1）后才执行一次性嵌入。这解决了 wxNotebook 非选中页面分配 0x0 或 1x1 尺寸的问题。

**决策 3：平台工厂函数忽略参数**
`createFlutterEngine(assetsPath, icuDataPath)` 的签名接受路径参数以实现跨平台统一，但各平台实现内部自行解析路径：
- Windows：通过 `GetModuleFileNameW` 定位可执行文件目录，追加相对路径。
- Linux：通过 `/proc/self/exe` 解析，硬编码 AOT 库名为 `libflutter_app.so`。
- macOS：由 Framework 捆绑的 `flutter_assets` 自动定位，不显式传递路径。

**决策 4：Mac 每个视图独立 Engine**
在 macOS 上，`createView()` 为每个视图创建独立的 `FlutterEngine` 实例（而非共享 Engine）。这使得多个面板可以运行不同 entrypoint，并且每个 engine 的 isolate 完全隔离。

---

## 第二章：整体架构

### 2.1 FlutterEngineHost（引擎层）

`FlutterEngineHost` 是进程级的抽象接口，负责 Flutter 引擎的全局生命周期。其接口定义在 `flutter_host.h` 中：

```cpp
class FlutterEngineHost {
public:
    virtual ~FlutterEngineHost() = default;

    virtual bool start() = 0;       // 初始化引擎环境
    virtual void stop() = 0;        // 清理引擎资源

    virtual std::unique_ptr<FlutterViewHost> createView(
        const std::string& entrypoint,   // Dart entrypoint 函数名
        const std::string& channelName) = 0;  // MethodChannel 名称
};
```

工厂函数 `createFlutterEngine()` 创建平台特定的实现：

| 平台 | 实现类 | 主要行为 |
|------|--------|---------|
| Windows | `FlutterEngineHostWin` | `start()`/`stop()` 仅设置标志位；真正的引擎创建在 `createView()` 中按需进行。 |
| macOS | `FlutterEngineHostMacOS` | `start()` 创建全局 `FlutterDartProject`；`createView()` 创建独立 `FlutterEngine` 实例并保留引用用于后续清理。`stop()` 依次调用 `[engine shutDownEngine]`。 |
| Linux | `FlutterEngineHostLinux` | `start()` 设置环境变量 `FLUTTER_LINUX_RENDERER=software` 强制软件渲染；`createView()` 通过 GObject API 创建 `FlView` + `FlEngine`。 |

#### Windows 引擎创建细节

`FlutterEngineHostWin::createView()` 执行以下步骤：
1. 通过 `GetModuleFileNameW` 获取当前可执行文件的绝对路径，提取所在目录。
2. 基于可执行文件目录拼接：
   - `data/flutter_assets/` — Flutter 资源包
   - `data/icudtl.dat` — ICU 国际化数据
   - `flutter_app.dll` — AOT 编译的 Dart 代码
3. 填充 `FlutterDesktopEngineProperties` 结构体，调用 `FlutterDesktopEngineCreate`。
4. 调用 `FlutterDesktopEngineRun` 启动 Dart VM isolate。
5. 创建 `FlutterDesktopViewController`（初始尺寸 800x600），包装为 `FlutterViewHostWin` 返回。

**路径解析的重要性**：使用可执行文件路径而非 `CWD`，因为应用程序可能从任意工作目录启动（例如通过文件关联打开文件时）。

#### macOS 引擎创建细节

`FlutterEngineHostMacOS::start()` 创建 `FlutterDartProject`（不指定预编译 Dart bundle，由 Framework 自动搜索 `flutter_assets`）。`createView()` 为每个视图创建独立的 `FlutterEngine` 实例：
- 引擎名称固定为 `"embed"`（仅用于日志标识）。
- 允许 headless 执行（`allowHeadlessExecution:YES`）。
- `entrypoint` 通过 `[engine runWithEntrypoint:]` 传递给 Dart 端（空字符串或 `"main"` 等价于默认 main 函数）。
- 所有 engine 引用存入 `NSMutableArray` 用于析构时清理。

#### Linux 引擎创建细节

`FlutterEngineHostLinux::start()` 设置 `FLUTTER_LINUX_RENDERER=software`，因为在嵌入的 wxWidgets 上下文中硬件加速渲染可能不稳定。`createView()` 通过 GObject API：
1. 创建 `FlDartProject`，覆盖 AOT 库路径为 `$EXE_DIR/lib/libflutter_app.so`（而非 Flutter 默认的 `libapp.so`）。
2. 如果存在 `$EXE_DIR/data/icudtl.dat`，设置 ICU 数据路径。
3. 创建 `FlView`，从中提取 `FlEngine` 和 `FlBinaryMessenger`。
4. 传递给 `FlutterViewHostLinux` 构造函数。

**注意**：Linux 上的 `fl_view_new` 不支持自定义 entrypoint，始终运行 Dart 的 `main()` 函数。

### 2.2 FlutterViewHost（视图层）

`FlutterViewHost` 是一个抽象接口，表示单个嵌入的 Flutter 视图。一个 `FlutterEngineHost` 可以创建多个 `FlutterViewHost` 实例。

```cpp
class FlutterViewHost {
public:
    using Reply = std::function<void(const std::string& result)>;
    using MethodCallHandler = std::function<void(
        const std::string& method,
        const std::string& arguments,
        Reply reply)>;

    virtual ~FlutterViewHost() = default;

    virtual void embedInto(void* parentView) = 0;    // 将 Flutter 视图嵌入父窗口
    virtual void resize(int width, int height) = 0;  // 调整视图大小
    virtual void invokeMethod(const std::string& method,
                              const std::string& arguments) = 0;  // C++ → Dart 调用
    virtual void setMethodCallHandler(MethodCallHandler handler) = 0; // 设置 Dart → C++ 回调
    virtual void focus() = 0;                        // 将键盘焦点转移到 Flutter 视图

#ifdef __WXMSW__
    virtual void* nativeHandle() const { return nullptr; }  // 返回 HWND（仅 Windows）
#endif
};
```

`nativeHandle()` 仅在 Windows（`#ifdef __WXMSW__`）存在，原因：

- **Windows**: wxWidgets 的焦点管理系统需要知道“具有焦点的实际 HWND”。`MSWGetFocusHWND()` 和 `ContainsHWND()` 覆盖都使用 `nativeHandle()` 返回的 Flutter 子 HWND，让 `wxWindow::SetFocus()` 将 `::SetFocus` 重定向到正确的子窗口。
- **macOS**: 不需要——Cocoa 通过 `NSView` 指针（而非 HWND）管理焦点，`makeFirstResponder:` 直接接收 `NSView*`，从 `controller.view` 获取即可。
- **Linux**: 不需要——GTK 通过 `GtkWidget*` 管理焦点，`gtk_widget_grab_focus` 直接接收 `GtkWidget*`，从 `m_view` 成员获取即可。

简而言之，`nativeHandle()` 是为 wxWidgets 的 MSW 焦点管理量身定制的桥接方法，其他平台的 UI 框架不依赖原生窗口句柄进行焦点操作。

#### 平台实现对比

| 操作 | Windows (`FlutterViewHostWin`) | macOS (`FlutterViewHostMacOS`) | Linux (`FlutterViewHostLinux`) |
|------|-------------------------------|-------------------------------|--------------------------------|
| **嵌入** | `SetParent(childHwnd, parentHwnd)` — 重新设置 Flutter 子窗口的父窗口。Flutter 引擎已在创建时将视图设置为 `WS_CHILD` 样式。 | `[cv addSubview:fv]` — Cocoa 标准的子视图添加。设置 `autoresizingMask` 实现自动布局。 | 将 `FlView` 包装在 `GtkEventBox` 中，通过 `wxPizza::put()` 放置到 wxWidgets 的 pizza 容器中。EventBox 确保 GTK 焦点链正常工作。 |
| **大小调整** | `SetWindowPos` 调整 HWND 位置和尺寸（无 Z 序/激活变化）。 | 空实现——依赖 Cocoa 的 autoresizingMask 自动处理。 | 通过 `wxPizza::move()` + `wxPizza::size_allocate_child()` 更新 GTK 布局，同时更新 `gtk_widget_set_size_request`。 |
| **焦点** | `::SetFocus(hwnd)` | `[window makeFirstResponder:view]` | `gtk_widget_grab_focus(view)` |
| **MethodCall** | 通过 `FlutterDesktopMessengerSetCallback` 注册原始回调，手动解码 MethodCall 消息。 | 使用 `FlutterMethodChannel` 的 ObjC API，直接处理 Dart 侧的 `invokeMethod`。 | 使用 `FlMethodChannel` 的 GObject API，回调接收 `FlValue*` 类型的参数。 |

### 2.3 FlutterPanel（wxWidgets 桥接层）

`FlutterPanel` 继承自 `wxWindow`，是 Flutter 视图在 wxWidgets 控件树中的代理节点。它不直接渲染任何内容——其子 HWND/NSView/GtkWidget 由 Flutter 引擎托管。

#### 构造与初始化

```cpp
FlutterPanel::FlutterPanel(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxFULL_REPAINT_ON_RESIZE | wxCLIP_CHILDREN) {
    SetBackgroundColour(*wxBLACK);
    Bind(wxEVT_SIZE, &FlutterPanel::onSize, this);
    Bind(wxEVT_SHOW, &FlutterPanel::onShow, this);
    Bind(wxEVT_SET_FOCUS, &FlutterPanel::onSetFocus, this);
}
```

关键样式：
- `wxFULL_REPAINT_ON_RESIZE`：确保尺寸变化时 Flutter 视图得到正确的重绘事件。
- `wxCLIP_CHILDREN`：避免 wxWidgets 自己的绘制覆盖到 Flutter 子窗口区域。

#### 延迟嵌入机制

`startView()` 调用后，视图被创建但不会立即嵌入到 wx Panel 中：

```cpp
bool FlutterPanel::startView(...) {
    m_view = engine->createView(entrypoint, channelName);
    // ... 设置 handler ...
    tryEmbed();  // 尝试立即嵌入（如果面板已有有效尺寸则成功）
    if (m_embedded) {
        wxSize sz = GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    return true;
}

void FlutterPanel::tryEmbed() {
    if (m_embedded || !m_view) return;
    wxSize sz = GetSize();
    if (sz.GetWidth() <= 1 || sz.GetHeight() <= 1) return;  // 无效尺寸，等待
    m_view->embedInto(GetHandle());
    m_embedded = true;
}
```

延迟的原因：当 `FlutterPanel` 位于 wxNotebook 的非活动页面时，wxWidgets 分配的初始尺寸可能是 0x0 或 1x1。如果在此状态下调用 `embedInto`，某些平台实现会回退到硬编码的 800x600，导致在面板首次显示时出现短暂的尺寸跳动。`tryEmbed()` 在 `onSize` 和 `onShow` 事件中调用，保证嵌入发生在面板获取到真实客户端尺寸之后。

#### 大小管理

```cpp
void FlutterPanel::onSize(wxSizeEvent& event) {
    if (m_view) {
        tryEmbed();  // 尚未嵌入则尝试嵌入
        wxSize sz = event.GetSize();  // 使用事件尺寸，而非 GetClientSize()
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    event.Skip();
}

void FlutterPanel::onShow(wxShowEvent& event) {
    if (event.IsShown() && m_view) {
        tryEmbed();
        wxSize sz = GetSize();  // 使用 wx 追踪的尺寸
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    event.Skip();
}
```

**重要细节**：在 `onSize` 中使用 `event.GetSize()` 而非 `GetClientSize()`。在 GTK 平台上，`GetClientSize()` 查询 GTK 的 widget allocation，该值可能在 GTK 尚未处理大小分配时是过时的。`event.GetSize()` 携带着 wxWidgets 已经确认的尺寸信息。

#### 焦点管理

`FlutterPanel` 的焦点管理是架构中最精细的部分，需要协调 wxWidgets、操作系统和 Flutter 引擎三方的焦点行为。

**设计原则**：`FlutterPanel` 本身不接受焦点（`AcceptsFocus() returns false`），所有键盘输入应该直达内部的 Flutter 子窗口。

```cpp
bool AcceptsFocus() const override { return false; }
```

**SetFocus 重写**：

```cpp
void FlutterPanel::SetFocus() {
#ifdef __WXMSW__
    // MSWGetFocusHWND() 返回 Flutter 子窗口 HWND，
    // 因此 wxWindow::SetFocus() 直接调用 ::SetFocus(childHwnd)。
    wxWindow::SetFocus();
#else
    wxWindow::SetFocus();
    if (m_view) m_view->focus();
#endif
}
```

**onSetFocus 与 CallAfter**：

```cpp
void FlutterPanel::onSetFocus(wxFocusEvent& event) {
    CallAfter([this] {
        if (m_view) m_view->focus();
    });
    event.Skip();
}
```

使用 `CallAfter` 的原因：在 Windows 上，在处理 `WM_SETFOCUS` 消息期间调用 `::SetFocus` 会被操作系统静默忽略。`CallAfter` 将焦点转移推迟到当前消息处理完成之后。

**MSW 特有重写**：

| 方法 | 作用 |
|------|------|
| `MSWGetFocusHWND()` | 返回 Flutter 子窗口的 HWND，使 `wxWindow::SetFocus()` 直接对子窗口调用 `::SetFocus`。 |
| `ContainsHWND(WXHWND)` | 检查给定 HWND 是否是 Flutter 子窗口，使 wxWidgets 将子窗口识别为此 panel 的组成部分。 |
| `MSWShouldPreProcessMessage(WXMSG*)` | 对于目标是 Flutter 子窗口的消息返回 `false`，阻止 wxWidgets 的 `PreProcessMessage` 父链遍历将 `WM_KEYDOWN` 转换为 `WM_CHAR`，确保 Flutter 的 WndProc 按正确顺序接收两种消息。详见注释（第 97-108 行）。 |

### 2.4 Dispatcher（MethodChannel 路由）

`Dispatcher` 是一个轻量的 dispatch 表，将 MethodChannel 的方法名映射到 C++ 处理函数。

```cpp
class Dispatcher {
public:
    using Fn = std::function<void(
        const std::string& args, FlutterViewHost::Reply reply)>;

    Dispatcher& on(const std::string& method, Fn fn) {
        m_map[method] = std::move(fn);
        return *this;  // 链式调用
    }

    FlutterViewHost::MethodCallHandler handler() {
        auto map = m_map;  // 捕获值副本
        return [map](const std::string& method,
                     const std::string& args,
                     FlutterViewHost::Reply reply) {
            auto it = map.find(method);
            if (it != map.end()) {
                try {
                    it->second(args, reply);
                } catch (const std::exception& e) {
                    reply(std::string("Error: ") + e.what());
                } catch (...) {
                    reply("Error: unknown");
                }
            } else {
                reply("");  // 未注册的方法返回空字符串
            }
        };
    }

private:
    std::unordered_map<std::string, Fn> m_map;
};
```

**使用示例**：

```cpp
Dispatcher disp;
disp.on("getPrinterList", [](const std::string& args, auto reply) {
    std::string json = fetchPrinterListAsJson();
    reply(json);
})
.on("setTemperature", [](const std::string& args, auto reply) {
    // args 是 JSON 字符串：{"target": 210}
    setNozzleTemperature(args);
    reply("ok");
});

// 注册到 FlutterPanel
panel->startView(engine, "main", "orca_channel", disp.handler());
```

关键设计点：
- **值捕获**：`handler()` 返回的 lambda 捕获 `map` 的**副本**（`auto map = m_map;`）。这意味着 `Dispatcher` 对象可以在 handler 被调用前安全析构，handler 持有自己的 dispatch 表副本。
- **异常安全**：每个 handler 调用被 try/catch 包裹。如果 handler 抛出异常，reply 会收到错误信息字符串，确保 Dart 侧的 future 不会无限等待。
- **未注册方法**：返回空字符串（不是错误），这是一种轻量的“未实现”响应，不会让 Dart 端的 `invokeMethod` 抛出异常。

### 2.5 构建系统集成

#### EnsureFlutterDeps.cmake

`ensure_flutter_deps()` 函数自动从 Flutter SDK 中提取引擎运行时文件并复制到 `CMAKE_PREFIX_PATH`（即 `deps/` 目录）。调用时机：在 CMakeLists.txt 中配置完 `CMAKE_PREFIX_PATH` 之后。

**Flutter SDK 发现策略**（按优先级）：
1. 环境变量 `FLUTTER_HOME`
2. macOS 上检查 Homebrew Caskroom（`/opt/homebrew/Caskroom/flutter/`），选择最新版本
3. 通过 `find_program(flutter)` 搜索 PATH 中的 flutter 命令，反向定位 SDK 目录

**各平台复制内容**：

| 平台 | 源路径（相对 `$FLUTTER_SDK/bin/cache/artifacts/engine`） | 复制到 `deps/` 的哪个子目录 |
|------|-------------------------------------------------------|--------------------------|
| macOS | `darwin-x64-release/FlutterMacOS.xcframework/macos-arm64_x86_64/FlutterMacOS.framework` | `FlutterMacOS.framework/` |
| Windows | `windows-x64-release/flutter_windows.h` 等头文件 | `include/flutter/` |
| | `windows-x64-release/flutter_windows.dll` | `bin/` |
| | `windows-x64-release/flutter_windows.dll.lib` | `lib/` |
| | `windows-x64-release/flutter_engine.dll`（如果存在） | `bin/` |
| | `windows-x64-release/icudtl.dat` | `bin/` |
| Linux | `linux-x64-release/flutter_linux/` 头文件目录 | `include/flutter/flutter_linux/` |
| | `linux-x64-release/libflutter_linux_gtk.so` | `lib/` |
| | `linux-x64-release/libflutter_engine.so`（如果存在） | `lib/` |
| | `linux-x64-release/icudtl.dat`（如果存在） | `bin/` |

如果 Flutter SDK 未找到且 `deps/` 中也没有对应文件，CMake 将报 fatal error 并提示用户安装 Flutter 或运行 `flutter precache --<platform>`。

#### BuildFlutterApp.cmake

`build_flutter_app()` 函数在 CMake 构建阶段编译 Flutter 应用（Dart 代码）。该函数被顶层 `CMakeLists.txt` 调用，定义 custom target `flutter_app`，使其成为 `ALL` 构建目标的一部分。

**参数**：
- `FLUTTER_APP_DIR`：Flutter 项目目录（默认 `${CMAKE_SOURCE_DIR}/flutter_app`）
- `OUTPUT_DIR`：构建产物输出目录（默认 `${CMAKE_BINARY_DIR}/flutter_app`）

**各平台构建流程**：

| 平台 | 构建命令 | 主要输出产物 |
|------|---------|------------|
| macOS | `flutter build macos --release` | `App.framework/` — 包含 AOT 编译的 Dart 代码和 flutter_assets |
| Windows | `flutter build windows --release`（通过 `cmd /c` 调用 `.bat`） | `flutter_app.dll`（重命名自 `app.so`）+ `data/` 目录 |
| Linux | `flutter build linux --release` | `lib/libflutter_app.so`（重命名自 `libapp.so`）+ `data/` 目录 |

**Windows 特殊处理**：Windows 的 flutter build 输出为 `app.so` 文件（实际上是 Windows DLL），被重命名为 `flutter_app.dll`。构建命令通过一个中间 CMake 脚本（`build_flutter_win.cmake`）执行，以正确处理 `flutter.bat` 的调用（CI 环境中 `execute_process` 可能无法直接识别 `.bat` 文件）。

**依赖触发**：custom command 依赖 `pubspec.yaml` 和 `lib/main.dart`，如果这些文件有变化则触发重新构建。此外提供 `flutter_app_rebuild` target 用于强制重建。

#### CMakeLists.txt 集成

在 `src/slic3r/CMakeLists.txt` 中各平台的集成方式：

```cmake
# Windows
ensure_flutter_deps()
target_include_directories(libslic3r_gui PRIVATE "${CMAKE_PREFIX_PATH}/include/flutter")
target_link_libraries(libslic3r_gui "${CMAKE_PREFIX_PATH}/lib/flutter_windows.dll.lib")

# Linux
ensure_flutter_deps()
target_include_directories(libslic3r_gui PRIVATE "${CMAKE_PREFIX_PATH}/include/flutter")
target_link_libraries(libslic3r_gui "${CMAKE_PREFIX_PATH}/lib/libflutter_linux_gtk.so")

# macOS
ensure_flutter_deps()
target_include_directories(libslic3r_gui PRIVATE "${CMAKE_PREFIX_PATH}/FlutterMacOS.framework/Headers")
target_compile_options(libslic3r_gui PRIVATE "-F${CMAKE_PREFIX_PATH}")
set_source_files_properties(GUI/Flutter/flutter_host_macos.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")  # 启用 ARC for Objective-C++
```

顶层 `CMakeLists.txt` 中将产物复制到发布目录（Windows），并调用 `build_flutter_app()`。

---

## 第三章：MethodChannel 通信协议

### 3.1 线格式（Wire Format）

Flutter 嵌入使用 **StandardMethodCodec**（基于 Flutter 标准消息编码），这是一种二进制序列化格式。编解码逻辑在 Windows 平台手动实现（`flutter_host_win.cpp` 第 13-193 行），macOS 和 Linux 使用引擎自带的编解码器。

#### 类型系统

每种值以单字节类型标记开头：

| 类型标记 | 十六进制 | 值 | 编码方式 |
|---------|---------|-----|---------|
| null | `0x00` | 空值 | 仅标记字节，无后续数据 |
| true | `0x01` | 布尔真 | 仅标记字节 |
| false | `0x02` | 布尔假 | 仅标记字节 |
| int32 | `0x03` | 32 位有符号整数 | 标记 + 4 字节小端序 |
| int64 | `0x04` | 64 位有符号整数 | 标记 + 8 字节小端序 |
| float64 | `0x06` | IEEE 754 双精度浮点 | 标记 + 8 字节（通过 `memcpy` 保留位模式） |
| string | `0x07` | UTF-8 字符串 | 标记 + varint(字节长度) + 原始字节 |

**注意**：没有 `0x05`——在 Flutter 编码规范中，`0x05` 预留给未来扩展使用。

#### Varint 编码

变长整数（varint）是一种 7 位编码方案，用于紧凑地表示非负整数：

**编码算法**（`writeVarint`）：
```
while value >= 0x80:
    emit (value & 0x7F) | 0x80   // 低 7 位 + 继续标志
    value >>= 7
emit value & 0x7F                  // 最后一个字节不含继续标志
```

每个字节的低 7 位存储数据，最高位（bit 7）为 1 表示后续还有字节，为 0 表示这是最后一个字节。

**解码算法**（`readVarint`）：
```
value = 0, shift = 0
while 还有数据:
    byte = next_byte
    value |= (byte & 0x7F) << shift
    if (byte & 0x80) == 0: break   // 最高位为 0 → 结束
    shift += 7
    if shift >= 32: return {0, pos}  // 溢出保护
return {value, pos}
```

溢出保护：当 shift 达到 32 位时停止解码，防止畸形输入导致的无符号整数溢出。

**示例**：字符串 `"hello"` 编码为 `0x07 0x05 68 65 6C 6C 6F`
- `0x07`：string 类型标记
- `0x05`：varint 编码的长度（5 个字节）
- `68 65 6C 6C 6F`：UTF-8 字节（`h`, `e`, `l`, `l`, `o`）

### 3.2 消息结构

#### MethodCall 消息

Dart 端调用 `channel.invokeMethod(name, arguments)` 时，编码器生成以下布局：

```
┌───────────────┬───────────────┐
│ encoded_name  │ encoded_args  │
│  (value)      │  (value)      │
└───────────────┴───────────────┘
```

两者都是 StandardMethodCodec 编码的值。例如 `channel.invokeMethod("greet", "world")` 编码为：

```
[0x07, 0x05, 'g','r','e','e','t',  0x07, 0x05, 'w','o','r','l','d']
  ───── method: string ───────      ───── args: string ───────
```

解码端（`decodeMethodCall`）依次调用两次 `decodeValue`，先解码方法名，再解码参数。

#### 响应封装（Reply Envelope）

C++ handler 通过 `reply(resultString)` 返回结果时，结果被包装在**成功封装**或**错误封装**中：

**成功封装**（`encodeSuccess`）：

```
┌────────┬──────────────────┐
│  0x00  │  encoded_result  │
│ 成功标记│  (value)         │
└────────┴──────────────────┘
```

**错误封装**（`encodeError`）：

```
┌────────┬──────────────────┬──────────────────┬────────┐
│  0x01  │  encoded_code    │  encoded_message  │  0x00  │
│        │  (string)        │  (string)         │  null  │
└────────┴──────────────────┴──────────────────┴────────┘
```

编码字段说明：
- `code`：错误码（如 `"EXCEPTION"`）。使用 `writeString` 编码（`0x07` + varint 长度 + 内容）。
- `message`：人类可读的错误描述。同样使用 `writeString` 编码。
- `details`：始终为 null（`0x00`），因为我们不传递结构化的错误详情。

#### 完整编码对照表

| 场景 | 实际字节布局 |
|------|------------|
| C++→Dart invokeMethod `"ping", ""` | `[0x07, 0x04, 'p','i','n','g', 0x00]` |
| Dart→C++ 调用 `"getStatus", null` | `[0x07, 0x09, 'g','e','t','S','t','a','t','u','s', 0x00]` |
| C++ reply `"ok"` → 成功封装 | `[0x00, 0x07, 0x02, 'o','k']` |
| C++ reply（异常）→ 错误封装 | `[0x01, 0x07, 0x09, 'E','X','C','E','P','T','I','O','N', 0x07, 0x03, 'o','o','m', 0x00]` |

### 3.3 writeValue 自动类型检测

Windows 实现的 `writeValue()` 函数采用智能类型检测策略（第 59-101 行）。当 C++ 代码调用 `invokeMethod(name, "42")` 时，参数是字符串 `"42"`，但 `writeValue` 会尝试将其编码为数字类型，以便 Dart 端可以通过 `invokeMethod<int>("name")` 接收正确的 `int` 类型。

**检测顺序**：

```
字符串输入
  │
  ├─ 1. 尝试 strtoll() ──── 成功且无 ERANGE ──→ 在 INT32_MIN..INT32_MAX 内？
  │    (10 进制)                                      │           │
  │                                              是 ↓         否 ↓
  │                                          编码为        编码为
  │                                          int32 (0x03)  int64 (0x04)
  │
  ├─ 2. 尝试 strtod() ──── 成功且无 ERANGE ──→ 编码为 float64 (0x06)
  │    (浮点解析)
  │
  └─ 3. 回退 ──→ 编码为 string (0x07)
```

**边界情况处理**：

| 输入 | 检测结果 | 编码类型 | 原因 |
|------|---------|---------|------|
| `""` （空字符串） | 跳过 strtoll（`s.empty()` 为 true） | 回退到 string (len=0) | 空字符串不应该被解释为整数 0 |
| `"42"` | strtoll 成功，`42` 在 INT32 范围内 | int32 | 最常见的使用场景 |
| `"9999999999"` | strtoll 成功，溢出 INT32 | int64 | `9999999999 > INT32_MAX` |
| `"3.14"` | strtoll 只解析 `3`，`*end == '.'` | 进入 strtod，成功 | float64 |
| `" 42"` | strtoll 跳过前导空白 → 成功 | int32 | 但前导空白不是好的实践 |
| `"42abc"` | strtoll 在 `a` 停止，`*end != '\0'` | 回退到 string | `end` 不指向字符串末尾 |
| HUGE 值 | strtoll 返回 LLONG_MAX，errno=ERANGE | 进入 strtod → 也 ERANGE | 回退到 string |
| `"text"` | strtoll 返回 0，end 指向第一个非数字字符 | 进入 strtod → 也失败 | 回退到 string |

**ERANGE 保护**：`errno != ERANGE` 检查至关重要。当输入超出 `strtoll` 的范围时，`errno` 被设置为 `ERANGE`，即使 `end` 指向字符串末尾，结果也是截断的。程序正确地跳过该编码并继续尝试 strtod，最终回退到 string。

### 3.4 Reply 合约

MethodChannel handler（无论是 `Dispatcher::handler()` 返回的 lambda，还是直接设置的 handler）必须遵守以下合约：

**规则 1：Handler 必须恰好调用一次 reply**
- 调用 `reply(result)` 发送成功响应（带成功封装）。
- 如果 handler 抛出异常，框架会自动调用 `reply`（带错误封装）。
- 如果 handler 存储了 `reply` 以供异步使用（例如网络回调），它必须确保最终调用 `reply`。
- 如果 handler 既不调用 `reply` 也不抛出异常，框架会发送空响应（`nullptr, 0`），防止 Dart 侧的 future 永久挂起。

**规则 2：重复调用 reply 被静默忽略**

Windows 实现使用 `shared_ptr<bool>` guard：

```cpp
auto replied = std::make_shared<bool>(false);

Reply reply = [..., replied](const std::string& result) {
    if (*replied) return;   // 已经回复过，忽略
    *replied = true;
    // ... 发送响应 ...
};
```

**三重保护机制**（`messageCallback` 的完整执行路径）：

```
收到 Dart 消息
  │
  ├─ 1. shared_ptr<bool> replied guard
  │     └─ 防止 handler 内部多次调用 reply
  │
  ├─ 2. try/catch 块
  │     ├─ std::exception → 发送 error envelope ("EXCEPTION", e.what())
  │     └─ ... (未知异常)  → 发送 error envelope ("EXCEPTION", "unknown error")
  │
  └─ 3. 返回后检查
        └─ 如果 !*replied → 发送空响应
           (handler 存储了 reply 但从未调用)
```

这在三种平台上略有差异但语义一致：

| 平台 | 成功响应 | 错误响应 | 未回复 fallback |
|------|---------|---------|---------------|
| Windows | `FlutterDesktopMessengerSendResponse(..., encoded.data(), encoded.size())` | 同左，但内容为 error envelope | `FlutterDesktopMessengerSendResponse(..., nullptr, 0)` |
| macOS | `fResult(decodeArg(out))` 即 `[FlutterResult invoke]` | 在 Dispatch 层捕获（第 28-31 行） | `fResult(FlutterMethodNotImplemented)` |
| Linux | `fl_method_call_respond_success(method_call, val, &error)` | `fl_method_call_respond_error(method_call, "EXCEPTION", ...)` | `fl_method_call_respond_not_implemented(method_call, nullptr)` |

### 3.5 线程模型

**所有 MethodChannel 调用都在平台线程（主线程）上执行。**

- Flutter Engine 的 platform thread 负责将消息从 Dart isolate 传递到 native 端。
- 在嵌入场景中，这天然地是 wxWidgets 的主线程（UI 线程）。
- 因此 handler 分发不需要跨线程同步——`Dispatcher` 的 `m_map` 不需要加锁。

**异步 reply**：
支持的场景：handler 存储 `reply` 闭包，从异步回调（网络请求完成、文件 I/O 完成等）中调用它。此时 reply lambda 捕获了 `response_handle`/`method_call` 引用，可以从任意线程安全地调用（Flutter C API 保证线程安全的 messenging）。

### 3.6 invokeMethod（C++ → Dart）

C++ 端调用 Dart 方法使用 `FlutterViewHost::invokeMethod(method, arguments)`：

```cpp
// flutter_host_win.cpp
void invokeMethod(const std::string& method,
                  const std::string& arguments) override {
    if (!m_messenger) return;
    auto encoded = encodeMethodCall(method, arguments);
    FlutterDesktopMessengerSend(
        m_messenger, m_channel.c_str(),
        encoded.data(), encoded.size());
}
```

**特点**：
- **发射后不管**（Fire-and-forget）——`invokeMethod` 不接收返回值。如果需要 Dart 返回结果，应该由 Dart 端通过另一个 `invokeMethod` 回调通知 C++。
- **类型自动检测**——`writeValue` 确保字符串参数被正确编码为数字或字符串类型，这样 Dart 端可以使用 `invokeMethod<int>`、`invokeMethod<double>` 或 `invokeMethod<String>` 并接收到正确类型。
- **线程安全**——`FlutterDesktopMessengerSend`（Windows）和 `fl_method_channel_invoke_method`（Linux）可以从任意线程安全调用。

#### 平台对比

| 平台 | C++ → Dart 实现 | 类型转换 |
|------|----------------|---------|
| Windows | `encodeMethodCall` + `FlutterDesktopMessengerSend` | `writeValue()` 自动检测（strtoll → strtod → string） |
| macOS | `[channel invokeMethod: arguments:]` | `decodeArg()`: strtol 成功 → `@(val)` NSNumber，否则 → NSString |
| Linux | `fl_method_channel_invoke_method(channel, method, args, ...)` | strtoll 成功 → `fl_value_new_int`，否则 → `fl_value_new_string` |

#### Dart 端接收

在 Dart 端，MethodChannel 的 handler 接收的参数类型取决于 `writeValue`（Windows）或 `decodeArg`（macOS）的编码结果：

```dart
// 如果 C++ 调用 invokeMethod("status", "42")
// Windows: writeValue 检测到 "42" 是整数 → 编码为 int32
// Dart 端:
channel.setMethodCallHandler((call) async {
  if (call.method == 'status') {
    // call.arguments 是 int 类型（42）
    int code = call.arguments as int;  // 正确
  }
});
```


## 第4章：Windows 平台实现

### 4.1 FlutterEngineHostWin

`FlutterEngineHostWin` 是 Windows 平台上的引擎宿主管道。与直觉相反，它的 `start()` 和 `stop()` 几乎为空——引擎的真正生命周期按视图（per-view）管理，因为 Windows 的 Flutter 嵌入层将引擎与视图控制器绑定在一起。

#### start() / stop() 的最小化设计

```cpp
bool start() override {
    m_started = true;
    return true;   // 无实际初始化，所有工作延迟到 createView()
}

void stop() override {
    m_started = false;   // 无实际清理，每个视图自行销毁
}
```

这里的 `m_started` 标志纯粹是状态跟踪，不触发任何 Flutter C API 调用。真正的引擎创建发生在 `createView()` 中。

#### createView() — 视图与引擎的联合创建

这是 Windows 平台上最关键的函数，完成路径解析、引擎初始化、视图创建三步走：

**第一步：路径解析（基于可执行文件位置，而非当前工作目录）**

```cpp
wchar_t exe_path[MAX_PATH];
DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
// 从完整路径中提取目录部分
// 例如 C:\Program Files\OrcaSlicer\orca-slicer.exe → C:\Program Files\OrcaSlicer
```

然后构建三个相对于 exe 目录的路径：

| 资源 | 相对路径 | 说明 |
|------|---------|------|
| `assets_path` | `data\flutter_assets` | Flutter 资源包（kernel_blob.bin, fonts, shaders 等） |
| `icu_data_path` | `data\icudtl.dat` | ICU 国际化数据文件 |
| `aot_library_path` | `flutter_app.dll` | 预编译（AOT）的 Dart 代码库 |

**关键设计决策**：使用 `GetModuleFileNameW(nullptr, ...)` 而非 `GetCurrentDirectory()`，确保路径在程序被其他进程以不同 CWD 启动时仍然正确。这是生产环境中常见的崩溃点修复。

**第二步：引擎创建与启动**

```cpp
FlutterDesktopEngineProperties props = {};
props.assets_path = assets.c_str();
props.icu_data_path = icu.c_str();
props.aot_library_path = aot.c_str();

FlutterDesktopEngineRef engine = FlutterDesktopEngineCreate(&props);
if (!engine) return nullptr;

const char* ep = entrypoint.empty() ? nullptr : entrypoint.c_str();
if (!FlutterDesktopEngineRun(engine, ep)) {
    FlutterDesktopEngineDestroy(engine);
    return nullptr;
}
```

注意 `entrypoint` 为 `nullptr` 时 Flutter 运行 `main()`；非空时运行指定的入口函数。引擎创建和启动分开两步，允许在启动前配置引擎属性。失败时通过 `FlutterDesktopEngineDestroy` 清理，防止资源泄漏。

**第三步：视图控制器创建**

```cpp
FlutterDesktopViewControllerRef controller =
    FlutterDesktopViewControllerCreate(800, 600, engine);
```

800x600 是 Flutter 视图的默认初始尺寸。值得注意的是 `FlutterDesktopViewControllerCreate` 内部会创建一个 `FlutterWindow` 对象，该对象在构造时立即将视图注册为子窗口（详见 4.5 节的 WS_CHILD 讨论）。创建失败时同样需要销毁引擎。

**第四步：组装 FlutterViewHostWin**

```cpp
auto* view = new FlutterViewHostWin(controller, engine, channelName);
return std::unique_ptr<FlutterViewHost>(view);
```

将控制器、引擎和通道名传给 `FlutterViewHostWin` 构造函数，由其完成 MethodChannel 的注册。

---

### 4.2 FlutterViewHostWin

`FlutterViewHostWin` 是 Windows 平台上与单个 Flutter 视图交互的门面，封装了视图控制器、引擎引用、BinaryMessenger 和 MethodChannel。

#### 构造函数 —— 消息回调注册

```cpp
FlutterViewHostWin(FlutterDesktopViewControllerRef ctrl,
                   FlutterDesktopEngineRef engine,
                   const std::string& channelName)
    : m_controller(ctrl), m_engine(engine), m_channel(channelName) {
    m_messenger = FlutterDesktopEngineGetMessenger(engine);
    if (m_messenger) {
        FlutterDesktopMessengerSetCallback(
            m_messenger, m_channel.c_str(), messageCallback, this);
    }
}
```

构造函数的核心职责是建立 C++/Dart 通信通道：
1. 从引擎获取 `FlutterDesktopMessengerRef`——这是通道通信的总线
2. 调用 `FlutterDesktopMessengerSetCallback` 注册静态回调 `messageCallback`，`this` 作为 `userData` 传递
3. 通道名（如 `"snapmaker/home"`）区分不同用途的消息通道

#### embedInto(void* parentView) —— 将 Flutter 视图嵌入 wxWidgets 面板

这是平台集成中最巧妙的设计之一：

```cpp
void embedInto(void* parentView) override {
    FlutterDesktopViewRef view =
        FlutterDesktopViewControllerGetView(m_controller);
    HWND childHwnd = FlutterDesktopViewGetHWND(view);
    HWND parentHwnd = static_cast<HWND>(parentView);

    if (!childHwnd || !parentHwnd) return;

    // Flutter 引擎在创建视图时已设置 WS_CHILD 样式（见 flutter_window.cc 的
    // FlutterWindow::InitializeChild），无需手动修改窗口样式。
    SetParent(childHwnd, parentHwnd);

    RECT rect;
    GetClientRect(parentHwnd, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w <= 1) w = 800;
    if (h <= 1) h = 600;
    SetWindowPos(childHwnd, nullptr,
                 0, 0, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
```

**关键设计要点**：

1. **无样式操作**：Flutter 引擎在 `FlutterWindow::InitializeChild` 中已将视图创建为 `WS_CHILD` 窗口。早期版本中尝试手动添加 `WS_CHILD` 样式导致了重复设置和视觉闪烁。当前实现仅需 `SetParent` 完成父子关系绑定。

2. **回退尺寸**：当父窗口尚未布局（尺寸 ≤ 1 像素）时，使用 800x600 回退尺寸。这种情况常见于面板初次显示或在未选中的 notebook 页面中。后续的 `wxEVT_SIZE` 事件会调用 `resize()` 修正尺寸。

3. **SetWindowPos 标志选择**：
   - `SWP_NOZORDER` — 保持当前 Z 序不变
   - `SWP_NOACTIVATE` — 不激活窗口（避免意外焦点转移）
   - `SWP_SHOWWINDOW` — 确保窗口可见

**SetParent 必须在 SetWindowPos 之前执行**，否则窗口定位会基于错误的父窗口坐标系。

#### resize(int width, int height)

```cpp
void resize(int width, int height) override {
    FlutterDesktopViewRef view =
        FlutterDesktopViewControllerGetView(m_controller);
    HWND hwnd = FlutterDesktopViewGetHWND(view);
    if (!hwnd) return;
    if (width <= 0 || height <= 0) return;
    SetWindowPos(hwnd, nullptr,
                 0, 0, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
```

仅设置尺寸，不改变位置（x=0, y=0 相对于父窗口客户区左上角）。使用相同的最小化标志组合。边界检查拒绝零或负尺寸。

#### invokeMethod —— 从 C++ 调用 Dart 方法

```cpp
void invokeMethod(const std::string& method,
                  const std::string& arguments) override {
    if (!m_messenger) return;
    auto encoded = encodeMethodCall(method, arguments);
    FlutterDesktopMessengerSend(
        m_messenger, m_channel.c_str(),
        encoded.data(), encoded.size());
}
```

调用链：编码参数 → `FlutterDesktopMessengerSend` → Dart 端 `MethodChannel` 接收。注意这是**单向异步**调用，不等待 Dart 回复。如需等待结果，应使用 `FlutterDesktopMessengerSendWithReply` 变体（当前实现未使用）。

#### focus() —— 将键盘焦点转移至 Flutter 视图

```cpp
void focus() override {
    FlutterDesktopViewRef view =
        FlutterDesktopViewControllerGetView(m_controller);
    HWND hwnd = FlutterDesktopViewGetHWND(view);
    if (hwnd) ::SetFocus(hwnd);
}
```

直接调用 Win32 `::SetFocus`，绕过 wxWidgets 的焦点管理系统。这是有意为之——wxWidgets 焦点管理假设所有子窗口都是 wxWindow，而 Flutter 子窗口是原生 HWND，wxWidgets 无法跟踪其焦点状态。

#### setMethodCallHandler

```cpp
void setMethodCallHandler(MethodCallHandler h) override {
    m_handler = std::move(h);
}
```

直接存储回调函数。实际触发在 `messageCallback` 静态函数中。

#### messageCallback（静态） —— 三层防御的异步消息处理

这是 Windows 平台上 MethodChannel 通信的核心。它是一个静态函数，通过 `userData` 获取 `this` 指针：

```cpp
static void messageCallback(FlutterDesktopMessengerRef messenger,
                             const FlutterDesktopMessage* msg,
                             void* userData) {
    auto* self = static_cast<FlutterViewHostWin*>(userData);
    if (!msg) return;

    auto [method, args] = decodeMethodCall(msg->message, msg->message_size);
```

**第一层防护 —— shared_ptr<bool> 防重复回复**：

```cpp
    auto replied = std::make_shared<bool>(false);

    Reply reply = [messenger, response_handle = msg->response_handle, replied]
                  (const std::string& result) {
        if (*replied) return;   // 已回复，忽略后续调用
        *replied = true;
        auto encoded = encodeSuccess(result);
        FlutterDesktopMessengerSendResponse(
            messenger, response_handle, encoded.data(), encoded.size());
    };
```

`shared_ptr<bool>` 确保即使在 handler 内部发生异常、handler 同步回复后又异步回复，以及 handler 的 lambda 捕获被多次调用的场景下，都能防止重复调用 `FlutterDesktopMessengerSendResponse`（重复回复会导致 use-after-free 崩溃）。

**第二层防护 —— try/catch 包裹 handler 调用**：

```cpp
    try {
        self->m_handler(method, args, reply);
    } catch (const std::exception& e) {
        if (!*replied) {
            *replied = true;
            auto encoded = encodeError("EXCEPTION", e.what());
            FlutterDesktopMessengerSendResponse(
                messenger, msg->response_handle, encoded.data(), encoded.size());
        }
    } catch (...) {
        if (!*replied) {
            *replied = true;
            auto encoded = encodeError("EXCEPTION", "unknown error");
            FlutterDesktopMessengerSendResponse(
                messenger, msg->response_handle, encoded.data(), encoded.size());
        }
    }
```

捕获 `std::exception` 和非标准异常，确保 Dart 端的 `Future` 不会永远挂起。

**第三层防护 —— 兜底空响应**：

```cpp
    if (!*replied) {
        *replied = true;
        FlutterDesktopMessengerSendResponse(
            messenger, msg->response_handle, nullptr, 0);
    }
```

如果 handler 存储了 reply 回调但从未调用它（也从未抛出异常），发送空字节作为兜底响应，防止 Dart `Future` 泄漏。

**三层防护的协同逻辑**：
- handler 同步调用 `reply(...)` → `*replied` 变为 true → 后续 catch 块和兜底检查全部跳过
- handler 抛出异常 → catch 块检查 `*replied` 为 false → 发送错误响应
- handler 返回但未调用 reply（违反约定）→ 兜底检查 `*replied` 为 false → 发送空响应
- handler 异步持有 reply → dart future 等待回调 → `*replied` 保持 false，不发送额外响应

#### ~FlutterViewHostWin —— 析构顺序至关重要

```cpp
~FlutterViewHostWin() override {
    if (m_messenger) {
        FlutterDesktopMessengerSetCallback(
            m_messenger, m_channel.c_str(), nullptr, nullptr);
    }
    if (m_controller) {
        FlutterDesktopViewControllerDestroy(m_controller);
    }
    // m_engine 由 m_controller 持有；在 Windows 上销毁控制器
    // 会自动销毁引擎。
}
```

**必须先注销回调再销毁控制器**。如果在回调仍处于注册状态时销毁控制器，后续到达的消息将访问悬空指针（use-after-free）。`FlutterDesktopViewControllerDestroy` 在 Windows 上的行为是同时销毁引擎——因为引擎内嵌于视图控制器中。

---

### 4.3 FlutterPanel MSW 覆盖 —— 解决焦点与键盘问题

`FlutterPanel` 在 Windows 上的几个 MSW 特定覆盖是键盘输入正确性的关键保障。这些覆盖直接解决了 wxWidgets 焦点管理与嵌入原生 Flutter 子窗口之间的架构冲突。

#### MSWGetFocusHWND() —— 焦点重定向器

```cpp
WXHWND FlutterPanel::MSWGetFocusHWND() const {
    if (m_view) {
        void* childHwnd = m_view->nativeHandle();
        if (childHwnd)
            return (WXHWND)childHwnd;
    }
    return GetHWND();
}
```

当 wxWidgets 调用 `wxWindow::SetFocus()` 时，内部流程是：
```
wxWindow::SetFocus()
  → HWND hwnd = MSWGetFocusHWND()   // 获取应该接收焦点的 HWND
  → ::SetFocus(hwnd)                 // 直接调用 Win32 API
```

重写后，`MSWGetFocusHWND()` 返回 Flutter 子窗口的 HWND，而非面板自身的 HWND。这意味着**一次 `::SetFocus` 调用**直接将焦点送达 Flutter 窗口。不重写时的两步流程（先 set focus 到 panel，再转发到 Flutter）存在竞态条件：wxWidgets 的 notebook 切换或鼠标点击处理可能在转发之前将焦点重新拉回面板。

#### ContainsHWND(WXHWND) —— 焦点身份识别

```cpp
bool FlutterPanel::ContainsHWND(WXHWND hWnd) const {
    if (m_view) {
        void* childHwnd = m_view->nativeHandle();
        if (childHwnd && hWnd == (WXHWND)childHwnd)
            return true;
    }
    return false;
}
```

wxWidgets 焦点系统通过此函数判断一个 HWND 是否"属于"该 wxWindow。当 Flutter 子窗口获得焦点时，系统询问 FlutterPanel 是否包含该 HWND——返回 `true` 阻止 wxWidgets 将焦点拉回面板自身。这直接阻止了 `MSWWindowProc` 中 `WM_LBUTTONDOWN` 处理对 `IsFocusable` 窗口的自动 `SetFocus` 调用。

#### MSWShouldPreProcessMessage(WXMSG*) —— 关键的键盘修复

**这是整个嵌入方案中最关键的修复**，解决了 Flutter 无法接收键盘输入的问题。完整的因果链如下：

##### 问题根源

wxWidgets 的 `PreProcessMessage`（位于 `src/msw/evtloop.cpp:64-155`）是一个父链遍历函数。当 Windows 消息循环收到一条消息时（如 `WM_KEYDOWN`），它会**从 `msg.hwnd` 开始沿 `WS_CHILD` 父链向上遍历**，对每个 wxWidgets 窗口调用 `MSWTranslateMessage`。

对于嵌入 Flutter 的场景，父链为：
```
Flutter 子窗口 (WS_CHILD, 非 wxWidgets 窗口)
  → FlutterPanel (wxWindow, 通过 SetParent 成为父窗口)
```

`PreProcessMessage` 找到 `FlutterPanel` 后调用其 `MSWTranslateMessage`，而基类的默认实现调用 `::TranslateMessage`。

`::TranslateMessage` 的行为是：
1. 接收 `WM_KEYDOWN` 消息
2. 生成对应的 `WM_CHAR` 或 `WM_DEADCHAR` 消息
3. **将生成的字符消息投递到消息队列**
4. 返回 **非零值**（表示已处理）

wxWidgets 将 `MSWTranslateMessage` 的非零返回值解释为"消息已消费"，因此 **`WM_KEYDOWN` 被标记为已处理，不再传递给 Flutter 的 WndProc**。

但 `::TranslateMessage` 生成的 `WM_CHAR` **仍然被投递到了消息队列**，最终到达 Flutter 的子窗口。Flutter 的 `KeyboardManager` 收到 `WM_CHAR` 时发现**没有待处理的 `WM_KEYDOWN`**（因为 `WM_KEYDOWN` 被 wxWidgets 消费了），遂丢弃该按键事件。

最终效果：**所有字母数字键输入完全丢失**。

##### 为什么 IME（中文输入法）不受影响

`::TranslateMessage` 对于 `WM_IME_*` 消息返回 0（不做转换），而中文输入法产生的 `WM_IME_COMPOSITION` 等消息不受此影响。因此中文 IME 输入正常工作，掩盖了底层键盘问题。

##### 解决方案

```cpp
bool FlutterPanel::MSWShouldPreProcessMessage(WXMSG* msg) {
    if (m_view) {
        HWND child = (HWND)m_view->nativeHandle();
        if (child && msg->hwnd == child)
            return false;   // ← 跳过 PreProcessMessage 父链遍历
    }
    return wxWindow::MSWShouldPreProcessMessage(msg);
}
```

当消息的目标窗口（`msg->hwnd`）是 Flutter 子窗口时，返回 `false`，告诉 wxWidgets **不要对这条消息执行 PreProcessMessage 父链遍历**。这完全绕过了 `MSWTranslateMessage` 调用。

结果是消息循环原生的 `::TranslateMessage` + `::DispatchMessage` 流程直接处理消息：`WM_KEYDOWN` 正常传递给 Flutter 的 WndProc，`WM_CHAR` 在 `WM_KEYDOWN` 之后正常生成并投递——两者以正确的顺序到达 Flutter 的 `KeyboardManager`。

##### 消息流对比——修复前

```
消息循环收到 WM_KEYDOWN (hwnd=FlutterChild)
  └ PreProcessMessage 父链遍历
      └ 找到 FlutterPanel (WS_CHILD 父)
          └ MSWTranslateMessage(msg)
              └ ::TranslateMessage(&msg) 
                  → 生成 WM_CHAR 投递至队列 ✓
                  → 返回非0 ✗
              └ 返回 true（消息已消费）
          └ 标记 WM_KEYDOWN 为已处理 ✗
  └ DispatchMessage → 跳过（已标记处理） ✗

稍后：WM_CHAR 到达 Flutter WndProc
  └ KeyboardManager: 无 pending WM_KEYDOWN → 丢弃 ✗
```

##### 消息流对比——修复后

```
消息循环收到 WM_KEYDOWN (hwnd=FlutterChild)
  └ PreProcessMessage 检查
      └ msg->hwnd == FlutterChild → MSWShouldPreProcessMessage 返回 false
      └ 跳过父链遍历 ✓
  └ ::TranslateMessage(&msg)
      → 生成 WM_CHAR 投递至队列 ✓
  └ ::DispatchMessage(&msg)
      → WM_KEYDOWN 到达 Flutter WndProc ✓
          └ KeyboardManager 记录 pending key-down

消息循环收到 WM_CHAR (hwnd=FlutterChild)
  └ PreProcessMessage 检查 → 同样跳过 ✓
  └ ::DispatchMessage(&msg)
      → WM_CHAR 到达 Flutter WndProc ✓
          └ KeyboardManager: 匹配到 pending key-down → 提交字符 ✓
```

#### SetFocus() 覆盖

```cpp
void FlutterPanel::SetFocus() {
#ifdef __WXMSW__
    // MSWGetFocusHWND() 已返回 Flutter 子窗口 HWND，
    // wxWindow::SetFocus() 直接调用 ::SetFocus(childHwnd)
    wxWindow::SetFocus();
#else
    wxWindow::SetFocus();
    if (m_view) m_view->focus();
#endif
}
```

Windows 路径中，由于 `MSWGetFocusHWND()` 已经返回 Flutter 子窗口 HWND，`wxWindow::SetFocus()` 内部直接调用 `::SetFocus(childHwnd)`。**不需要**先聚焦面板再转发——重写 `MSWGetFocusHWND` 实现了"一步到位"。

非 Windows 路径中（GTK），先调用标准 `wxWindow::SetFocus()` 设置 GTK 焦点链，然后显式调用 `m_view->focus()` 处理 Flutter 视图的焦点。

#### AcceptsFocus() 返回 false

```cpp
bool AcceptsFocus() const override { return false; }
```

FlutterPanel 是**透明包装器**，不是一个真正的可聚焦控件。只有 Flutter 子窗口应接受焦点。返回 `false` 防止：
- `MSWWindowProc` 在 `WM_LBUTTONDOWN` 时对 `IsFocusable` 窗口自动调用 `SetFocus`
- 键盘导航时 wxWidgets 将焦点交给面板而非 Flutter 子窗口

#### onSetFocus —— WM_SETFOCUS 期间的 CallAfter

```cpp
void FlutterPanel::onSetFocus(wxFocusEvent& event) {
    CallAfter([this] {
        if (m_view) m_view->focus();
    });
    event.Skip();
}
```

使用 `CallAfter` 而非直接调用 `m_view->focus()` 的原因：Windows 在 `WM_SETFOCUS` 处理期间**忽略** `::SetFocus` 调用。`CallAfter` 将焦点转移操作推迟到当前消息处理完成后的空闲时刻，确保 `::SetFocus` 能够生效。

---

### 4.4 StandardMethodCodec 手动实现

Windows 平台的 Flutter 嵌入 C API 不提供内置的 MethodCodec。Dart 端使用 `StandardMethodCodec`，C++ 端必须实现完全兼容的编码/解码。以下是字节级别的实现细节。

#### writeVarint —— 7-bit 变长整数编码

```
编码规则：
1. 将整数拆分为 7-bit 分组
2. 除最后一个分组外，其他分组的最高位 (bit 7) 设为 1（延续位）
3. 低地址字节存放低位分组

编码示例（值 300）：
  300 = 0b100101100
  分组1 (低7位): 0b0101100 | 0x80 (延续位) = 0xAC
  分组2 (高2位): 0b0000010                      = 0x02
  编码结果: [0xAC, 0x02]
```

```cpp
static void writeVarint(std::vector<uint8_t>& buf, uint32_t value) {
    while (value >= 0x80) {
        buf.push_back((value & 0x7F) | 0x80);   // 低7位 + 延续位
        value >>= 7;
    }
    buf.push_back(value & 0x7F);                // 最后一组，无延续位
}
```

#### readVarint —— 7-bit 变长整数解码（带溢出保护）

```cpp
static std::pair<uint32_t, size_t> readVarint(const uint8_t* data, size_t size) {
    uint32_t value = 0;
    size_t shift = 0;
    size_t pos = 0;
    while (pos < size) {
        uint8_t byte = data[pos++];
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;   // 延续位为 0，结束
        shift += 7;
        if (shift >= 32) return {0, pos};   // 溢出保护
    }
    return {value, pos};
}
```

**溢出保护**：当 `shift >= 32` 时（即读到第 5 个带延续位的字节），继续解码会导致 `uint32_t` 位移溢出（未定义行为）。直接返回 `{0, pos}` 安全终止解码。

#### writeValue —— 自动类型检测编码

```cpp
static void writeValue(std::vector<uint8_t>& buf, const std::string& s) {
    if (!s.empty()) {
        // 尝试解析为 int32
        errno = 0;
        char* end = nullptr;
        long long val = std::strtoll(s.c_str(), &end, 10);
        if (end && *end == '\0' && errno != ERANGE) {
            if (val >= INT32_MIN && val <= INT32_MAX) {
                buf.push_back(0x03);         // int32 类型标记
                // 小端序写 4 字节
                ...
                return;
            }
            buf.push_back(0x04);             // int64 类型标记
            // 小端序写 8 字节
            ...
            return;
        }
        // 尝试解析为 float64
        errno = 0;
        char* fend = nullptr;
        double dval = std::strtod(s.c_str(), &fend);
        if (fend && *fend == '\0' && errno != ERANGE) {
            buf.push_back(0x06);             // float64 类型标记
            // IEEE 754 双精度 8 字节
            ...
            return;
        }
    }
    // 回退：作为字符串编码
    buf.push_back(0x07);                     // string 类型标记
    writeVarint(buf, s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}
```

**检测顺序的重要性**：
1. 先尝试 `strtoll`（整数），再尝试 `strtod`（浮点数）。`strtod` 对纯整数（如 "42"）也会成功解析，如果调换顺序，所有整数字符串都会被编码为 `float64`，导致 Dart 端的 `invokeMethod<int>` 收到错误类型。
2. `errno != ERANGE` 检查防止溢出值被错误编码。
3. 空字符串直接进入字符串分支（跳过数值检测）。

**类型标记字节映射**：

| 标记 | 类型 | 有效负载 |
|------|------|---------|
| 0x00 | null | 无 |
| 0x01 | true | 无 |
| 0x02 | false | 无 |
| 0x03 | int32 | 4 字节小端序 |
| 0x04 | int64 | 8 字节小端序 |
| 0x06 | float64 | 8 字节 IEEE 754 小端序 |
| 0x07 | string | varint 长度 + UTF-8 字节 |

#### decodeValue —— 完整的 7 类型解码

```cpp
static size_t decodeValue(const uint8_t* data, size_t size, std::string* out) {
    if (size < 1) return 0;
    switch (data[0]) {
        case 0x00: *out = "";       return 1;   // null
        case 0x01: *out = "true";   return 1;   // true
        case 0x02: *out = "false";  return 1;   // false
        case 0x03:                                 // int32
            if (size < 5) return 0;               // 需要 5 字节
            // 小端序组装 4 字节
            uint32_t bits = data[1] | (static_cast<uint32_t>(data[2]) << 8) |
                           (static_cast<uint32_t>(data[3]) << 16) |
                           (static_cast<uint32_t>(data[4]) << 24);
            int32_t val;
            std::memcpy(&val, &bits, sizeof(val));
            *out = std::to_string(val);
            return 5;
        case 0x04:                                 // int64
            if (size < 9) return 0;               // 需要 9 字节
            // 小端序组装 8 字节
            ...
            return 9;
        case 0x06:                                 // float64
            if (size < 9) return 0;               // 需要 9 字节
            // 小端序组装 8 字节
            double dbl;
            std::memcpy(&dbl, &bits, sizeof(dbl));
            *out = std::to_string(dbl);
            return 9;
        case 0x07:                                 // string
            return readString(data, size, out);
        default: return 0;                         // 未知类型
    }
}
```

所有解码值统一转为 `std::string`，保持接口简洁。每个 case 都验证所需的最小字节数，防止缓冲区越界读取。

#### encodeSuccess / encodeError —— 回复信封

```cpp
// 成功信封: [0x00, 编码结果]
static std::vector<uint8_t> encodeSuccess(const std::string& result) {
    std::vector<uint8_t> buf;
    buf.push_back(0x00);   // 成功标记
    writeValue(buf, result);
    return buf;
}

// 错误信封: [0x01, error_code(string), error_message(string), null]
static std::vector<uint8_t> encodeError(const std::string& code,
                                         const std::string& message) {
    std::vector<uint8_t> buf;
    buf.push_back(0x01);   // 错误标记
    writeString(buf, code);
    writeString(buf, message);
    buf.push_back(0x00);   // null details
    return buf;
}
```

#### encodeMethodCall / decodeMethodCall —— 方法调用帧

```cpp
// 方法调用帧: [编码方法名, 编码参数]
static std::vector<uint8_t> encodeMethodCall(const std::string& method,
                                             const std::string& args) {
    std::vector<uint8_t> buf;
    writeValue(buf, method);
    writeValue(buf, args);
    return buf;
}
```

---

### 4.5 Windows 特有陷阱与解决方案

#### 陷阱 #1：WM_SETFOCUS 期间的 ::SetFocus 被静默忽略

Windows 操作系统的内部焦点规则阻止在 `WM_SETFOCUS` 消息处理期间转移焦点。直接调用会导致 `::SetFocus` 返回但无效果。解决方案是使用 `CallAfter` 将焦点转移推迟到消息处理完成之后：

```cpp
void FlutterPanel::onSetFocus(wxFocusEvent& event) {
    CallAfter([this] {
        if (m_view) m_view->focus();
    });
    event.Skip();
}
```

#### 陷阱 #2：SetWindowPos 在 SetParent 之前的视觉闪烁

如果在 `SetParent` 之前调用 `SetWindowPos`，窗口定位基于错误的父窗口坐标系统，导致短暂出现在屏幕错误位置。正确的顺序是先 `SetParent`，再 `SetWindowPos`（如 `embedInto()` 实现所示）。

#### 陷阱 #3：不需要手动设置 WS_CHILD 样式

Flutter 引擎在内部通过 `FlutterWindow::InitializeChild` 已将视图窗口样式设置为 `WS_CHILD`。手动修改样式（如通过 `SetWindowLong`）可能导致：
- 重复设置：`WS_CHILD` 的特定行为被触发两次
- 视觉闪烁：样式变更触发重绘
- 未定义行为：`SetWindowLong` 后 `SetParent` 的交互未完全文档化

当前实现完全信任引擎的默认样式，仅操作父子关系。

---

## 第5章：Linux 平台实现

### 5.1 FlutterEngineHostLinux

#### start() / stop()

```cpp
bool start() override {
    setenv("FLUTTER_LINUX_RENDERER", "software", 1);
    m_started = true;
    return true;
}

void stop() override {
    m_started = false;
}
```

**关键环境变量**：`FLUTTER_LINUX_RENDERER=software` 强制 Flutter 使用软件渲染路径。这在无 GPU 的服务器环境或共享 OpenGL 上下文冲突的场景中至关重要。`1` 参数表示可覆盖已有值。

#### createView() —— 三步构建 + 路径覆盖

与 Windows 不同，Linux 的 `createView()` 使用 GObject 系统，资源路径覆盖发生在 `createView` 内部：

**第一步：创建 FlDartProject**

```cpp
g_autoptr(FlDartProject) project = fl_dart_project_new();
```

`g_autoptr` 确保函数退出时自动释放（GLib 的 RAII 等价物）。

**第二步：覆盖 AOT 库路径**

```cpp
g_autofree gchar* exe_path = g_file_read_link("/proc/self/exe", nullptr);
if (exe_path) {
    g_autofree gchar* exe_dir = g_path_get_dirname(exe_path);
    g_autofree gchar* aot_path = g_build_filename(exe_dir, "lib", "libflutter_app.so", nullptr);
    fl_dart_project_set_aot_library_path(project, aot_path);
}
```

Linux 使用 `/proc/self/exe` 符号链接获取可执行文件路径（与 Windows 的 `GetModuleFileNameW` 对应）。Flutter 默认识别 `lib/libapp.so`，但项目构建系统生成的是 `libflutter_app.so`，必须显式覆盖。

**第三步：覆盖 ICU 数据路径**

```cpp
g_autofree gchar* icu_path = g_build_filename(exe_dir, "data", "icudtl.dat", nullptr);
if (g_file_test(icu_path, G_FILE_TEST_EXISTS)) {
    fl_dart_project_set_icu_data_path(project, icu_path);
}
```

使用 `g_file_test` 检查文件是否存在再设置路径，避免 Flutter 因不存在的路径而失败。

**第四步：创建视图**

```cpp
(void)entrypoint;   // Linux 不支持自定义入口点
FlView* view = fl_view_new(project);
```

**重要限制**：`fl_view_new` 不提供自定义入口点参数——始终运行 `main()`。`entrypoint` 参数被静默忽略（通过 `(void)entrypoint` 抑制编译器警告）。

**第五步：提取引擎和 Messenger，构建 FlutterViewHostLinux**

```cpp
FlEngine* engine = fl_view_get_engine(view);
FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(engine);

try {
    auto* host = new FlutterViewHostLinux(view, engine, messenger, channelName);
    return std::unique_ptr<FlutterViewHost>(host);
} catch (...) {
    g_object_unref(view);
    return nullptr;
}
```

构造异常时手动 `g_object_unref(view)` 释放 GObject 引用，因为 `unique_ptr` 尚未接管所有权。

---

### 5.2 FlutterViewHostLinux

#### 构造函数 —— 浮动引用处理与通道注册

```cpp
FlutterViewHostLinux(FlView* view, FlEngine* engine,
                     FlBinaryMessenger* messenger,
                     const std::string& channelName)
    : m_view(view), m_engine(engine) {

    // fl_view_new 返回浮动引用，沉锚为常规引用
    if (m_view) {
        g_object_ref_sink(m_view);
        g_object_add_weak_pointer(G_OBJECT(m_view),
                                  reinterpret_cast<gpointer*>(&m_view));
    }
```

**GObject 浮动引用（Floating Reference）概念**：
- 新创建的 GObject 初始带有一个"浮动"引用
- 浮动引用的所有权在首次被父容器引用时自动沉锚（sink）
- 如果对象未添加父容器，浮动引用在最终 `unref` 时导致引用计数异常

`g_object_ref_sink` 将浮动引用转换为常规引用，明确定义所有权：即使 `embedInto` 被延迟调用或从未调用，对象的引用计数也是确定的。

`g_object_add_weak_pointer` 注册弱指针回调：当对象被销毁时，自动将 `m_view` 设为 `NULL`。这防止了析构顺序问题——如果 `m_view` 在 `FlutterViewHostLinux` 之前被意外销毁（例如父容器级联销毁），弱指针检测可以避免双重释放。

```cpp
    // 注册 MethodChannel
    g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
    m_channel = fl_method_channel_new(
        messenger, channelName.c_str(), FL_METHOD_CODEC(codec));
    if (m_channel) {
        fl_method_channel_set_method_call_handler(
            m_channel, methodCallHandler, this, nullptr);
    }

    // 持有引擎引用
    if (m_engine) g_object_ref(m_engine);
}
```

**与 Windows 的关键差异**：Linux 使用 Flutter 内置的 `FlStandardMethodCodec`，无需手动实现编解码。这得益于 Linux 嵌入 API 的成熟度更高。

#### createView → embedInto 流程

`createView()` 创建的是 **FlView（GtkGLArea）**——一个 OpenGL 渲染区域。`embedInto()` 将其嵌入 wxWidgets 的 GTK 容器中。但问题在于：

- **FlView 默认不可聚焦**：GtkGLArea 没有 `GTK_CAN_FOCUS` 标志
- **GtkFixed（wxPizza 的父类）不传播焦点**：即使手动设置 `GTK_CAN_FOCUS`，GtkFixed 的焦点链遍历也不会将其传递给子元素

**解决方案：GtkEventBox 包装**

```cpp
void embedInto(void* parentHandle) override {
    GtkWidget* parent = GTK_WIDGET(parentHandle);
    if (!parent || !m_view) return;

    GtkAllocation alloc;
    gtk_widget_get_allocation(parent, &alloc);
    int w = alloc.width > 1 ? alloc.width : 800;
    int h = alloc.height > 1 ? alloc.height : 600;

    if (!WX_IS_PIZZA(parent)) return;   // 必须是 wxPizza 容器

    GtkWidget* view_widget = GTK_WIDGET(m_view);
    wxPizza* pizza = WX_PIZZA(parent);

    // 清除上一次嵌入的 event box（防御性）
    if (m_event_box) {
        g_object_remove_weak_pointer(G_OBJECT(m_event_box),
                                     reinterpret_cast<gpointer*>(&m_event_box));
        gtk_container_remove(GTK_CONTAINER(m_event_box), view_widget);
        gtk_container_remove(GTK_CONTAINER(pizza), m_event_box);
        m_event_box = nullptr;
    }

    // 创建新的 GtkEventBox，设置为可聚焦
    m_event_box = gtk_event_box_new();
    g_object_add_weak_pointer(G_OBJECT(m_event_box),
                              reinterpret_cast<gpointer*>(&m_event_box));
    gtk_container_add(GTK_CONTAINER(m_event_box), view_widget);
    gtk_widget_set_can_focus(m_event_box, TRUE);
    gtk_widget_set_can_focus(view_widget, TRUE);

    pizza->put(m_event_box, 0, 0, w, h);

    gtk_widget_show_all(m_event_box);
    if (gtk_widget_get_realized(parent))
        gtk_widget_realize(view_widget);
    gtk_widget_grab_focus(view_widget);
}
```

**嵌入分层结构**：
```
wxPizza (GtkFixed 子类)
  └── GtkEventBox (可聚焦包装器)
        └── FlView (GtkGLArea，实际渲染)
```

**重复嵌入安全性**：如果 `m_event_box` 已存在（因重新布局或页面切换导致再次调用 `embedInto`），先移除弱指针、解绑 FlView、从 pizza 移除旧 event box，再创建新的。特别注意：
- 先移除 FlView 再销毁 event box——如果顺序颠倒，`gtk_widget_destroy` 会级联销毁 event box 的所有子元素，包括 FlView
- 先移除弱指针——防止旧 event box 的延迟析构回调误将新 `m_event_box` 置空

#### resize(int width, int height) —— 四步顺序（顺序至关重要）

```cpp
void resize(int width, int height) override {
    if (!m_view || !m_event_box) return;
    GtkWidget* parent = gtk_widget_get_parent(m_event_box);
    if (!parent || !WX_IS_PIZZA(parent)) return;
    if (width <= 0 || height <= 0) return;

    wxPizza* pizza = WX_PIZZA(parent);

    // 步骤1：更新 wxPizza 内部的子元素位置记录
    pizza->move(m_event_box, 0, 0, width, height);

    // 步骤2：分配 GTK 屏幕空间
    pizza->size_allocate_child(m_event_box, 0, 0, width, height);

    // 步骤3：设置 event_box 的尺寸请求
    gtk_widget_set_size_request(m_event_box, width, height);

    // 步骤4：设置 FlView 的尺寸请求
    gtk_widget_set_size_request(GTK_WIDGET(m_view), width, height);
}
```

**顺序为什么至关重要**：

1. `pizza->move` 首先调用——更新 wxPizza 内部数据结构中的子元素尺寸，确保后续 GTK 布局周期（`pizza_size_allocate`）读取到正确的尺寸值。

2. `pizza->size_allocate_child` 其次调用——实际分配 GTK 级别的屏幕空间给 event_box。这必须在尺寸请求设置之前完成，因为 GTK 的布局协商机制可能在 `set_size_request` 后立即触发重分配，而此时 `pizza->move` 的记录必须已经是最新的。

3. `gtk_widget_set_size_request(event_box)` 第三——设置 event_box 期望的尺寸。

4. `gtk_widget_set_size_request(view)` 最后——设置 FlView 自身期望的尺寸。

如果第 4 步在第 2 步之前执行，FlView 向父容器（event_box）报告新尺寸请求时，event_box 尚未获得新的 GTK 空间分配，导致 FlView 以旧尺寸渲染然后被拉伸——产生视觉闪烁。

#### invokeMethod —— 通过 FlMethodChannel 调用 Dart 方法

```cpp
void invokeMethod(const std::string& method,
                  const std::string& arguments) override {
    if (!m_channel) return;
    g_autoptr(FlValue) args = nullptr;

    // 自动检测整数参数
    if (!arguments.empty()) {
        errno = 0;
        char* end = nullptr;
        long long n = std::strtoll(arguments.c_str(), &end, 10);
        if (end && *end == '\0' && errno != ERANGE)
            args = fl_value_new_int(static_cast<int64_t>(n));
    }
    if (!args) args = fl_value_new_string(arguments.c_str());

    fl_method_channel_invoke_method(
        m_channel, method.c_str(), args,
        nullptr, nullptr, nullptr);
}
```

与 Windows 类似的整数自动检测。`fl_method_channel_invoke_method` 的最后三个参数（cancellable、callback、user_data）均为 `nullptr`，表示这是"发后不理"（fire-and-forget）调用，不等待 Dart 端响应。

#### setMethodCallHandler —— GAsyncReadyCallback 异步回复

```cpp
void setMethodCallHandler(MethodCallHandler h) override {
    m_handler = std::move(h);
}
```

实际处理在静态函数 `methodCallHandler` 中：

```cpp
static void methodCallHandler(FlMethodChannel* /*channel*/,
                              FlMethodCall* method_call,
                              gpointer user_data) {
    auto* self = static_cast<FlutterViewHostLinux*>(user_data);
    if (!self->m_handler) {
        fl_method_call_respond_not_implemented(method_call, nullptr);
        return;
    }

    const gchar* method = fl_method_call_get_name(method_call);
    FlValue* args_fl = fl_method_call_get_args(method_call);
    std::string args = flValueToString(args_fl);

    // 持有 method_call 引用，防止异步处理时被释放
    g_object_ref(method_call);
    auto replied = std::make_shared<bool>(false);

    Reply reply = [method_call, replied](const std::string& result) {
        if (*replied) return;
        *replied = true;

        g_autoptr(GError) error = nullptr;
        g_autoptr(FlValue) val = nullptr;

        // 自动检测整数回复
        if (!result.empty()) {
            errno = 0;
            char* end = nullptr;
            long long n = std::strtoll(result.c_str(), &end, 10);
            if (end && *end == '\0' && errno != ERANGE)
                val = fl_value_new_int(static_cast<int64_t>(n));
        }
        if (!val) val = fl_value_new_string(result.c_str());

        fl_method_call_respond_success(method_call, val, &error);
        g_object_unref(method_call);   // 释放我们持有的引用
    };

    try {
        self->m_handler(method ? method : "", args, reply);
    } catch (const std::exception& e) {
        if (!*replied) {
            *replied = true;
            g_autoptr(GError) error = nullptr;
            fl_method_call_respond_error(method_call, "EXCEPTION", e.what(), nullptr, &error);
            g_object_unref(method_call);
        }
    } catch (...) {
        if (!*replied) {
            *replied = true;
            g_autoptr(GError) error = nullptr;
            fl_method_call_respond_error(method_call, "EXCEPTION", "unknown error", nullptr, &error);
            g_object_unref(method_call);
        }
    }

    // 兜底：handler 未调用 reply 且未抛出异常
    if (!*replied) {
        *replied = true;
        fl_method_call_respond_not_implemented(method_call, nullptr);
        g_object_unref(method_call);
    }
}
```

**与 Windows 的关键差异**：Linux 方法使用 `g_object_ref(method_call)` 持有一个引用，允许 handler 异步回复（例如在后台线程完成工作后延迟调用 reply）。`shared_ptr<bool>` 防护与 Windows 相同。

**g_bytes 数据传递**：`flValueToString` 辅助函数处理所有 FlValue 类型到字符串的转换：

| FlValue 类型 | 转换结果 |
|-------------|---------|
| `FL_VALUE_TYPE_STRING` | UTF-8 字符串 |
| `FL_VALUE_TYPE_INT` | `std::to_string` |
| `FL_VALUE_TYPE_FLOAT` | `std::to_string` |
| `FL_VALUE_TYPE_BOOL` | `"true"` 或 `"false"` |
| `FL_VALUE_TYPE_NULL` | 空字符串 |

---

### 5.3 GObject 内存管理详解

GObject 是 GLib 的面向对象系统，其引用计数机制比标准 C++ 智能指针更精细，存在几个需要特别注意的概念。

#### 浮动引用（Floating Reference）

```cpp
// fl_view_new 返回的对象带有浮动引用
FlView* view = fl_view_new(project);

// 必须沉锚（sink），转换为常规引用
g_object_ref_sink(view);

// 现在 view 有一个常规引用，count = 1
// 如果在沉锚前调用 g_object_unref，会同时清除浮动引用
// 导致引用计数变为 0 → 销毁对象
```

**概念解释**：浮动引用是 GTK/GObject 中的所有权语义糖。当一个对象被创建时，它持有一个"浮动"的初始引用。这个引用的目的是被父容器在 `gtk_container_add` 时"沉锚"——父容器获取所有权，而不是使引用计数增加到 2。

在我们的场景中，`FlView` 可能不会立即被添加到父容器中（`embedInto` 可能延迟调用）。因此必须手动调用 `g_object_ref_sink` 获取明确的常规引用。

#### 弱指针（Weak Pointer）

```cpp
g_object_add_weak_pointer(G_OBJECT(m_view),
                          reinterpret_cast<gpointer*>(&m_view));
```

注册弱指针后，当 `m_view` 指向的 GObject 被销毁（引用计数归零）时，GLib 自动将 `&m_view` 处的指针置为 `NULL`。这解决了"对象在我不知道的情况下被销毁了"的问题——典型场景是父容器级联销毁。

析构时的安全清理：

```cpp
~FlutterViewHostLinux() override {
    // ...
    if (m_view) {
        g_object_remove_weak_pointer(G_OBJECT(m_view),
                                     reinterpret_cast<gpointer*>(&m_view));
        g_clear_object(&m_view);   // unref + 置 NULL
    }
    if (m_event_box) {
        g_object_remove_weak_pointer(G_OBJECT(m_event_box),
                                     reinterpret_cast<gpointer*>(&m_event_box));
        // 不 unref event_box——pizza 容器持有其所有权
    }
}
```

**event_box 的特殊处理**：不在析构函数中释放 event_box 的引用——它由 wxPizza 容器持有。只移除弱指针。如果手动 `unref` event_box，会导致 wxPizza 持有悬空指针（wxPizza 仍认为自己拥有该子元素）。

#### g_clear_object 宏

`g_clear_object` 等价于：
```cpp
if (ptr) {
    g_object_unref(ptr);
    ptr = nullptr;
}
```

确保安全地释放引用并防止悬空指针。

#### 无管理状态下的潜在问题

在没有正确管理引用计数的情况下：
- **双重释放**：如果 `g_object_ref_sink` 后未正确 `unref`，再被容器释放导致 use-after-free
- **use-after-free**：弱指针未注册时，引用计数归零后仍访问对象
- **泄漏**：`g_object_ref` 后永远不调用 `g_object_unref`

---

### 5.4 Linux 特有陷阱与解决方案

#### 陷阱 #1：GTK 分配尺寸可能过时

`GetClientSize()` 在 wxGTK 中查询的是 GTK 的 `GtkAllocation`，但如果 GTK 尚未处理尺寸分配，这个值可能是过时的。`onSize` 事件处理中使用 `event.GetSize()` 而非 `GetClientSize()`：

```cpp
void FlutterPanel::onSize(wxSizeEvent& event) {
    // 使用 event.GetSize() 而非 GetClientSize()——在 wxGTK 中
    // GetClientSize() 查询 GTK 分配值，可能过时。
    wxSize sz = event.GetSize();
    if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
        m_view->resize(sz.GetWidth(), sz.GetHeight());
}
```

这是 Linux 平台上一个实际的 bug 修复——notebook 页面切换时 GTK 尺寸事件可能先于分配到达。

#### 陷阱 #2：GtkEventBox 是键盘焦点工作的前提

没有 GtkEventBox 包装的 FlView 无法接收键盘焦点。GtkGLArea 本身不默认支持焦点，而 GtkFixed 容器不会向子元素传播焦点。使用 GtkEventBox 并在其上设置 `GTK_CAN_FOCUS` 是经过验证的解决方案。

#### 陷阱 #3：resize 中的顺序依赖

四个步骤的顺序经过实际验证才能避免尺寸闪烁。顺序改变会导致以下症状：
- `pizza->move` 在 `size_allocate_child` 之后 → 下一次 GTK 布局周期使用旧尺寸
- `set_size_request(view)` 在 `size_allocate_child` 之前 → FlView 报告新尺寸时 event_box 空间不足
- 跳过 `size_allocate_child` → GTK 延迟分配，导致渲染滞后

#### 陷阱 #4：入口点不可自定义

`fl_view_new` 不提供入口点参数，始终运行 Dart 的 `main()` 函数。这与 Windows（通过 `FlutterDesktopEngineRun` 的 `entry_point` 参数）和 macOS（通过 `runWithEntrypoint:`）不同。如果需要在 Linux 上运行非 main 入口点，需要使用 `fl_engine_new_with_project` 等更低级别的 API。

#### 陷阱 #5：wxPizza 的 put 方法

`pizza->put(child, x, y, w, h)` 是 wxPizza（wxGTK 内部类）特殊方法，同时设置位置和尺寸。使用正确的方法很重要——直接向 GtkFixed 添加子元素而不通过 wxPizza 会导致 wxWidgets 布局系统失去对该子元素的跟踪。

---

## 第6章：macOS 平台实现

### 6.1 FlutterEngineHostMacOS

macOS 的实现使用 Objective-C++（`.mm` 文件），直接桥接 Cocoa 和 FlutterMacOS 框架。

#### NSMutableArray 引擎生命周期管理

```objc
class FlutterEngineHostMacOS : public FlutterEngineHost {
    FlutterDartProject* project = nil;
    NSMutableArray<FlutterEngine*>* engines;

public:
    FlutterEngineHostMacOS() : engines([[NSMutableArray alloc] init]) {}
```

使用 `NSMutableArray` 管理多个引擎实例——每个视图创建一个新引擎，引擎数组持有强引用，在 `stop()` 时统一关闭和释放：

```objc
void stop() override {
    for (FlutterEngine* e in engines) [e shutDownEngine];
    [engines removeAllObjects];
    project = nil;
}
```

**架构决策**：与 Windows 上"引擎内嵌于视图控制器"不同，macOS 的引擎是独立对象，视图控制器持有对引擎的引用。`NSMutableArray` 明确管理这种一对多的关系。

#### start()

```objc
bool start() override {
    project = [[FlutterDartProject alloc] initWithPrecompiledDartBundle:nil];
    return true;
}
```

`initWithPrecompiledDartBundle:nil` 使用默认 bundle 路径（App.framework 内的 Flutter 资源）。传入 `nil` 时，Flutter 引擎从 main bundle 中的 `flutter_assets` 目录加载资源。与 Windows/Linux 不同，**macOS 不需要显式的路径解析**——因为 FlutterMacOS.framework 和 App.framework 已嵌入 app bundle 的标准位置。

**为何 `createFlutterEngine(assetsPath, icuDataPath)` 在 macOS 上忽略参数：** 工厂函数签名接受两个路径参数（出于跨平台统一接口的考虑），但 macOS 实现中两个参数均为 `/* unnamed */`——未命名、未使用。因为：
1. `assetsPath` 不必要——`FlutterDartProject initWithPrecompiledDartBundle:nil` 自动从 `[NSBundle mainBundle]` 定位 `flutter_assets`
2. `icuDataPath` 不必要——`icudtl.dat` 已嵌入 FlutterMacOS.framework 的 `Resources/` 目录，引擎自动发现
3. Framework bundle 的封装性意味着所有资源路径在编译时确定，无需运行时显式指定

这与 Windows（从 `GetModuleFileNameW` 推导路径）和 Linux（从 `/proc/self/exe` 推导路径）形成对比——它们需要运行时路径推导，尽管它们的工厂函数参数同样未被使用。

#### createView()

```objc
std::unique_ptr<FlutterViewHost> createView(
    const std::string& entrypoint,
    const std::string& channelName) override {

    FlutterEngine* engine = [[FlutterEngine alloc] initWithName:@"embed"
                                                         project:project
                                          allowHeadlessExecution:YES];

    NSString* nsEntry = nil;
    if (!entrypoint.empty() && entrypoint != "main")
        nsEntry = [NSString stringWithUTF8String:entrypoint.c_str()];

    if (![engine runWithEntrypoint:nsEntry]) {
        return nullptr;
    }

    auto* view = new FlutterViewHostMacOS(engine, channelName);
    [engines addObject:engine];   // 持有引擎引用
    return std::unique_ptr<FlutterViewHost>(view);
}
```

**与 Windows/Linux 的关键差异**：
1. `allowHeadlessExecution:YES` 允许引擎在无视图的情况下保持运行——这在多视图场景中很关键
2. `runWithEntrypoint:` 支持指定入口点（与 Windows 相同，与 Linux 不同）
3. `nsEntry` 仅在入口点非空且非 "main" 时才设置——"main" 是默认入口点，传 `nil` 等价

---

### 6.2 FlutterViewHostMacOS

#### createView — FlutterViewController 创建

```objc
FlutterViewHostMacOS(FlutterEngine* engine, const std::string& channelName) {
    controller = [[FlutterViewController alloc] initWithEngine:engine
                                                        nibName:nil bundle:nil];
    controller.mouseTrackingMode = kFlutterMouseTrackingModeInActiveApp;
    controller.view.wantsLayer = YES;
    engine.viewController = controller;
    channel = [FlutterMethodChannel
        methodChannelWithName:[NSString stringWithUTF8String:channelName.c_str()]
             binaryMessenger:engine.binaryMessenger];
}
```

**关键配置项**：

1. `mouseTrackingMode = kFlutterMouseTrackingModeInActiveApp`：启用在活动应用中的鼠标跟踪。确保 Flutter 的鼠标悬停、拖拽和光标样式 API 在 macOS 上正常工作。

2. `view.wantsLayer = YES`：启用 Core Animation 层支持。在 macOS 10.14+ 中，这是视图渲染的基础要求，确保 Flutter 渲染管线与 AppKit 的图层合成正确对接。

3. `engine.viewController = controller`：将视图控制器注册到引擎。这建立了双向引用——引擎可以通知视图控制器状态变化，视图控制器可以触发引擎操作。

4. `FlutterMethodChannel` 直接从引擎的 `binaryMessenger` 创建，而不经过中间层。

**autoresizingMask = NSViewWidthSizable | NSViewHeightSizable**

```objc
void embedInto(void* parentView) override {
    NSView* fv = controller.view;
    NSView* cv = (__bridge NSView*)parentView;
    if (!fv || !cv) return;
    fv.frame = cv.bounds;
    fv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [cv addSubview:fv];
}
```

这是 macOS 实现的**核心设计优势**：`autoresizingMask` 设置为 `NSViewWidthSizable | NSViewHeightSizable` 后，当父视图尺寸改变时，Cocoa 的自动布局系统会自动调整 Flutter 视图的 frame。这意味着：

```objc
void resize(int, int) override {}   // 空实现！
```

**不需要**手动 resize。这与 Windows（需要 `SetWindowPos`）和 Linux（需要四步 GTK 操作）形成鲜明对比。macOS 的自动布局基础设施直接消除了整个 resize 路径的复杂性。

#### invokeMethod —— Objective-C 异步 API

```objc
void invokeMethod(const std::string& method,
                  const std::string& arguments) override {
    if (!channel) return;
    [channel invokeMethod:[NSString stringWithUTF8String:method.c_str()]
                arguments:decodeArg(arguments)];
}
```

`decodeArg` 辅助函数处理类型转换：

```objc
static id decodeArg(const std::string& s) {
    if (s.empty()) return nil;
    char* end = nullptr;
    long val = strtol(s.c_str(), &end, 10);
    if (end && *end == '\0') return @(val);   // 整数
    return [NSString stringWithUTF8String:s.c_str()];  // 字符串
}
```

`@(val)` 是 Objective-C 的装箱语法——将 C 整数包装为 `NSNumber` 对象。这与 Dart 端的类型检测兼容，确保 `invokeMethod<int>` 收到正确的数值类型。

#### setMethodCallHandler —— Block 回调

```objc
void setMethodCallHandler(MethodCallHandler h) override {
    handler = h;
    [channel setMethodCallHandler:^(
        FlutterMethodCall* call, FlutterResult fResult) {
        if (handler) {
            std::string args = encodeArg(call.arguments);
            FlutterViewHost::Reply reply = [fResult](const std::string& out) {
                fResult(decodeArg(out));
            };
            handler([call.method UTF8String], args, reply);
        } else {
            fResult(FlutterMethodNotImplemented);
        }
    }];
}
```

`FlutterResult` 是一个 ObjC block 类型（`typedef void (^FlutterResult)(id _Nullable result)`）。我们的 `Reply` lambda 捕获该 block，在 C++ handler 完成时回调。

**与 Windows/Linux 的重大差异**：FlutterMacOS 的 `setMethodCallHandler` API 是天然异步的——`FlutterResult` block 可以在任意线程上调用，Flutter 引擎内部处理线程安全。这使得代码比 Windows（需要手动实现回调防护）和 Linux（需要 `g_object_ref` 管理生命周期）更简洁。

---

### 6.3 macOS 特有优势

#### 无需手动 resize

`autoresizingMask` 使 macOS 成为三个平台中 resize 实现最简单的。Cocoa 的自动布局系统在父视图边界改变时自动调用 `-[NSView setFrame:]`，无需任何 C++ 端介入。

#### 无焦点管理问题

macOS 的 `NSWindow` 焦点管理基于响应链（responder chain），Flutter 视图作为 NSView 自然参与其中。不像 Windows 需要覆盖 `MSWGetFocusHWND()`、`ContainsHWND()` 和 `MSWShouldPreProcessMessage()`，也不像 Linux 需要 GtkEventBox 包装。只需调用 `[window makeFirstResponder:view]` 即可转移焦点。

**macOS 响应链（Responder Chain）机制详解：**

macOS 的事件分发不依赖 HWND 和父子窗口关系，而是通过 Cocoa 的响应链：

1. `NSWindow` 收到键盘事件后，调用 `-[NSWindow sendEvent:]`
2. `sendEvent:` 将事件发送给 `firstResponder`（当前焦点视图）
3. 如果 `firstResponder` 不处理，事件沿响应链向上传递：view → superview → ... → window → window controller → application
4. Flutter 的 `FlutterViewController.view` 是一个标准的 NSView，它实现了 `acceptsFirstResponder`（返回 YES），因此自然可以成为 `firstResponder`
5. 用户点击 Flutter 视图时，Cocoa 自动将其设为 `firstResponder`；也可以通过 `[window makeFirstResponder:flutterView]` 程序化设置

这与 Windows 的根本区别在于：
- **Windows**: `::SetFocus(childHwnd)` 将焦点交给子 HWND，但父窗口的 WndProc 可能通过 `PreProcessMessage` 拦截消息。焦点变化需要通过 `WM_SETFOCUS`/`WM_KILLFOCUS` 协调，存在竞态。
- **macOS**: 响应链是 NSView 层次结构的自然属性，不需要窗口过程钩子。事件从 `firstResponder` 开始，自动沿视图树向上冒泡，没有消息队列的拦截问题。

因此在 macOS 上，`FlutterPanel::focus()` 的实现在 `FlutterViewHostMacOS` 中只需要一行：

```objc
[controller.view.window makeFirstResponder:controller.view];
```

#### ObjC API 成熟度高于 Windows/Linux C API

FlutterMacOS 的 Objective-C API 是 Flutter 团队最成熟的嵌入接口：
- `FlutterMethodChannel` 内置 StandardMethodCodec 支持（与 Linux 相同，Windows 需要手动实现）
- `FlutterEngine` 类封装了完整的引擎生命周期
- Block 回调语义明确，线程安全由框架保证
- `FlutterViewController` 内置视图管理，包括鼠标跟踪

#### Framework 打包（App.framework + FlutterMacOS.framework）

macOS 应用使用 framework bundle 结构，Flutter 引擎和 Dart 代码被打包为标准 macOS framework：
- `App.framework` 包含 AOT 编译的 Dart 代码
- `FlutterMacOS.framework` 包含 Flutter 引擎二进制
- 资源（`flutter_assets`、`icudtl.dat`）嵌入在 framework 的 `Resources` 目录中

这意味着与 Windows/Linux 相比，**不需要任何路径解析代码**。Cocoa 的 `NSBundle` 系统自动定位 framework 内的资源。

---

### 平台差异速查表

| 特性 | Windows | Linux | macOS |
|------|---------|-------|-------|
| 引擎管理 | 每视图一个引擎（嵌入在 ViewController 中） | 每视图一个引擎（FlView 持有 FlEngine） | 每视图一个引擎（NSMutableArray 管理） |
| 路径解析 | GetModuleFileNameW | /proc/self/exe | NSBundle（自动） |
| AOT 库 | flutter_app.dll | libflutter_app.so（需覆盖默认 libapp.so） | App.framework |
| resize 实现 | SetWindowPos | GTK 四步操作 | 空实现（autoresizingMask） |
| MethodCodec | 手动实现 StandardMethodCodec | FlStandardMethodCodec（内置） | FlutterMethodChannel（内置） |
| 焦点管理 | 三步 MSW 覆盖 | GtkEventBox + grab_focus | makeFirstResponder |
| 键盘修复 | MSWShouldPreProcessMessage | 无特殊处理 | 无特殊处理 |
| 入口点支持 | 支持（FlutterDesktopEngineRun 参数） | 不支持（始终 main()） | 支持（runWithEntrypoint:） |
| 浮动引用 | 不适用 | g_object_ref_sink | ARC 自动管理 |
| resize 是否需要 | 是 | 是 | 否 |

### 键盘问题的完整因果分析（Windows）

### 修复前的完整消息流

```
步骤1：用户按下键盘上的 'A' 键
  → Windows 内核将 WM_KEYDOWN (VK_A) 放入应用消息队列

步骤2：消息循环 (wxEventLoop) 取出 WM_KEYDOWN
  → msg.hwnd = Flutter 子窗口

步骤3：wxEventLoop::PreProcessMessage(msg)
  → 从 msg.hwnd 开始向上遍历 WS_CHILD 父链
  → msg.hwnd = Flutter 子窗口 → GetParent → FlutterPanel
  → FlutterPanel 是 wxWindow → 调用 MSWTranslateMessage(&msg)

步骤4：wxWindow::MSWTranslateMessage(&msg)
  → 调用 ::TranslateMessage(&msg)
  → Windows 内部：根据 VK_A + 键盘状态生成 WM_CHAR ('a')
  → WM_CHAR 被投递到 Flutter 子窗口的消息队列
  → ::TranslateMessage 返回 1（非零 = 已翻译）

步骤5：PreProcessMessage 检查返回值
  → MSWTranslateMessage 返回 true → 消息被认为已处理
  → WM_KEYDOWN 被标记为 "processed"
  → PreProcessMessage 返回 true

步骤6：消息循环检查 processed 标志
  → 已标记为 processed → 跳过 ::DispatchMessage
  → WM_KEYDOWN 永远不到达 Flutter 的 WndProc

步骤7：稍后，消息循环取出 WM_CHAR
  → ::DispatchMessage → WM_CHAR 到达 Flutter WndProc
  → Flutter KeyboardManager::HandleMessage(WM_CHAR)
  → 查找 pending key-down → 不存在（WM_KEYDOWN 被吞了）
  → 丢弃此 WM_CHAR → 无字符输出
```

### 修复后的完整消息流

```
步骤1：用户按下键盘上的 'A' 键
  → Windows 内核将 WM_KEYDOWN (VK_A) 放入应用消息队列

步骤2：消息循环 (wxEventLoop) 取出 WM_KEYDOWN
  → msg.hwnd = Flutter 子窗口

步骤3：wxEventLoop::PreProcessMessage(msg)
  → 检查 msg.hwnd 对应的 wxWindow
  → 找到 FlutterPanel，调用 MSWShouldPreProcessMessage(&msg)
  → FlutterPanel 检测到 msg.hwnd == Flutter 子窗口 HWND
  → 返回 false → PreProcessMessage 跳过整个父链遍历

步骤4：消息循环调用原始流程
  → ::TranslateMessage(&msg)
  → Windows 内部：生成 WM_CHAR ('a')，投递到消息队列
  → ::DispatchMessage(&msg)
  → WM_KEYDOWN 到达 Flutter WndProc
  → Flutter KeyboardManager 记录 pending key-down (VK_A)

步骤5：稍后，消息循环取出 WM_CHAR
  → 同样的 MSWShouldPreProcessMessage 返回 false
  → ::DispatchMessage → WM_CHAR 到达 Flutter WndProc
  → Flutter KeyboardManager::HandleMessage(WM_CHAR)
  → 找到匹配的 pending key-down (VK_A)
  → 提交字符 'a' → 输入框中出现 'a' ✓
```

## 第七章：C++ 开发者指南

### 7.1 添加新的 Flutter Panel（分步教程）

向 OrcaSlicer 添加一个 Flutter 面板需要六个步骤。下面以添加"设备监控面板"为例，完整展示每个步骤的代码模式。

#### Step 1：在 Dart 端定义入口函数（entrypoint）

在 `flutter_app/lib/main.dart` 中定义带 `@pragma('vm:entry-point')` 注解的顶级函数：

```dart
// flutter_app/lib/main.dart

import 'package:flutter/material.dart';

@pragma('vm:entry-point')
void main() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void homeMain() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void prepareMain() => runApp(const PreparePanelApp());

// 新增设备监控入口
@pragma('vm:entry-point')
void monitorMain() => runApp(const MonitorPanelApp());

class MonitorPanelApp extends StatelessWidget {
  const MonitorPanelApp({super.key});
  @override
  Widget build(BuildContext context) =>
      const PanelApp('Monitor Panel', Colors.orange, 'snapmaker/monitor');
}
```

**关键规则：**
- 入口必须是顶级函数（top-level）或静态方法，不能是实例方法。
- `@pragma('vm:entry-point')` 是必须的——它告诉 Flutter 的 AOT 编译器保留该函数的符号，防止树摇优化（tree-shaking）将其移除。
- `main()` 是默认入口，如果不指定 entrypoint 名称则自动调用。
- 自定义入口（如 `monitorMain`）由 C++ 端通过名称字符串指定。
- 入口函数通常只做一件事：调用 `runApp()` 并传入一个 Widget。

#### Step 2：创建 FlutterPanel 实例

在 C++ 端（通常是 `MainFrame.cpp`），创建 `FlutterPanel` 并将其挂载到 wxWidgets 的窗口树中：

```cpp
// MainFrame.hpp 成员变量声明
#include "Flutter/FlutterPanel.hpp"

class MainFrame : public wxFrame {
    // ...
    FlutterPanel*  m_monitor_flutter_panel{nullptr};
    // ...
};

// MainFrame.cpp 初始化代码
#include "Flutter/FlutterPanel.hpp"

// 在创建 tab 的函数中：
m_monitor_flutter_panel = new FlutterPanel(m_tabpanel);
m_tabpanel->AddPage(m_monitor_flutter_panel,
                     _L("Monitor"),
                     std::string("tab_monitor_active"),
                     std::string("tab_monitor_active"),
                     false);
```

**关键点：**
- `FlutterPanel` 继承自 `wxWindow`，构造时需要传入父窗口（通常是 notebook 页面）。
- 构造时自动设置黑色背景 (`SetBackgroundColour(*wxBLACK)`)，避免闪白屏。
- 使用 `wxFULL_REPAINT_ON_RESIZE | wxCLIP_CHILDREN` 样式。
- 构造时即绑定 `wxEVT_SIZE`、`wxEVT_SHOW`、`wxEVT_SET_FOCUS` 事件。

#### Step 3：启动 View

```cpp
// 在 CallAfter 或合适的时机调用（确保面板已布局完毕）
CallAfter([this] {
    if (!m_monitor_flutter_panel->startView(
            m_flutter_engine,        // engine 指针
            "monitorMain",           // entrypoint 名称（对应 Dart 中的 @pragma 函数）
            "snapmaker/monitor",     // MethodChannel 名称
            d.handler())) {          // 方法回调处理器
        BOOST_LOG_TRIVIAL(error) << "[Flutter] Monitor panel startView failed";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "[Flutter] Monitor panel started";
});
```

**参数说明：**
- `entrypoint`: 对应 Dart 端 `@pragma('vm:entry-point')` 标注的函数名。传空字符串表示使用默认 `main()`。
- `channelName`: MethodChannel 名称，Dart 端使用相同的名称创建 `MethodChannel('snapmaker/monitor')`，双方通过此名称匹配通信。
- `handler`: 处理 Dart→C++ 方法调用的回调函数。由 `Dispatcher` 生成。

**`startView()` 内部流程：**
1. 调用 `engine->createView(entrypoint, channelName)` 创建平台相关的 ViewController。
2. 如果提供了 handler，立即通过 `m_view->setMethodCallHandler()` 注册。
3. 调用 `tryEmbed()` 尝试将 Flutter 子窗口嵌入到 wxWindow 中（如果面板已有有效尺寸）。
4. 如果嵌入成功且尺寸有效，立即调用 `m_view->resize()` 调整到正确大小。

#### Step 4：注册 MethodChannel 处理器

使用 `Dispatcher` 类（链式 API）注册 Dart→C++ 的方法处理函数：

```cpp
Dispatcher d;
d.on("getStatus", [this](auto args, auto reply) {
    // args: Dart 端传来的 JSON 字符串参数
    // reply: 回传结果的函数，必须调用且只能调用一次
    std::string status = get_device_status();
    reply(status);
})
.on("startJob", [this](auto args, auto reply) {
    auto job_params = nlohmann::json::parse(args);
    bool success = start_print_job(job_params);
    reply(success ? "ok" : "failed");
})
.on("getTemperature", [](auto args, auto reply) {
    // 无状态的简单处理器
    reply("220");
});
```

**Dispatcher 工作原理：**
- `on(method, fn)` 将方法名映射到处理函数，返回自身引用支持链式调用。
- `handler()` 返回一个 `MethodCallHandler` lambda，内部维护一个 `unordered_map<string, function>` 查找表。
- 如果方法名未注册，自动回复空字符串（不会抛异常也不会挂起 Dart 侧的 Future）。
- 处理函数若抛出异常，异常信息会被自动捕获并编码为错误回复发送给 Dart 端。

#### Step 5：添加到 Notebook/Sizer

```cpp
// 方式一：添加到 wxNotebook（标签页导航）
m_tabpanel->AddPage(m_monitor_flutter_panel,
                     _L("Monitor"),           // 标签页标题
                     std::string("tab_active"),
                     std::string("tab_active"),
                     false);                   // 不选中

// 方式二：添加到 wxSizer（用于 dialog 等场景）
auto* sizer = new wxBoxSizer(wxVERTICAL);
sizer->Add(m_monitor_flutter_panel, 1, wxEXPAND);
SetSizer(sizer);
```

#### Step 6：处理面板生命周期

在 Notebook 标签切换时通知 Flutter 面板激活/非激活状态：

```cpp
// MainFrame.cpp 中 Notebook 页面切换的处理
void MainFrame::onTabChanged(wxBookCtrlEvent& event) {
    int sel = event.GetSelection();
    int old = event.GetOldSelection();
    
    // 离开监控面板 — 通知 Dart 侧进入非活跃状态
    if (old == tpMonitor && m_monitor_flutter_panel && m_monitor_flutter_panel->view()) {
        m_monitor_flutter_panel->view()->invokeMethod("onPageState", "inactive");
    }
    
    // 进入监控面板 — 通知 Dart 侧进入活跃状态
    if (sel == tpMonitor && m_monitor_flutter_panel && m_monitor_flutter_panel->view()) {
        m_monitor_flutter_panel->view()->invokeMethod("onPageState", "active");
    }
    
    event.Skip();
}

// 应用退出时清理引擎
void MainFrame::shutdown(bool isRecreate) {
    // ... 其他清理代码 ...
    if (m_flutter_engine) {
        m_flutter_engine->stop();
        delete m_flutter_engine;
        m_flutter_engine = nullptr;
    }
}
```

### 7.2 MethodChannel 通信模式

#### C++ 调用 Dart 方法：`invokeMethod`

```cpp
// 基础调用（无参数，无返回值）
m_view->invokeMethod("refreshUI", "");

// 带参数调用（参数为 JSON 字符串）
m_view->invokeMethod("updateConfig", R"({"speed": 100, "temp": 220})");

// 带整型参数（自动检测数字字符串并编码为 int，而非 string）
m_view->invokeMethod("setLayer", "5");

// 使用 nlohmann::json 构建参数
nlohmann::json args;
args["printer_name"] = "Snapmaker J1";
args["status"] = "printing";
m_view->invokeMethod("onStatusChanged", args.dump());
```

**平台实现差异：**
- **Windows**: 使用手动实现的 `StandardMethodCodec` 编码。自动检测参数字符串是否为整数，如是则发 int32/int64 类型码，否则发 string。`encodeMethodCall()` 将方法名和参数分别用 `writeValue()` 编码后通过 `FlutterDesktopMessengerSend` 发送。
- **macOS**: 使用 `[FlutterMethodChannel invokeMethod:arguments:]`，参数自动通过 `decodeArg()` 判断：空字符串→nil，纯数字字符串→NSNumber，其他→NSString。
- **Linux**: 使用 `fl_method_channel_invoke_method()`，参数通过 `flValueToString()` 转换。同样自动检测整数字符串并编码为 `FL_VALUE_TYPE_INT`。

#### Dart→C++ 调用处理：Dispatcher 链模式

```cpp
// 完整示例：注册多个处理器并处理异步回复
Dispatcher d;
d.on("fetchData", [this](auto args, auto reply) {
    // 异步操作：存储 reply 稍后使用
    m_pending_reply = std::make_shared<FlutterViewHost::Reply>(std::move(reply));
    start_async_fetch([this](std::string result) {
        if (m_pending_reply) (*m_pending_reply)(result);
    });
})
.on("saveConfig", [this](auto args, auto reply) {
    bool ok = save_config_to_file(args);
    reply(ok ? "success" : "error: write failed");
})
.on("ping", [](auto args, auto reply) {
    // 最简单的处理器
    reply("pong");
});

// 注册到 panel
m_panel->setHandler(d.handler());
```

#### 异步回复：存储 Reply 对象

```cpp
// 场景：需要等待后台线程结果后再回复 Dart 侧
class MyPanel {
    std::shared_ptr<FlutterViewHost::Reply> m_pending;

    void onMethodCall(std::string args, FlutterViewHost::Reply reply) {
        m_pending = std::make_shared<FlutterViewHost::Reply>(std::move(reply));
        // 在后台线程完成时调用 (*m_pending)(result);
    }

    void onWorkerDone(std::string result) {
        if (m_pending) (*m_pending)(result);
        m_pending.reset();
    }
};
```

**重要安全规则：**
- Reply 对象必须恰好调用一次。如果处理器存储了 reply 稍后使用但从未调用，Dart 侧的 Future 会永久挂起。
- 在 Windows 实现中，如果 handler 未抛异常但也没有调用 reply，框架会自动发送空回复防止挂起。
- 在 Linux 实现中，同样有未回复保护：未调用 reply 的处理器会自动回复 `not_implemented`。
- 线程安全：所有 Flutter API 调用必须在主线程执行，不可在 worker 线程中调用 `reply()` 或 `invokeMethod()`。

#### 错误处理约定

```cpp
// C++ 侧：抛出标准异常，Dispatcher 会自动捕获并编码
d.on("riskyOperation", [](auto args, auto reply) {
    if (args.empty()) {
        throw std::invalid_argument("args must not be empty");
    }
    // 正常处理
    reply("done");
});

// Dart 侧接收错误
try {
    await channel.invokeMethod('riskyOperation', '');
} on PlatformException catch (e) {
    print('Error: ${e.message}'); // "args must not be empty"
}
```

**平台错误编码实现：**
- **Windows**: `encodeError(code, message)` 生成 `[0x01, code_string, message_string, null]`。
- **macOS**: 通过 `FlutterMethodNotImplemented` 或 `FlutterError` 传递。
- **Linux**: `fl_method_call_respond_error()` 传递错误码和消息。

#### JSON 参数格式（与 nlohmann::json 集成）

```cpp
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// 解析 Dart 传来的复杂参数
d.on("processObject", [](auto args, auto reply) {
    try {
        json j = json::parse(args);
        std::string name = j["name"];
        int count = j["count"];
        double value = j.value("value", 0.0);
        
        json result;
        result["processed"] = true;
        result["name_upper"] = name;
        result["count_doubled"] = count * 2;
        reply(result.dump());
    } catch (const json::exception& e) {
        reply(std::string("JSON error: ") + e.what());
    }
});
```

**注意：** MethodChannel 传递的始终是字符串。两端需要对复杂数据结构进行 JSON 序列化/反序列化。在 Dart 端，`channel.invokeMethod` 可以传递可 JSON 序列化的对象，Flutter 框架会自动编码；在 C++ 端，接收到的也是编码后的字符串。

### 7.3 焦点和输入管理

#### 焦点流转路径

```
Notebook 标签页切换
    → FlutterPanel::SetFocus() 被调用
        → [Windows] MSWGetFocusHWND() 返回 Flutter 子窗口 HWND
            → wxWindow::SetFocus() 直接调用 ::SetFocus(childHwnd)
        → [macOS/Linux] wxWindow::SetFocus() + m_view->focus()
            → 手动将焦点转移给 Flutter 子视图
```

#### 为什么 AcceptsFocus() 返回 false

```cpp
// FlutterPanel.hpp
bool AcceptsFocus() const override { return false; }
```

**原因：** FlutterPanel 本身只是一个透明容器，实际接收键盘/鼠标输入的是 Flutter 子窗口。如果 `AcceptsFocus()` 返回 true，wxWidgets 的 `MSWWindowProc` 在处理 `WM_LBUTTONDOWN` 时会调用 `::SetFocus(panelHwnd)`，导致焦点被面板本身截获而非传递给 Flutter 子窗口。返回 false 阻止了这种自动焦点抢占行为。

#### 平台差异

**Windows：**

```cpp
// MSWGetFocusHWND: 告诉 wxWidgets 真正的焦点目标是 Flutter 子窗口 HWND
WXHWND FlutterPanel::MSWGetFocusHWND() const {
    if (m_view) {
        void* childHwnd = m_view->nativeHandle();
        if (childHwnd) return (WXHWND)childHwnd;
    }
    return GetHWND();
}

// ContainsHWND: 告诉 wxWidgets 某个 HWND 是否属于本面板
bool FlutterPanel::ContainsHWND(WXHWND hWnd) const {
    if (m_view) {
        void* childHwnd = m_view->nativeHandle();
        if (childHwnd && hWnd == (WXHWND)childHwnd) return true;
    }
    return false;
}
```

**Linux：**

```cpp
void FlutterPanel::SetFocus() {
    wxWindow::SetFocus();        // 先调用 wx 的标准焦点设置
    if (m_view) m_view->focus(); // 再手动抓取 Flutter widget 的焦点
}
```

Linux 实现中使用 `gtk_widget_grab_focus(GTK_WIDGET(m_view))` 来触发 GTK 焦点链。由于 `GtkGLArea`（FlView 的底层实现）本身不接受焦点，Linux 端需要在 FlView 外部包裹一个 `GtkEventBox`（参见 ADR-5）来参与 GTK 的焦点传播。

**macOS：**

```cpp
void focus() override {
    if (controller.view.window)
        [controller.view.window makeFirstResponder:controller.view];
}
```

macOS 使用 Cocoa 的响应链（responder chain）机制，通过 `makeFirstResponder:` 将键盘焦点交给 Flutter 视图。

#### Windows 键盘输入特殊处理

```cpp
// MSWShouldPreProcessMessage: 关键方法，防止 wxWidgets 的
// PreProcessMessage 消费掉 Flutter 子窗口的键盘消息
bool FlutterPanel::MSWShouldPreProcessMessage(WXMSG* msg) {
    if (m_view) {
        HWND child = (HWND)m_view->nativeHandle();
        if (child && msg->hwnd == child)
            return false;  // 跳过 wxWidgets 预处理，让原生消息循环处理
    }
    return wxWindow::MSWShouldPreProcessMessage(msg);
}
```

**为什么需要这个重写？**

wxWidgets 的 `PreProcessMessage` 方法会从消息目标 HWND 向上遍历父窗口链，找到 `FlutterPanel` 后调用 `MSWTranslateMessage`（内部调用 `::TranslateMessage`），后者会将 `WM_KEYDOWN` 消费掉并生成 `WM_CHAR`。这导致 Flutter 的 `KeyboardManager` 收不到 `WM_KEYDOWN` 却收到了 `WM_CHAR`，无法正确配对处理键盘输入。

返回 `false` 跳过整个预处理链，让原生消息循环的 `::TranslateMessage` + `::DispatchMessage` 按正确顺序将 `WM_KEYDOWN` 和随后的 `WM_CHAR` 都送达 Flutter 子窗口。

### 7.4 生命周期管理

#### 生命周期全景图

```
应用启动                    用户操作                     应用退出
   │                          │                            │
   ▼                          ▼                            ▼
createFlutterEngine()   切换到 Flutter 页              shutdown()
   │                          │                            │
   ▼                          ▼                            ▼
engine->start()       tryEmbed() + resize()       engine->stop()
   │                          │                    delete engine
   ▼                          ▼
(等待面板可见)        视图活跃状态
                        invokeMethod()
                        ("onPageState", "active")
   │                          │
   ▼                          ▼
startView()           切换到其他页
   │                          │
   ▼                          ▼
createView() +        invokeMethod()
handler注册           ("onPageState", "inactive")
   │
   ▼
tryEmbed() (尺寸>1x1时)
   │
   ▼
embedInto() → 子窗口挂载
   │
   ▼
resize() → 匹配面板尺寸
```

#### Engine 创建（应用启动时）

```cpp
// MainFrame.cpp 构造函数中（所有 tab 创建之前）
m_flutter_engine = createFlutterEngine("", "").release();
m_flutter_engine->start();
```

- `createFlutterEngine(assetsPath, icuDataPath)` — 工厂函数，创建平台对应的引擎实例。两个参数目前在平台实现中都未使用（路径在 `createView` 时内部解析）。
- **macOS**: 创建 `FlutterDartProject` 并保留引用，后续每个 view 创建时通过 `initWithName:project:allowHeadlessExecution:YES` 共享同一个 project。
- **Windows**: `start()` 实际上只设置一个 `m_started = true` 标记——Windows 的引擎/视图是耦合的，引擎在 `createView` 时创建。
- **Linux**: `start()` 设置 `FLUTTER_LINUX_RENDERER=software` 环境变量后标记已启动。

#### View 创建（面板变为可见时）

```cpp
// startView() 调用链:
m_view = engine->createView(entrypoint, channelName);
// 内部:
//   Windows: FlutterDesktopEngineCreate + FlutterDesktopEngineRun + FlutterDesktopViewControllerCreate
//   macOS: [FlutterEngine runWithEntrypoint:] + FlutterViewController
//   Linux: fl_view_new(project) + FlMethodChannel 创建
```

**View 创建不会立即嵌入**——`startView()` 调用 `tryEmbed()` 并检查 `m_embedded` 标志。如果此时面板尺寸为 0×0 或 1×1（例如不在当前选中的 notebook 页上），则推迟嵌入。

#### tryEmbed() 延迟嵌入模式

```cpp
void FlutterPanel::tryEmbed() {
    if (m_embedded || !m_view) return;
    wxSize sz = GetSize();
    if (sz.GetWidth() <= 1 || sz.GetHeight() <= 1) return;
    m_view->embedInto(GetHandle());
    m_embedded = true;
}
```

**调用时机：**
- `startView()` 末尾 — 如果面板已有有效尺寸，立即嵌入。
- `onSize()` — 每次面板尺寸变化时检查，如果尚未嵌入且尺寸 > 1×1，执行嵌入。
- `onShow()` — 面板变为可见时检查。

**为什么需要 1×1 阈值：** Notebook 未选中页面的子控件通常被分配 0×0 或 1×1 的尺寸（取决于平台和 wxWidgets 版本）。在这样的尺寸下嵌入 Flutter 视图会导致 view 以 800×600 的硬编码默认尺寸显示一帧，然后才被 `onSize` 调整，产生可见的闪烁。

#### View 销毁

- **Windows**: `FlutterViewHostWin` 析构时先注销 messenger callback（`FlutterDesktopMessengerSetCallback` 传 nullptr），然后 `FlutterDesktopViewControllerDestroy` 销毁 controller（controller 销毁同时也会销毁 engine）。
- **macOS**: controller 和 channel 由 engine 持有，`FlutterViewHostMacOS` 析构时不手动释放。Engine 在 `FlutterEngineHostMacOS::stop()` 中通过 `[engine shutDownEngine]` 统一关闭。

  **macOS 引擎销毁顺序详解**：
  1. `FlutterEngineHostMacOS::stop()` 遍历 `NSMutableArray *engines`（按创建顺序）
  2. 对每个 `FlutterEngine *e` 调用 `[e shutDownEngine]`——这会关闭 Dart isolate、释放 GPU 资源、终止 vsync 回调
  3. `[engines removeAllObjects]` 清空数组，ARC 自动释放 engine 对象
  4. 注意：必须先调用 `shutDownEngine` 再移除引用，否则 engine 在 isolate 仍在运行时被释放会导致 crash
  5. platform thread 回调（如 MethodChannel handler）在 `shutDownEngine` 返回前完成，因此不存在回调访问已释放 engine 的竞态

  这与 Windows 不同：Windows 的 engine 生命周期绑定在 ViewController 中（`FlutterDesktopViewControllerDestroy` 同时销毁 engine），而 macOS 的 engine 是独立对象，由 `NSMutableArray` 集中管理。Linux 则介于两者之间——engine 与 FlView 通过 weak pointer 关联。
- **Linux**: 析构时先注销 method channel handler，然后 `g_clear_object` 释放 channel 和 engine，再安全释放 FlView（使用 weak pointer 防止悬空指针）。

#### Engine 关闭（应用退出时）

```cpp
if (m_flutter_engine) {
    m_flutter_engine->stop();
    delete m_flutter_engine;
    m_flutter_engine = nullptr;
}
```

### 7.5 C++ 开发者常见陷阱

#### 1. 不要在面板尚无有效尺寸前调用 embedInto

```cpp
// 错误示例：
auto view = engine->createView("myEntrypoint", "my/channel");
view->embedInto(panelHandle);  // panic: 面板可能还是 0×0

// 正确做法：使用 FlutterPanel::startView()，它会自动调用 tryEmbed()
panel->startView(engine, "myEntrypoint", "my/channel", handler);
```

#### 2. 不要忘记在事件处理器中调用 event.Skip()

```cpp
void FlutterPanel::onSize(wxSizeEvent& event) {
    if (m_view) { /* resize 逻辑 */ }
    event.Skip();  // 必须！否则父类/布局系统收不到 SIZE 事件
}

void FlutterPanel::onShow(wxShowEvent& event) {
    if (event.IsShown() && m_view) { /* 处理 */ }
    event.Skip();  // 必须！否则 wxWidgets 内部状态不一致
}
```

#### 3. MethodChannel handler 必须调用 reply() 或抛异常

```cpp
// 错误示例：既没有 reply 也没有 throw
d.on("badHandler", [](auto args, auto reply) {
    do_something(args);
    // 函数结束时 reply 未被调用，Dart 侧 Future 永久挂起
});

// 正确做法：
d.on("goodHandler", [](auto args, auto reply) {
    do_something(args);
    reply("done");
});

// 或：存储 reply 以备稍后使用（必须确保最终会调用）
d.on("asyncHandler", [this](auto args, auto reply) {
    m_pending_reply = std::make_shared<FlutterViewHost::Reply>(std::move(reply));
    // 确保在某个未来时刻调用 (*m_pending_reply)(result);
});
```

**框架保护机制：** 在 Windows 和 Linux 实现中，如果 handler 返回时没有调用 reply 且没有抛异常，框架会自动回复（Windows 发送空回复，Linux 回复 `not_implemented`）防止 Dart 侧挂起。但不应依赖此行为——这只是一个安全兜底。

#### 4. 线程安全：所有 Flutter API 必须在主线程调用

```cpp
// 错误示例：在工作线程中调用 Flutter API
std::thread([this] {
    auto result = compute_heavy();
    m_view->invokeMethod("onResult", result);  // 崩溃！
}).detach();

// 正确做法：使用 CallAfter 将结果投递回主线程
std::thread([this] {
    auto result = compute_heavy();
    CallAfter([this, result = std::move(result)] {
        if (m_view) m_view->invokeMethod("onResult", result);
    });
}).detach();
```

这适用于所有 Flutter API：`invokeMethod`、`reply()`、`focus()`、`resize()`、`embedInto()`。Flutter Engine 不是线程安全的，所有操作必须在创建引擎的同一线程（主线程/UI 线程）执行。

#### 5. Windows：不要在 WM_SETFOCUS 处理期间调用 ::SetFocus

```cpp
// 为什么 FlutterPanel::onSetFocus 使用 CallAfter：
void FlutterPanel::onSetFocus(wxFocusEvent& event) {
    // 直接调用 m_view->focus() 会在此处执行 ::SetFocus(childHwnd)
    // 但 Windows 在处理 WM_SETFOCUS 时会忽略嵌套的 SetFocus 调用
    CallAfter([this] {
        if (m_view) m_view->focus();  // 延迟到下一个消息循环
    });
    event.Skip();
}

// SetFocus() 重写提供了更直接的路径
// 通过 MSWGetFocusHWND() 让 wxWindow::SetFocus() 自身指向正确的 HWND
```

#### 6. Messenger callback 必须在 controller 销毁前注销

```cpp
// FlutterViewHostWin 析构顺序正确示范：
~FlutterViewHostWin() override {
    // 第一步：注销 messenger 回调（如果先销毁 controller，
    //         messenger 可能已失效导致回调注销失败或访问无效内存）
    if (m_messenger) {
        FlutterDesktopMessengerSetCallback(
            m_messenger, m_channel.c_str(), nullptr, nullptr);
    }
    // 第二步：销毁 controller（engine 随 controller 销毁）
    if (m_controller) {
        FlutterDesktopViewControllerDestroy(m_controller);
    }
}
```

---

## 第八章：Flutter 开发者指南

### 8.1 项目结构

```
flutter_app/
├── lib/
│   └── main.dart              # 入口定义 + Widget 树（核心文件）
├── test/
│   └── widget_test.dart       # Widget 测试
├── linux/                     # Linux 平台 Runner（构建目标用）
│   ├── CMakeLists.txt
│   ├── runner/
│   │   ├── main.cc            # 独立运行的 main()
│   │   ├── my_application.h
│   │   └── my_application.cc
│   └── flutter/
│       ├── CMakeLists.txt
│       └── generated_plugin_registrant.cc
├── windows/                   # Windows 平台 Runner（构建目标用）
│   ├── CMakeLists.txt
│   └── flutter/
│       └── generated_plugins.cmake
├── macos/                     # macOS 平台 Runner（构建目标用）
│   ├── Runner.xcodeproj/
│   ├── Runner/
│   │   ├── MainFlutterWindow.swift
│   │   └── AppDelegate.swift
│   └── Flutter/
│       └── GeneratedPluginRegistrant.swift
├── pubspec.yaml               # 项目依赖和元数据
├── pubspec.lock               # 锁定的依赖版本
├── analysis_options.yaml      # Dart 静态分析配置
└── .metadata                  # Flutter 工具元数据
```

**关键说明：**
- `lib/main.dart` 是唯一手写的业务代码文件。所有 UI、入口点和通信逻辑都在此文件中。
- `linux/`、`windows/`、`macos/` 目录中的 Runner 代码仅在独立 `flutter run` 调试时使用——嵌入式运行时由 OrcaSlicer 的 C++ 代码管理引擎生命周期，这些 Runner 的 `main()` 不会被调用。
- `pubspec.yaml` 指定了 Dart SDK 约束（当前为 `^3.11.5`）和依赖项。

### 8.2 入口约定（Entrypoint Convention）

```dart
// 所有入口函数必须满足以下条件：
// 1. 使用 @pragma('vm:entry-point') 注解
// 2. 是顶级函数（top-level）或类的静态方法
// 3. 无参数，无返回值

@pragma('vm:entry-point')
void main() => runApp(const HomePanelApp());  // 默认入口

@pragma('vm:entry-point')
void homeMain() => runApp(const HomePanelApp());

@pragma('vm:entry-point')
void prepareMain() => runApp(const PreparePanelApp());
```

**`@pragma('vm:entry-point')` 的作用：**

Flutter 的 AOT 编译器默认执行树摇优化（tree-shaking），移除编译时看起来未被引用的函数。由于这些入口函数是从 C++ 端通过名称字符串调用的，Dart 编译器无法通过静态分析发现它们被引用。`@pragma('vm:entry-point')` 告诉编译器保留这些符号，防止被优化移除。

**入口函数命名规范：**
- 使用小写驼峰（camelCase）。
- 建议以 `Main` 结尾，如 `homeMain`、`prepareMain`。
- C++ 端 `startView()` 的 `entrypoint` 参数与此处的函数名完全匹配（字符串精确比较）。
- 在 Linux 上，`fl_view_new` 不支持自定义 entrypoint（总是运行 `main()`），但代码中保留了参数以便将来支持。

**main() 作为默认入口：**
- 如果调用 `startView(engine, "", channelName, handler)`（entrypoint 为空字符串），各平台会自动使用默认的 `main()` 函数。
- Windows 实现：`FlutterDesktopEngineRun(engine, nullptr)` 使用默认入口。
- macOS 实现：`runWithEntrypoint:nil` 使用默认入口。

### 8.3 MethodChannel 设置

#### Dart 端完整示例

```dart
import 'package:flutter/services.dart';

class PanelPage extends StatefulWidget {
  final String channelName;
  const PanelPage(this.channelName, {super.key});

  @override
  State<PanelPage> createState() => _PanelPageState();
}

class _PanelPageState extends State<PanelPage> {
  late final MethodChannel _channel;
  int _counter = 0;

  @override
  void initState() {
    super.initState();
    // 1. 使用与 C++ 端相同的 name 创建 MethodChannel
    _channel = MethodChannel(widget.channelName);

    // 2. 设置处理器，接收 C++ 端发送的方法调用
    _channel.setMethodCallHandler(_onMethodCall);

    // 3. （可选）主动调用 C++ 方法获取初始数据
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
          });
        }
        return null;

      case 'onCounterUpdate':
        // 整型参数 — 得益于 C++ 端的自动类型检测
        if (mounted) setState(() => _counter = call.arguments as int);
        return null;

      default:
        throw MissingPluginException();
    }
  }

  Future<void> _askCpp() async {
    try {
      // invokeMethod<T>: 指定期望的返回类型
      final val = await _channel.invokeMethod<int>('incrementCounter', _counter);
      if (mounted && val != null) setState(() => _counter = val);
    } catch (_) {}
  }
}
```

**关键约定：**

- MethodChannel 的 name 字符串必须与 C++ 端 `startView()` 的 `channelName` 参数完全一致。当前使用的命名约定是 `snapmaker/模块名`，如 `snapmaker/home`、`snapmaker/prepare`。
- 使用 `invokeMethod<T>` 指定返回类型，让 Dart 的类型系统正确解析 C++ 返回的值。
- 始终在操作前检查 `mounted` 状态，防止 Widget 已销毁后还在 setState。
- 使用 try/catch 包裹 invokeMethod，因为 C++ 侧可能未实现该方法（返回 `MissingPluginException`）。

#### 类型感知编码

得益于 C++ 端的自动类型检测，Dart 侧可以使用类型化的 `invokeMethod`：

```dart
// C++ 端回复 "123" → 检测为整数 → 编码为 int32
final count = await channel.invokeMethod<int>('getCount');
print(count.runtimeType); // int

// C++ 端回复 "hello world" → 非数字 → 编码为 string
final text = await channel.invokeMethod<String>('getMessage');
print(text.runtimeType); // String

// C++ 端回复 "3.14" → 检测为浮点数 → 编码为 float64
final pi = await channel.invokeMethod<double>('getPi');
print(pi.runtimeType); // double
```

### 8.4 添加新 Widget（最佳实践）

#### Step 1：定义 MethodChannel 接口契约

首先在设计层面明确双方通信的方法签名：

```
Dart → C++（Dart 调用，C++ 实现）:
  - getVersion(): String
  - sendMessage(text: String): String
  - incrementCounter(value: int): int

C++ → Dart（C++ 调用，Dart 实现）:
  - onMessage(msg: String): void
  - onCounterUpdate(value: int): void
  - onPageState(state: String): void  // "active" | "inactive"
```

#### Step 2：创建 Widget，将 channel 作为构造函数参数

```dart
class MonitorPanel extends StatefulWidget {
  final MethodChannel channel;  // 由外部注入，便于测试
  const MonitorPanel({required this.channel, super.key});

  @override
  State<MonitorPanel> createState() => _MonitorPanelState();
}
```

#### Step 3：在 initState/dispose 中管理 channel 生命周期

```dart
class _MonitorPanelState extends State<MonitorPanel> {
  @override
  void initState() {
    super.initState();
    widget.channel.setMethodCallHandler(_onCall);
    _fetchInitialData();
  }

  @override
  void dispose() {
    // 清空 handler 防止 Widget 销毁后的回调
    widget.channel.setMethodCallHandler(null);
    super.dispose();
  }
}
```

#### Step 4：正确处理异步响应

```dart
Future<void> _sendCommand(String cmd) async {
  try {
    final result = await widget.channel
        .invokeMethod<String>('sendCommand', cmd)
        .timeout(const Duration(seconds: 5));
    if (mounted) {
      setState(() => _lastResult = result ?? 'no response');
    }
  } on TimeoutException {
    if (mounted) _showError('Command timed out');
  } on PlatformException catch (e) {
    if (mounted) _showError('Error: ${e.message}');
  }
}
```

#### Step 5：使用 MockMethodChannel 在集成前进行独立测试

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/services.dart';

void main() {
  testWidgets('MonitorPanel receives status updates', (tester) async {
    final mockChannel = MethodChannel('snapmaker/monitor');
    
    // 拦截 Dart→C++ 调用
    mockChannel.setMockMethodCallHandler((call) async {
      if (call.method == 'getStatus') return 'idle';
      return null;
    });
    
    await tester.pumpWidget(
      MaterialApp(home: MonitorPanel(channel: mockChannel)),
    );
    
    // 模拟 C++→Dart 调用
    await mockChannel.invokeMethod('onStatusChanged', 'printing');
    await tester.pump();
    
    expect(find.text('printing'), findsOneWidget);
  });
}
```

这种方法允许你在不启动整个 OrcaSlicer 的情况下开发和测试 Flutter UI，利用 Flutter 的 Hot Reload 和 DevTools 提高效率。参见 9.2 节的 Method A。

### 8.5 dart:ffi 与直接内存访问

在某些场景下（如需要高性能数据交换、访问 C++ 配置结构体），可以通过 `dart:ffi` 直接访问宿主进程的内存。

#### 加载宿主进程符号

```dart
import 'dart:ffi';

// DynamicLibrary.open('') 加载宿主进程（OrcaSlicer）自身
// 所有在 C++ 端标记为导出的符号都可以从 Dart 访问
final dylib = DynamicLibrary.open(''); // 空字符串 = 宿主进程
```

**注意：** `DynamicLibrary.open('')` 在不同平台的行为：
- macOS/Linux: 加载主可执行文件的符号表。
- Windows: 需要 C++ 端使用 `__declspec(dllexport)` 或 `.def` 文件导出符号，然后加载自身 DLL。

#### 通过指针访问 C++ 对象

```dart
// C++ 端通过 MethodChannel 将对象地址作为整数传给 Dart
// C++: reply(std::to_string(reinterpret_cast<intptr_t>(config_ptr)));

// Dart 端接收并转换
final ptrStr = await channel.invokeMethod<String>('getConfigPointer');
final ptr = Pointer<ConfigStruct>.fromAddress(int.parse(ptrStr!));

// 读取结构体字段
final speed = ptr.ref.print_speed;
final temp = ptr.ref.nozzle_temperature;
```

#### C++ 端导出示例

```cpp
// C++ 端：定义可被 Dart FFI 访问的结构体
extern "C" {  // 使用 C 链接，防止 C++ 名称修饰
struct __attribute__((packed)) PrinterConfig {
    int32_t print_speed;
    int32_t nozzle_temperature;
    float layer_height;
};

// 导出函数供 Dart 调用
__declspec(dllexport)  // Windows
__attribute__((visibility("default")))  // macOS/Linux
int32_t get_printer_status(int32_t printer_id) {
    return query_status(printer_id);
}
}
```

#### Dart FFI 端类型映射

```dart
// Dart FFI 对应结构体定义
final class PrinterConfig extends Struct {
  @Int32()
  external int printSpeed;

  @Int32()
  external int nozzleTemperature;

  @Float()
  external double layerHeight;
}

// Dart FFI 函数绑定
typedef GetPrinterStatusNative = Int32 Function(Int32 printerId);
typedef GetPrinterStatusDart = int Function(int printerId);

final getStatus = dylib
    .lookupFunction<GetPrinterStatusNative, GetPrinterStatusDart>('get_printer_status');
```

#### 安全约束和限制

1. **指针生命周期管理**：Dart 端持有的 C++ 指针必须保证在 Dart 访问时 C++ 对象仍然存活。不要在 C++ 端销毁对象后继续使用指针。
2. **不能跨平台假定内存布局**：结构体填充（padding）和对齐（alignment）因平台/编译器而异。使用 `packed` 属性并仔细验证大小。
3. **MethodChannel 传递指针是整数**：通过 `intptr_t → string → int` 的转换传递指针，而非直接通过 MethodChannel 传递二进制数据。
4. **线程限制**：FFI 调用同样必须在主线程执行。
5. **类型安全**：FFI 绕过 Dart 的类型系统，错误的指针类型转换会导致未定义行为（通常表现为崩溃）。

#### 完整示例：读取 C++ 配置结构体

```dart
// Dart 端完整流程
class ConfigReader {
  late final DynamicLibrary _lib;
  Pointer<PrinterConfig>? _configPtr;

  Future<void> init(MethodChannel channel) async {
    _lib = DynamicLibrary.open('');
    final addrStr = await channel.invokeMethod<String>('getConfigPointer');
    _configPtr = Pointer<PrinterConfig>.fromAddress(int.parse(addrStr!));
  }

  PrinterConfig get config => _configPtr!.ref;

  void dispose() {
    _configPtr = nullptr;
  }
}
```

### 8.6 Widget 通信模式

#### 父子 Widget 通信

标准 Flutter 模式，通过构造函数参数或 callback 传递数据：

```dart
class ParentPanel extends StatefulWidget {
  @override
  State<ParentPanel> createState() => _ParentPanelState();
}

class _ParentPanelState extends State<ParentPanel> {
  String _status = 'idle';

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        StatusHeader(status: _status),
        ControlPanel(onCommand: (cmd) {
          setState(() => _status = 'executing $cmd');
          _channel.invokeMethod('sendCommand', cmd);
        }),
      ],
    );
  }
}
```

#### 跨面板通信（通过 C++ 桥接）

由于每个 Flutter 面板是独立的 Flutter View（独立的 Dart isolate 上下文），面板之间不能直接通信。它们通过 C++ 端作为中介：

```
Panel A (Dart)                    Panel B (Dart)
    │                                  │
    │ invokeMethod("notifyB", data)    │
    ▼                                  │
  C++ Dispatcher                      │
    │                                  │
    │ m_panel_b->view()                │
    │   ->invokeMethod("onNotify",     │
    │                  data)            │
    └─────────────────────────────────►│
```

```cpp
// C++ 端跨面板通信示例
d_a.on("notifyB", [this](auto args, auto reply) {
    // 从 Panel A 收到消息，转发给 Panel B
    if (m_panel_b && m_panel_b->view()) {
        m_panel_b->view()->invokeMethod("onNotify", args);
    }
    reply("forwarded");
});
```

```dart
// Panel B (Dart 端)
void _onMethodCall(MethodCall call) async {
  switch (call.method) {
    case 'onNotify':
      final data = call.arguments as String;
      setState(() => _notifications.add(data));
      break;
  }
}
```

#### 状态管理方式

当前项目采用简单的 `setState` + 直接状态传递方式，没有引入 Provider、Riverpod 或 BLoC 等状态管理库。原因：
1. 当前 Flutter 面板较简单，单个 Widget 的状态复杂度不高。
2. 大部分复杂状态（配置数据、打印进度）存储在 C++ 端，Dart 端仅展示。
3. 避免额外依赖——`pubspec.yaml` 中只有 `flutter` SDK 和 `cupertino_icons`。

当面板复杂度增加时，可以考虑引入 `provider` 包进行状态管理，但仍应保持跨面板通信通过 C++ 桥接的模式。

---

## 第九章：调试与故障排查

### 9.1 C++ 端调试

#### 调试构建配置

```bash
# Windows: Debug 构建
build_release_vs2022.bat debug

# macOS: Debug 构建
# 修改 build_release_macos.sh 中的构建类型为 Debug

# Linux: Debug 构建
./build_linux.sh -b -dsi
```

Debug 模式下：
- Flutter 引擎使用 debug 版本的库，包含更多断言和诊断信息。
- C++ 代码包含调试符号，可以使用断点调试。

#### C++ 端日志

```cpp
#include <boost/log/trivial.hpp>

// 使用 Boost.Log 输出诊断信息
BOOST_LOG_TRIVIAL(info) << "[Flutter] Engine started";
BOOST_LOG_TRIVIAL(error) << "[Flutter] startView failed";
BOOST_LOG_TRIVIAL(debug) << "[Flutter] Method call: " << method;
```

#### MethodChannel 处理器断点

```cpp
// 在 Dispatcher 的回调中设置断点
d.on("sendMessage", [](auto args, auto reply) {
    // 在此处设断点，可以观察：
    //   - args: Dart 传来的参数
    //   - 调用栈: 从 Flutter 引擎映射到 C++ handler 的完整路径
    reply("[Orca] C++ received: " + args);
});
```

#### 验证引擎状态

```cpp
// 运行时检查引擎是否正常
if (!m_flutter_engine) {
    BOOST_LOG_TRIVIAL(error) << "Engine is null";
}
if (!m_view) {
    BOOST_LOG_TRIVIAL(error) << "View is null";
}
if (!m_embedded) {
    BOOST_LOG_TRIVIAL(warning) << "View not yet embedded";
}
```

### 9.2 嵌入式 Flutter 调试方法

#### Method A：独立 Flutter 开发（推荐用于 Widget 开发）

**适用场景：** 开发 Flutter UI/Widget 时使用，不需要完整的 OrcaSlicer 环境。

```bash
# 1. 选择一个平台独立运行
cd flutter_app
flutter run -d macos    # macOS
flutter run -d windows  # Windows
flutter run -d linux    # Linux

# 2. 在 Dart 代码中使用 MockMethodChannel 模拟 C++ 端
```

**优点：**
- 支持 Hot Reload — 代码修改后几乎即时生效（< 1 秒）。
- 完整的 Flutter DevTools 可用（Widget Inspector、Performance、Memory 等）。
- 不需要编译整个 OrcaSlicer C++ 项目。

**缺点：**
- 与 C++ 端的交互是模拟的，真实集成时可能遇到编码/通信问题。
- 无法测试依赖 C++ 数据的场景。

**MockMethodChannel 示例：**

```dart
// 在 main.dart 中添加 mock 模式开关
const bool kUseMockChannel = true; // 独立运行时为 true

class _PanelPageState extends State<PanelPage> {
  late final MethodChannel _channel;

  @override
  void initState() {
    super.initState();
    if (kUseMockChannel) {
      _channel = const MethodChannel('snapmaker/home');
      _setupMock();
    } else {
      _channel = MethodChannel(widget.channelName);
    }
    _channel.setMethodCallHandler(_onMethodCall);
  }

  void _setupMock() {
    (_channel as dynamic).setMockMethodCallHandler((MethodCall call) async {
      switch (call.method) {
        case 'getVersion': return 'mock-1.0';
        case 'sendMessage': return '[Mock] received: ${call.arguments}';
        case 'incrementCounter': return (call.arguments as int) + 1;
        default: return null;
      }
    });
  }
}
```

**widget_test.dart 说明：**

`flutter_app/test/widget_test.dart` 是项目附带的 Flutter 标准单元测试文件。它使用 Flutter 的 `testWidgets` 函数对 Widget 进行渲染和交互验证：

```dart
// 典型内容（随项目模板生成，可按需扩展）
void main() {
  testWidgets('HomePanel renders', (WidgetTester tester) async {
    await tester.pumpWidget(const HomePanelApp());
    expect(find.text('Home'), findsOneWidget);
  });
}
```

**运行测试：**
```bash
cd flutter_app
flutter test          # 运行所有 test/ 下的测试
flutter test test/widget_test.dart  # 运行特定文件
```

测试在不启动完整应用的情况下验证 Widget 的正确性。推荐在每次修改 Dart 代码后运行 `flutter test` 确保没有引入回归。

#### Method B：Debug 嵌入模式（VM Service）

**适用场景：** 需要在真实 OrcaSlicer 环境中调试 Dart 代码。

```bash
# 1. 以 Debug 模式编译 Flutter App
cd flutter_app
flutter build macos --debug  # 或其他平台

# 2. 启动 OrcaSlicer 时传入 VM Service 参数
# 在 C++ 端传递参数给引擎（平台相关）:
#   --dart-flags=--enable-vm-service=8181
#   --dart-flags=--disable-service-auth-codes

# 3. 使用 flutter attach 连接
flutter attach --debug-uri=http://localhost:8181/...
```

**或者通过 DevTools 连接：**
```bash
flutter devtools
# 在浏览器中打开 DevTools，输入 VM Service URI
```

**限制：**
- 嵌入式模式不支持 Hot Reload — 引擎以 AOT 模式运行，每次修改需要重新编译完整应用。
- Hot Restart 也不可用，因为引擎生命周期由宿主应用管理。
- 可以使用 DevTools 查看 Widget 树、内存、日志，但不能实时更新代码。

#### Method C：基于日志的调试

**适用场景：** 最小侵入性的诊断方式。

```dart
// Dart 端使用 debugPrint
debugPrint('[Dart] Channel initialized: ${widget.channelName}');
debugPrint('[Dart] Received method call: ${call.method}');
debugPrint('[Dart] Arguments: ${call.arguments}');
```

C++ 端对应的日志级别设置：

```bash
# Windows
set FLUTTER_LOG_LEVEL=2

# macOS/Linux
export FLUTTER_LOG_LEVEL=2
# 0=无日志, 1=错误, 2=警告, 3=信息, 4=详细
```

**`debugPrint` 输出位置：**
- **Windows**: `OutputDebugString` → Visual Studio 输出窗口或 DebugView 工具。
- **macOS**: `NSLog` → Console.app 或系统日志。
- **Linux**: `g_print` → 终端输出（如果从终端启动）或系统日志。

### 9.3 常见问题及解决方案

#### 问题 1：空白面板（Blank White Panel）

**症状：** Flutter 面板区域只显示一片白色或黑色，无任何 UI 内容。

**可能原因及排查步骤：**

1. **引擎未启动** — 确认 `m_flutter_engine->start()` 已被调用且返回 true。
2. **embedInto 未被调用** — 检查 `tryEmbed()` 条件：面板尺寸是否大于 1×1？`GetHandle()` 是否返回有效句柄？
3. **AOT 库未找到** — 检查可执行文件目录下是否存在：
   - Windows: `flutter_app.dll` 和 `data/flutter_assets/`
   - macOS: `App.framework`
   - Linux: `lib/libflutter_app.so` 和 `data/flutter_assets/`
4. **entrypoint 名称错误** — C++ 端传入的 entrypoint 名称与 Dart 端的 `@pragma('vm:entry-point')` 函数名不匹配。
5. **Flutter Engine 崩溃** — 检查控制台/日志中是否有 Flutter 引擎的错误输出。

#### 问题 2：Dart 无响应（No Response from Dart）

**症状：** `invokeMethod` 调用后 Dart 侧没有收到，或 Dart 侧的 `invokeMethod` 返回 MissingPluginException。

**排查步骤：**

1. **MethodChannel 名称不匹配** — 检查 C++ 端 `startView()` 的 `channelName` 参数与 Dart 端 `MethodChannel('...')` 的 name 是否完全一致。
2. **handler 未注册** — 确认 `setMethodCallHandler` 在方法调用到达之前已完成。
3. **使用 CallAfter 延迟注册** — 确认 handler 在 `CallAfter` 中注册且已被执行。
4. **Dart 入口未正确初始化** — 检查 Dart 端 `initState` 中是否正确创建了 MethodChannel 并设置 handler。

#### 问题 3：键盘不工作（Keyboard Doesn't Work）

**症状：** Flutter 面板中的 TextField 等输入控件无法接收键盘输入。

**排查：**

- **Windows**: 检查 `MSWShouldPreProcessMessage` 是否正确返回 `false`（对于 Flutter 子窗口消息）。
- **Linux**: 确认 `GtkEventBox` 包装已正确创建并设置了 `gtk_widget_set_can_focus(TRUE)`。
- **macOS**: 确认 `makeFirstResponder:` 已正确调用。
- 检查焦点是否实际在 Flutter 子窗口上（可使用 Spy++/GTK Inspector/ Accessibility Inspector 查看焦点链）。

#### 问题 4：尺寸错误（Wrong Size）

**症状：** Flutter 视图显示为错误的大小，如默认 800×600 而非实际面板大小。

**排查：**

1. **tryEmbed 是否在尺寸有效后被调用** — 在 `onSize` 或 `onShow` 中添加日志确认。
2. **Linux resize 时序问题** — 确认 `resize()` 中的 `pizza->move()` 和 `pizza->size_allocate_child()` 调用顺序正确。
3. **使用 `event.GetSize()` 而不是 `GetClientSize()`** — 在 wxGTK 上，`GetClientSize()` 查询 GTK 分配尺寸可能已过时。

#### 问题 5：退出时崩溃（Crash on Exit）

**症状：** 关闭 OrcaSlicer 时应用程序崩溃，调用栈指向 Flutter 引擎相关代码。

**排查：**

1. **Messenger callback 在 controller 销毁之前未注销** — 检查 `FlutterViewHostWin` 析构顺序（Windows）。
2. **悬空指针** — Linux 端使用 `g_object_add_weak_pointer` 防止悬空指针。
3. **Engine 和 View 的销毁顺序** — 确保先销毁所有 View，再销毁 Engine。

#### 问题 6：Dart Method Not Found

**症状：** `invokeMethod` 调用后 Dart 侧返回 "MissingPluginException" 或类似错误。

**排查：**

1. **entrypoint 名称拼写错误** — 大小写匹配，完全相同的字符串。
2. **AOT 编译时入口被树摇优化移除** — 确认 `@pragma('vm:entry-point')` 注解存在且格式正确。
3. **Dart 代码未重新编译** — 修改 Dart 代码后是否重新构建了 Flutter App？

### 9.4 诊断检查清单

在 Flutter 面板出现问题时，按以下清单逐项排查：

- [ ] **引擎是否成功启动？** `createFlutterEngine()` 返回非空，且 `start()` 返回 true。
- [ ] **View 是否用正确的 entrypoint 创建？** `startView()` 返回 true。检查 entrypoint 名称与 Dart 端匹配。
- [ ] **embedInto 是否被调用，且传入了有效的父 HWND？** 检查 `m_embedded` 是否为 true。若为 false，检查面板尺寸是否大于 1×1（参见 `tryEmbed()` 逻辑）。
- [ ] **Channel 名称是否两端匹配？** C++ 的 `channelName` 参数 == Dart 的 `MethodChannel('name')` 的 name。
- [ ] **Handler 是否在第一条消息到达前注册？** 通常在 `CallAfter` 中调用 `startView()` 以确保 handler 在消息循环开始前就位。
- [ ] **面板是否有非零尺寸？** 在 `onSize` 事件中打印 `event.GetSize()`。若为 0×0 或 1×1，面板可能在不活跃的 Notebook 页上。
- [ ] **AOT 库和 assets 是否在正确的位置？** 相对于可执行文件路径检查：
  - Windows: `exe_dir\flutter_app.dll` 和 `exe_dir\data\flutter_assets\`
  - macOS: `App.framework` 在 app bundle 的 Frameworks 目录中
  - Linux: `exe_dir/lib/libflutter_app.so` 和 `exe_dir/data/flutter_assets/`
- [ ] **是否有 Flutter 引擎错误日志？** 设置 `FLUTTER_LOG_LEVEL=2` 环境变量，查看引擎输出。
- [ ] **Dart 端的 channel 是否在 initState 中正确初始化？** 确认 `MethodChannel(widget.channelName)` 和 `setMethodCallHandler` 都在 `initState` 中调用。
- [ ] **线程是否正确？** 所有 Flutter API 调用是否都在主线程执行？

---

## 第十章：架构决策记录 (ADR)

### ADR-1：双层 Engine/View 架构

**决策：** 将 Flutter 引擎实例（`FlutterEngineHost`）与视图实例（`FlutterViewHost`）分离为两个独立的类层次。

**背景：** OrcaSlicer 需要在多个 Notebook 标签页中显示 Flutter UI。每个标签页有独立的 Dart Widget 树和独立的 MethodChannel，但它们共享同一个 Flutter 引擎进程。

**理由：**
- 一个引擎进程（Engine）可以创建多个视图（View），每个视图对应一个独立的 Dart isolate 和 WindowController。
- Engine 生命周期等同于应用生命周期（随应用启动创建，退出时销毁）。
- View 生命周期等同于面板生命周期（面板可见时创建，面板关闭时销毁）。
- 每个 View 拥有独立的 MethodChannel，互不干扰。

**备选方案：**
1. **每个 View 一个 Engine**：资源开销过大。每个 Flutter Engine 包含完整的 Dart VM 实例，内存占用高（~50MB+），启动速度慢（~500ms+）。对于 3-5 个面板意味着 150-250MB 额外内存。
2. **全局单例（Singleton）**：难以处理面板间隔离。MethodChannel 的 name 成为唯一区分符，多面板同时活跃时容易产生命名冲突或回调串扰。
3. **每个 Window 一个 Engine**：不符合 wxNotebook 的架构——Notebook 页面不是独立的 OS 窗口，而是同一窗口内的子控件。

**实现：**

```cpp
// flutter_host.h — 接口定义
class FlutterViewHost {
    // 一个 View 对应一个 Flutter ViewController + MethodChannel
    virtual void embedInto(void* parentView) = 0;
    virtual void resize(int width, int height) = 0;
    virtual void invokeMethod(...) = 0;
    virtual void setMethodCallHandler(...) = 0;
};

class FlutterEngineHost {
    // 一个 Engine 对应一个 Flutter Engine 进程
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual unique_ptr<FlutterViewHost> createView(...) = 0;
};
```

> **注：** 注意 MacOS 平台内部实现不同。在 macOS 上，每个视图创建一个新的 FlutterEngine 实例（通过 `initWithName:project:allowHeadlessExecution:YES`），但从概念上讲仍是"→对多"的架构——主机仅管理一个 Flutter Dart Project。

### ADR-2：Windows 手动实现 StandardMethodCodec

**决策：** 在 Windows 平台手动实现 `StandardMethodCodec` 的编码/解码，不使用 JSON channel 或链接 Flutter C++ 源码。

**背景：** Flutter 的 MethodChannel 在源码层面使用 `StandardMethodCodec`（一种二进制编码）作为默认编解码器。C++ 端需要能够发送和接收与 Dart 端 `MethodChannel` 兼容的编码消息。

**理由：**
- `StandardMethodCodec` 是 MethodChannel 的底层通信协议——它不是可选的，不支持直接使用 JSON channel。
- Dart 端的类型化 `invokeMethod<T>`（如 `invokeMethod<int>`）依赖 C++ 端返回正确类型码的值（int32=0x03, int64=0x04, float64=0x06, string=0x07 等），而不是始终返回字符串。
- 链接完整的 Flutter C++ `client_wrapper` 源码会引入大量不必要的依赖，且部分 API 在新版 Flutter SDK (3.38+) 中已被移除。

**实现范围：** 约 200 行代码，涵盖以下 StandardMessageCodec 类型的读写：

| 类型         | 码值 | 大小              | 实现          |
|-------------|------|-------------------|---------------|
| null        | 0x00 | 1 byte            | 读写          |
| true        | 0x01 | 1 byte            | 读            |
| false       | 0x02 | 1 byte            | 读            |
| int32       | 0x03 | 5 bytes (1+4)     | 读写          |
| int64       | 0x04 | 9 bytes (1+8)     | 读写          |
| float64     | 0x06 | 9 bytes (1+8)     | 读写          |
| string      | 0x07 | 1+varint+data     | 读写          |

此外实现了：
- varint 编码/解码（7-bit 可变长度整数）
- `encodeSuccess(result)` / `encodeError(code, message)` 封装
- `writeValue()` 自动类型检测：以字符串为输入，自动判断是否为纯整数或浮点数，选择正确的编码类型

**备选方案：**
1. **JSON-only**：Dart 端 `invokeMethod<T>` 返回的值始终被编码为字符串，导致 `channel.invokeMethod<int>()` 收到的不是 int 类型而抛出类型错误。
2. **链接 Flutter C++ client_wrapper**：需要维护大量源码依赖，且在 Flutter 3.38+ 版本中该 wrapper 已被移除，导致编译兼容性问题。

### ADR-3：延迟嵌入（tryEmbed）

**决策：** 不立即将 Flutter 视图嵌入 wxWindow，而是推迟到面板获得有效尺寸（> 1×1 像素）之后。

**背景：** 在 wxNotebook 中，非当前选中页面的子控件可能被分配 0×0 或 1×1 的分配尺寸（allocation）。如果 Flutter View 在这种状态下被嵌入，`embedInto()` 会回退到硬编码的默认尺寸（800×600）。

**理由：**
- 800×600 的默认尺寸在面板可见前会显示一帧（visible flicker），然后被第一个 `wxEVT_SIZE` 事件修正为实际尺寸。这个闪烁体验很差。
- 延迟嵌入直到尺寸有效可以确保 Flutter View 从一开始就以正确的尺寸渲染。
- `tryEmbed()` 是一次性操作（`m_embedded` 标志防止重复嵌入），不会引入性能开销。

**实现：**

```cpp
void FlutterPanel::tryEmbed() {
    if (m_embedded || !m_view) return;
    wxSize sz = GetSize();
    if (sz.GetWidth() <= 1 || sz.GetHeight() <= 1) return;
    m_view->embedInto(GetHandle());
    m_embedded = true;
}
```

**调用点：**
- `startView()` — 面板首次创建时检查
- `onSize()` — 尺寸变化时检查
- `onShow()` — 面板变为可见时检查

**备选方案：**
1. **立即嵌入并使用默认尺寸**：导致明显的视觉闪烁。
2. **在 Sizer 布局完成后嵌入**：难以可靠地检测"布局完成"——`wxEVT_SIZE` 可能在嵌入前多次触发，且每次触发都是不同的临时尺寸。
3. **隐藏 Flutter View 直到尺寸就绪**：增加复杂度，且隐藏/显示可能导致额外的 resize 和焦点问题。

### ADR-4：MSWShouldPreProcessMessage 重写

**决策：** 重写 `FlutterPanel::MSWShouldPreProcessMessage()`，对目标为 Flutter 子窗口 HWND 的消息返回 `false`。

**背景：** wxWidgets 的 `PreProcessMessage` 机制会遍历消息目标 HWND 的父窗口链，在找到的每个 wxWindow 上调用 `MSWTranslateMessage`。后者内部调用 `::TranslateMessage`，它会消费 `WM_KEYDOWN` 并生成 `WM_CHAR` 张贴到消息队列。

**问题链：**
1. 用户按下键盘 → `WM_KEYDOWN` 被发送到 Flutter 子窗口 HWND。
2. wxWidgets 的 `PreProcessMessage` 在 FlutterPanel 上调用 `MSWTranslateMessage`。
3. `::TranslateMessage` 消费 `WM_KEYDOWN` 并生成 `WM_CHAR`。
4. Flutter 的 `KeyboardManager` 收到孤立的 `WM_CHAR` 但没有配对的前置 `WM_KEYDOWN`。
5. 键盘输入处理失败——TextField 无法输入文字。

**解决方案：**

```cpp
bool FlutterPanel::MSWShouldPreProcessMessage(WXMSG* msg) {
    if (m_view) {
        HWND child = (HWND)m_view->nativeHandle();
        if (child && msg->hwnd == child)
            return false;  // 跳过 wxWidgets 预处理
    }
    return wxWindow::MSWShouldPreProcessMessage(msg);
}
```

返回 `false` 使整个预处理链跳过该消息，让原生 Windows 消息循环的 `::TranslateMessage` + `::DispatchMessage` 正常工作，确保 Flutter 子窗口的 WndProc 按正确顺序收到 `WM_KEYDOWN` 和随后的 `WM_CHAR`。

**备选方案：**
1. **MSWWindowProc 转发**：在 FlutterPanel 的 WindowProc 中将消息转发给 Flutter 子窗口。问题：`PeekMessage` 的工作方式使得通过 `DispatchMessage` 投递的消息无法被正确转发。
2. **Windows Hook**：使用 `SetWindowsHookEx` 过滤消息。过于侵入性，可能影响整个应用的键盘处理。
3. **避免重写 PreProcessMessage，改为在 Flutter 子窗口预翻译后再人工合成 WM_CHAR**：复杂且脆弱，容易遗漏边界情况。

### ADR-5：Linux 上的 GtkEventBox 包装

**决策：** 在 Linux 上，将 `FlView` 包装在 `GtkEventBox` 中再放入 `wxPizza`（即 `GtkFixed`）。

**背景：** `FlView` 基于 `GtkGLArea` 实现。GTK 的焦点传播规则规定：
- `GtkGLArea` 默认不可聚焦（`can-focus` 为 FALSE）。
- `GtkFixed`（wxPizza 的父类）不会将焦点传播给子控件。

因此，直接放入 wxPizza 的 FlView 完全无法接收焦点，导致键盘事件无法传递给 Flutter 面板。

**解决方案：**

```cpp
// flutter_host_linux.cpp embedInto() 中:
// 1. 创建 GtkEventBox 作为容器
m_event_box = gtk_event_box_new();
// 2. 将 FlView 放入 EventBox
gtk_container_add(GTK_CONTAINER(m_event_box), view_widget);
// 3. 设置可聚焦
gtk_widget_set_can_focus(m_event_box, TRUE);
gtk_widget_set_can_focus(view_widget, TRUE);
// 4. 将 EventBox 放入 wxPizza
pizza->put(m_event_box, 0, 0, w, h);
// 5. 抓取焦点
gtk_widget_grab_focus(view_widget);
```

**生命周期管理：**
- 如果发生重复嵌入（`embedInto` 被多次调用），旧的 EventBox 会被从 pizza 中移除，FlView 先从旧 EventBox 中解除父级关系，防止级联销毁。
- 使用 `g_object_add_weak_pointer` 在 EventBox 和 FlView 上分别注册弱引用，防止析构顺序问题导致悬空指针。

**备选方案：**
1. **直接在 FlView 上设置 can-focus**：被 `GtkGLArea` 的内部逻辑覆盖，不生效。
2. **继承 GtkFixed 创建自定义容器**：过度工程化。`GtkEventBox` 是 GTK 标准组件，专门为此类场景设计。

---

## 附录 A：文件索引

### C++ 嵌入层（src/slic3r/GUI/Flutter/）

| 文件 | 描述 |
|------|------|
| `flutter_host.h` | 抽象接口定义：`FlutterEngineHost`（引擎层）和 `FlutterViewHost`（视图层），以及工厂函数 `createFlutterEngine()` |
| `FlutterPanel.hpp` | `FlutterPanel` wxWidgets 控件声明 + `Dispatcher` 方法路由类 |
| `FlutterPanel.cpp` | `FlutterPanel` 实现：事件处理、焦点管理、延迟嵌入（tryEmbed）、平台特定代码（MSW） |
| `flutter_host_win.cpp` | Windows 平台实现：手动 StandardMethodCodec 编解码器、`FlutterViewHostWin`、`FlutterEngineHostWin` |
| `flutter_host_macos.mm` | macOS 平台实现（Objective-C++）：`FlutterViewHostMacOS`、`FlutterEngineHostMacOS`、Cocoa 集成 |
| `flutter_host_linux.cpp` | Linux 平台实现：`FlutterViewHostLinux`、`FlutterEngineHostLinux`、GTK/GObject 集成、EventBox 包装 |

### Flutter/Dart 应用（flutter_app/）

| 文件 | 描述 |
|------|------|
| `lib/main.dart` | Dart 端核心文件：入口函数定义（`main()`, `homeMain()`, `prepareMain()`）、PanelApp/HomePanelApp/PreparePanelApp Widget、MethodChannel 交互逻辑 |
| `pubspec.yaml` | Flutter 项目配置：名称、版本、Dart SDK 约束（^3.11.5）、依赖（flutter, cupertino_icons） |
| `pubspec.lock` | 锁定的依赖版本快照 |
| `analysis_options.yaml` | Dart linter 配置（继承 flutter_lints） |
| `test/widget_test.dart` | 基本 Widget 测试 |
| `linux/` | Linux Runner（独立 `flutter run` 使用）：CMake 构建、GTK 应用入口 |
| `windows/` | Windows Runner（独立 `flutter run` 使用）：CMake 构建 |
| `macos/` | macOS Runner（独立 `flutter run` 使用）：Xcode 项目、Swift 入口 |

### CMake 构建模块（cmake/modules/）

| 文件 | 描述 |
|------|------|
| `BuildFlutterApp.cmake` | `build_flutter_app()` 函数：调用 Flutter CLI 构建应用，将产物复制到构建目录。支持 macOS（App.framework）、Windows（flutter_app.dll + data）、Linux（libflutter_app.so + data） |
| `EnsureFlutterDeps.cmake` | `ensure_flutter_deps()` 函数：从 Flutter SDK 缓存中自动提取引擎头文件、库文件和 ICU 数据到 deps 目录 |

### 主项目集成

| 文件 | 关键 Flutter 代码 |
|------|-------------------|
| `src/CMakeLists.txt` | 调用 `BuildFlutterApp.cmake` 和 `EnsureFlutterDeps.cmake`，链接引擎库，复制产物，创建 `flutter_app` 构建目标 |
| `src/slic3r/CMakeLists.txt` | 包含 Flutter 源文件到 `libslic3r_gui` 目标，链接引擎库，设置头文件搜索路径 |
| `src/slic3r/GUI/MainFrame.hpp` | 声明 `m_flutter_test_panel`（`FlutterPanel*`）和 `m_flutter_engine`（`FlutterEngineHost*`）成员，定义 `tpFlutterTest` 标签页枚举值 |
| `src/slic3r/GUI/MainFrame.cpp` | 引擎创建、面板创建、Dispatcher 设置、startView 调用、标签页切换通知、引擎清理——完整的使用示例 |
| `src/slic3r/GUI/GUI_App.cpp` | 启动时打印 Flutter 版本日志 |
| `src/common_func/common_func.cpp` | `get_flutter_version()` 函数：读取 Flutter Web 资源的 version.json |

---

## 附录 B：平台 API 速查表

| 操作 | Windows | macOS | Linux |
|------|---------|-------|-------|
| **embedInto** | `SetParent(childHwnd, parentHwnd)` + `SetWindowPos` | `[cv addSubview:fv]` + `autoresizingMask` | `pizza->put(eventBox, ...)` + `gtk_widget_show_all` |
| **resize** | `SetWindowPos(hwnd, ..., w, h, SWP_NOZORDER \| ...)` | 空操作（autoresizingMask 自动处理） | `pizza->move()` + `pizza->size_allocate_child()` + `gtk_widget_set_size_request`×2 |
| **focus** | `::SetFocus(hwnd)` | `[window makeFirstResponder:view]` | `gtk_widget_grab_focus(viewWidget)` |
| **invokeMethod** | `FlutterDesktopMessengerSend()`（手动 StandardCodec 编码） | `[channel invokeMethod:arguments:]` | `fl_method_channel_invoke_method()`（自动类型检测） |
| **handler 注册** | `FlutterDesktopMessengerSetCallback()` + 手动消息解码 | `[channel setMethodCallHandler:]` | `fl_method_channel_set_method_call_handler()` |
| **view 创建** | `FlutterDesktopEngineCreate` + `EngineRun` + `ViewControllerCreate` | `[FlutterEngine runWithEntrypoint:]` + `FlutterViewController` | `fl_view_new()` + `FlMethodChannel` |
| **entrypoint** | 支持自定义（传 `entrypoint` 字符串给 `EngineRun`） | 支持自定义（传 `entrypoint` 给 `runWithEntrypoint:`） | 不支持（`fl_view_new` 总是运行 `main()`） |
| **Dart←C++ 参数类型** | 自动检测 int32/int64/float64/string | NSString/NSNumber 自动映射 | 自动检测 int64/float64/string |
| **C++←Dart 参数解码** | 手动 decodeValue() 支持 null/bool/int32/int64/float64/string | 通过 FlutterMethodCall.arguments 的 ObjC 类型 | fl_value_get_type() 支持 string/int/float/bool/null |
| **引擎共享** | 每个 View 独立的 Engine（随 Controller 创建/销毁） | 每个 View 独立的 Engine（共享 FlutterDartProject） | 每个 View 独立的 Engine（随 FlView 创建/销毁） |
| **焦点容器** | 原生 HWND 父子关系 | NSView 层级 | GtkEventBox 包装（提供焦点能力） |
| **键盘消息** | MSWShouldPreProcessMessage 拦截 | Cocoa 响应链 | GTK 事件传播 |

---

## 附录 C：StandardMethodCodec 编码表

Flutter 的 `StandardMethodCodec` 使用 `StandardMessageCodec` 作为底层消息编码，以下是在 Windows 嵌入层中手动实现的完整编码格式：

### 基本类型编码

| 类型 | 类型码 | 数据布局 | 数据大小 | 写支持 | 读支持 |
|------|--------|----------|----------|--------|--------|
| null | `0x00` | (无数据) | 1 byte | 否 | 是 |
| true | `0x01` | (无数据) | 1 byte | 否 | 是 |
| false | `0x02` | (无数据) | 1 byte | 否 | 是 |
| int32 | `0x03` | 4 bytes (小端, two's complement) | 5 bytes | 是 | 是 |
| int64 | `0x04` | 8 bytes (小端, two's complement) | 9 bytes | 是 | 是 |
| bigint | `0x05` | (未实现) | — | 否 | 否 |
| float64 | `0x06` | 8 bytes (IEEE 754, 小端) | 9 bytes | 是 | 是 |
| string | `0x07` | varint(len) + UTF-8 bytes | 1 + varint + len | 是 | 是 |
| uint8list | `0x08` | (未实现) | — | 否 | 否 |
| int32list | `0x09` | (未实现) | — | 否 | 否 |
| int64list | `0x0A` | (未实现) | — | 否 | 否 |
| float64list | `0x0B` | (未实现) | — | 否 | 否 |
| list | `0x0C` | (未实现) | — | 否 | 否 |
| map | `0x0D` | (未实现) | — | 否 | 否 |
| float32 | `0x0E` | (未实现) | — | 否 | 否 |

### varint 编码规则（7-bit 变长整数）

```
示例：300 的编码
  300 的二进制: 100101100 (9 bits)
  分组（7-bit）:
    group1: 0101100 (低 7 位)
    group2: 0000010 (高 2 位)
  编码:
    byte 0: 1 0101100 = 0xAC (最高位=1 表示后面还有)
    byte 1: 0 0000010 = 0x02 (最高位=0 表示结束)
  最终: [0xAC, 0x02]
```

### MethodChannel 消息格式

**Dart→C++ 调用（C++ 接收）：**
```
[ method_value ][ args_value ]
```

其中 `method_value` 是方法名字符串的标准编码，`args_value` 是参数的标准编码。

**C++→Dart 成功回复：**
```
[0x00][ result_value ]
```

**C++→Dart 错误回复：**
```
[0x01][ error_code_string ][ error_message_string ][ null(0x00) ]
```

### 自动类型检测规则（C++ writeValue）

当前 `writeValue()` 函数以字符串为输入，按以下优先级自动检测合适的编码类型：

```
1. 尝试 strtoll() 解析
   ├── 成功且在 int32 范围内 → 编码为 int32 (0x03)
   ├── 成功但超出 int32 范围 → 编码为 int64 (0x04)
   └── 失败 → 步骤 2
2. 尝试 strtod() 解析
   ├── 成功 → 编码为 float64 (0x06)
   └── 失败 → 步骤 3
3. 编码为 string (0x07)
```

**注意：** 对于空字符串 `""`，会被编码为 string 类型（长度 0），而非 null。null 类型仅在 Dart 端传递 null 或 C++ 端显式编码时使用，当前的 `writeValue()` 不生成 null。

### 字节序

所有多字节整数和浮点数均使用**小端序（little-endian）**编码，这是 Flutter StandardMethodCodec 的标准格式。

---

## 附录 D：环境设置

### Flutter SDK 安装

```bash
# macOS
brew install --cask flutter
# 或手动下载：https://docs.flutter.dev/get-started/install/macos

# Linux
# 手动下载 SDK：https://docs.flutter.dev/get-started/install/linux
# 推荐安装到 ~/flutter/
export PATH="$PATH:$HOME/flutter/bin"

# Windows
# 下载安装包或使用包管理器（如 scoop/choco）
scoop install flutter
# 或手动下载：https://docs.flutter.dev/get-started/install/windows
```

**版本要求：** 项目当前使用 Dart SDK ^3.11.5（参见 `pubspec.yaml`）。建议使用与 pubspec.lock 中锁定的 Flutter SDK 版本一致的版本。

### 平台前置条件

**macOS：**
- Xcode 15.0+（用于编译 macOS Flutter runner 和引擎）
- CocoaPods（如果使用插件）
- 引擎框架会自动从 Flutter SDK 的缓存中提取

```bash
# 预缓存 Flutter 引擎（确保可选平台引擎已下载）
flutter precache --macos
```

**Windows：**
- Visual Studio 2022（含 "Desktop development with C++" 工作负载）
- Windows 10 SDK（10.0.19041.0 或更高）

```bash
# 预缓存 Flutter 引擎
flutter precache --windows
```

**Linux：**
- GTK 3 development headers（`libgtk-3-dev` 或等效包）
- CMake, pkg-config, Ninja, Clang 或 GCC
- 引擎使用 OpenGL 软件渲染（通过 `FLUTTER_LINUX_RENDERER=software` 环境变量）

```bash
# Ubuntu/Debian
sudo apt install libgtk-3-dev cmake pkg-config ninja-build

# 预缓存引擎
flutter precache --linux
```

### CMake 配置选项

`BuildFlutterApp.cmake` 中的关键变量：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `FLUTTER_HOME` (环境变量) | — | 指定 Flutter SDK 路径。未设置时自动检测（Homebrew、PATH） |
| `BFA_FLUTTER_APP_DIR` | `${CMAKE_SOURCE_DIR}/flutter_app` | Flutter 项目目录路径 |
| `BFA_OUTPUT_DIR` | `${CMAKE_BINARY_DIR}/flutter_app` | 构建产物输出目录 |
| `FLUTTER_APP_FRAMEWORK` (macOS) | `${BFA_OUTPUT_DIR}/App.framework` | 编译后的 macOS 框架路径 |
| `FLUTTER_APP_DLL` (Windows) | `${BFA_OUTPUT_DIR}/flutter_app.dll` | 编译后的 Windows DLL 路径 |
| `FLUTTER_APP_SO` (Linux) | `${BFA_OUTPUT_DIR}/lib/libflutter_app.so` | 编译后的 Linux SO 路径 |

**强制重建 Flutter App：**
```bash
cmake --build build --target flutter_app_rebuild
```

此 target 会删除构建标记文件并重新执行 Flutter 构建。

---

## 附录 E：版本兼容性

### Flutter SDK

| 组件 | 版本 | 说明 |
|------|------|------|
| Dart SDK | ^3.11.5 | 定义在 `pubspec.yaml` 中 |
| Flutter SDK | 3.38+ | 兼容当前代码的最新验证版本 |
| flutter_lints | ^6.0.0 | 开发依赖 |
| cupertino_icons | ^1.0.8 | UI 图标库 |

### 平台依赖

| 平台 | 组件 | 最低版本 | 说明 |
|------|------|----------|------|
| 全部 | wxWidgets | 3.x | GUI 框架 |
| 全部 | CMake | 3.13+ | 构建系统 |
| Windows | Visual Studio | 2022 | C++ 编译器 |
| Windows | Windows SDK | 10.0.19041 | 系统 API |
| macOS | Xcode | 15.0 | 编译工具链 |
| macOS | macOS SDK | 11.3+ | 目标部署版本 |
| Linux | GTK 3 | 3.x | UI 工具包 |
| Linux | GLib | 2.x | GObject 类型系统 |
| Linux | C++ 编译器 | GCC 9+ 或 Clang 10+ | C++17 支持 |

### Flutter Engine API 变更注意事项

- **Flutter 3.38+**: Windows 平台的 `cpp_client_wrapper` 已被移除。`EnsureFlutterDeps.cmake` 中已有条件检查，仅在存在时复制 wrapper 头文件。
- **Flutter 3.38+**: `libflutter_engine.so` 在某些 Linux SDK 版本中不再独立存在（已合并到 `libflutter_linux_gtk.so`）。CMake 构建脚本中有条件检查。
- **Windows C API**: 使用 `flutter_windows.h` 中定义的平面 C API（`FlutterDesktop*` 函数族），而非已弃用的 C++ wrapper。

---

## 附录 F：FFI 完整示例

以下是一个完整的示例，展示如何通过 `dart:ffi` 从 Dart 直接访问 C++ 端的内存数据。

### C++ 端（导出到 Dart）

```cpp
// config_export.cpp — 编译到 OrcaSlicer 中
#include <cstdint>

// 使用 extern "C" 防止 C++ 名称修饰
extern "C" {

// 简单的 POD 结构体，packed 确保跨平台布局一致
struct __attribute__((packed, aligned(4))) ExportedConfig {
    int32_t  magic;              // 魔数，用于验证
    int32_t  version;            // 结构体版本
    float    nozzle_temp;        // 喷嘴温度 (Celsius)
    float    bed_temp;           // 热床温度 (Celsius)
    float    layer_height;       // 层高 (mm)
    int32_t  print_speed;        // 打印速度 (mm/s)
    int32_t  fan_speed;          // 风扇速度 (0-255)
};

// 全局实例（嵌入式场景中通过 MethodChannel 传递指针）
static ExportedConfig g_config = {
    0x4F524341,  // "ORCA" 魔数
    1,
    200.0f,      // 默认喷嘴温度
    60.0f,       // 默认热床温度
    0.2f,        // 默认层高
    60,          // 默认打印速度
    128          // 默认风扇速度
};

// 导出函数：返回配置结构体的指针（作为整数）
__attribute__((visibility("default")))
intptr_t get_config_pointer(void) {
    return reinterpret_cast<intptr_t>(&g_config);
}

// 导出函数：更新配置
__attribute__((visibility("default")))
void set_nozzle_temp(intptr_t ptr, float temp) {
    auto* cfg = reinterpret_cast<ExportedConfig*>(ptr);
    cfg->nozzle_temp = temp;
}

// 导出函数：验证魔数
__attribute__((visibility("default")))
int32_t validate_config(intptr_t ptr) {
    auto* cfg = reinterpret_cast<ExportedConfig*>(ptr);
    return (cfg->magic == 0x4F524341) ? 1 : 0;
}

} // extern "C"
```

### Dart 端（通过 FFI 访问）

```dart
// config_reader.dart — Flutter app 中使用
import 'dart:ffi';
import 'package:flutter/services.dart';

// FFI 类型定义 — 必须与 C++ 端的结构体布局完全匹配
final class ExportedConfig extends Struct {
  @Int32()
  external int magic;

  @Int32()
  external int version;

  @Float()
  external double nozzleTemp;

  @Float()
  external double bedTemp;

  @Float()
  external double layerHeight;

  @Int32()
  external int printSpeed;

  @Int32()
  external int fanSpeed;
}

// FFI 函数类型定义
typedef GetConfigPointerNative = IntPtr Function();
typedef GetConfigPointerDart = int Function();

typedef SetNozzleTempNative = Void Function(IntPtr ptr, Float temp);
typedef SetNozzleTempDart = void Function(int ptr, double temp);

typedef ValidateConfigNative = Int32 Function(IntPtr ptr);
typedef ValidateConfigDart = int Function(int ptr);

class CxxConfigBridge {
  late final DynamicLibrary _lib;
  late final GetConfigPointerDart _getConfigPointer;
  late final SetNozzleTempDart _setNozzleTemp;
  late final ValidateConfigDart _validateConfig;
  Pointer<ExportedConfig>? _ptr;
  MethodChannel? _channel;

  /// 通过 MethodChannel 获取指针（适合嵌入式场景，FFI 符号不可用时的备选）
  Future<void> initViaChannel(MethodChannel channel) async {
    _channel = channel;
    final ptrStr = await channel.invokeMethod<String>('getConfigPointer');
    if (ptrStr != null) {
      _ptr = Pointer<ExportedConfig>.fromAddress(int.parse(ptrStr));
    }
  }

  /// 通过 FFI 直接加载符号（适合独立运行或 FFI 符号可用时）
  void initViaFFI() {
    _lib = DynamicLibrary.open(''); // 加载宿主进程
    _getConfigPointer = _lib.lookupFunction<
        GetConfigPointerNative, GetConfigPointerDart>('get_config_pointer');
    _setNozzleTemp = _lib.lookupFunction<
        SetNozzleTempNative, SetNozzleTempDart>('set_nozzle_temp');
    _validateConfig = _lib.lookupFunction<
        ValidateConfigNative, ValidateConfigDart>('validate_config');

    final addr = _getConfigPointer();
    _ptr = Pointer<ExportedConfig>.fromAddress(addr);
  }

  /// 安全读取配置（带魔数验证）
  ExportedConfig? readConfig() {
    if (_ptr == null) return null;
    if (_validateConfig(_ptr!.address) != 1) {
      debugPrint('[FFI] Config validation failed — bad magic');
      return null;
    }
    return _ptr!.ref;
  }

  /// 更新喷嘴温度
  void setNozzleTemp(double temp) {
    if (_ptr != null) {
      _setNozzleTemp(_ptr!.address, temp);
    }
  }

  /// 获取当前喷嘴温度
  double get nozzleTemp {
    final cfg = readConfig();
    return cfg?.nozzleTemp ?? 0.0;
  }

  /// 获取当前打印速度
  int get printSpeed {
    final cfg = readConfig();
    return cfg?.printSpeed ?? 0;
  }
}
```

### 使用示例

```dart
// 在 Flutter Widget 中使用
class ConfigMonitor extends StatefulWidget {
  @override
  State<ConfigMonitor> createState() => _ConfigMonitorState();
}

class _ConfigMonitorState extends State<ConfigMonitor> {
  final CxxConfigBridge _bridge = CxxConfigBridge();
  Timer? _pollTimer;

  @override
  void initState() {
    super.initState();
    // 方式 1：通过 FFI 直接加载（需要符号可见）
    try {
      _bridge.initViaFFI();
    } catch (e) {
      debugPrint('FFI init failed: $e');
    }

    // 每 2 秒轮询一次配置
    _pollTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final cfg = _bridge.readConfig();
    return Card(
      child: Column(
        children: [
          Text('Nozzle: ${cfg?.nozzleTemp ?? "N/A"} °C'),
          Text('Bed: ${cfg?.bedTemp ?? "N/A"} °C'),
          Text('Speed: ${cfg?.printSpeed ?? "N/A"} mm/s'),
        ],
      ),
    );
  }
}
```

### 关键注意事项

1. **方法 1（FFI）vs 方法 2（MethodChannel）**：FFI 提供零拷贝访问，适合高频读取场景；MethodChannel 涉及编解码开销，适合低频控制。
2. **符号可见性**：C++ 函数必须导出（macOS/Linux: `__attribute__((visibility("default")))`, Windows: `__declspec(dllexport)`）才能被 `DynamicLibrary` 找到。
3. **内存安全**：Dart 端持有的指针在 C++ 对象被销毁后会变成悬空指针。需要确保指针的生命周期管理。
4. **结构体对齐**：使用 `packed` 属性和显式对齐值确保跨平台布局一致。在 Dart 端通过 `@Int32()`, `@Float()` 等注解匹配。
5. **类型映射**：

| C++ 类型 | Dart FFI 类型 |
|----------|---------------|
| `int32_t` | `@Int32() external int` |
| `int64_t` | `@Int64() external int` |
| `float` | `@Float() external double` |
| `double` | `@Double() external double` |
| `intptr_t` | `IntPtr` / `int` |
| `char*` | `Pointer<Utf8>` |

