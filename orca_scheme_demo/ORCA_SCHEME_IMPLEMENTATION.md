# Orca 自定义 Scheme 实现文档

## 一、实现原理

### 1.1 核心思路

通过自定义 URL 协议 `orca://`，让 WebView 的静态资源请求由应用直接处理，而非走 HTTP 或 `file://`，实现：

- **按需读盘**：请求时从磁盘流式读取，不预加载到内存
- **无 HTTP 依赖**：不依赖 HttpServer，避免端口、代理等问题
- **路径隔离**：不暴露真实文件路径，由 handler 做映射

### 1.2 架构示意

```
┌─────────────────────────────────────────────────────────────────────┐
│  WebTextPanel                                                        │
│  ├── setup_temp_demo() → 创建 temp 目录，复制示例图片                    │
│  ├── CreateWebViewWithLocalRoot(rootPath) → 注册 orca:// handler     │
│  ├── 读取 resources/web/webtext/demo.html（由 CMake 从本目录复制）        │
│  └── SetPage(html, "orca://app/") → 注入 HTML，base 为 orca://app/    │
└─────────────────────────────────────────────────────────────────────┘
                                    │
        ┌───────────────────────────┼───────────────────────────┐
        │                           │                           │
        ▼                           ▼                           ▼
  HTML 直接注入              相对路径 img1.png            axios HTTP 请求
  (SetPage)                  → orca://app/img1.png        (外部 API)
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│  OrcaLocalHandler::GetFile("orca://app/img1.png")                     │
│  ├── 解析 path → img1.png                                             │
│  ├── fullPath = rootPath + img1.png                                   │
│  ├── new wxFileInputStream(fullPath)  ← 按需打开，流式返回              │
│  └── return wxFSFile(stream, uri, "image/png", ...)                  │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.3 关键设计

| 设计点 | 方案 | 原因 |
|--------|------|------|
| HTML 加载 | `SetPage(html, "orca://app/")` | wxWebView 对自定义 scheme 返回的 HTML 的 MIME 处理有缺陷，会以纯文本显示 |
| Base URL | `orca://app/` | 相对路径（如 `img1.png`）解析为 `orca://app/img1.png`，由 handler 提供 |
| 资源路径 | temp 目录 = orca root | 图片等资源从 temp 按需读取，不占应用内存 |
| HTML 来源 | `orca_scheme_demo/demo.html` | 本目录为 demo 验证的集中存放处，CMake 构建时复制到 resources |

---

## 二、解决的问题

### 2.1 与 HTTP 方案对比

| 问题 | HTTP (localhost) | orca:// |
|------|------------------|---------|
| 代理/防火墙 | 易被拦截、改端口 | 不受影响 |
| 端口占用 | 需固定端口、健康检查 | 无 |
| 大文件传输 | 易整段进内存 | 流式按需读 |
| 启动依赖 | 需 HttpServer 就绪 | 无额外服务 |
| 文件访问 | 需经 HTTP 或 JS 桥接 | 直接由 C++ 读盘 |

### 2.2 与 file:// 方案对比

| 问题 | file:// | orca:// |
|------|---------|---------|
| 路径暴露 | 暴露真实路径 | 虚拟路径 |
| 扩展性 | 仅本地文件 | 可接 API、逻辑 |
| Service Worker | 受限 | 可支持 |
| 跨域 | 同源策略复杂 | 可控 |

### 2.3 适用场景

- 生产环境静态资源加载
- 大文件、流式传输
- 代理/网络环境复杂
- 需要直接访问本地文件且不依赖 HTTP

---

## 三、文件结构

```
OrcaSlicer/
├── orca_scheme_demo/                    # demo 验证集中目录（本目录）
│   ├── demo.html                        # 主页面（HTML+CSS+JS）
│   ├── ORCA_SCHEME_IMPLEMENTATION.md    # 实现文档
│   └── README.md                        # 本目录说明
├── src/slic3r/GUI/
│   ├── OrcaLocalHandler.hpp/cpp         # 自定义 scheme handler
│   ├── WebTextPanel.hpp/cpp             # webText 标签页
│   └── Widgets/WebView.cpp              # CreateWebViewWithLocalRoot()
└── resources/web/webtext/
    └── demo.html                        # 由 CMake 从 orca_scheme_demo 复制
```

---

## 四、代码职责

| 文件 | 职责 |
|------|------|
| `OrcaLocalHandler` | 实现 `GetFile(uri)`，将 `orca://app/path` 映射到 root 目录，按需读盘返回 `wxFSFile` |
| `WebView::CreateWebViewWithLocalRoot` | 创建 WebView 并注册 `OrcaLocalHandler("orca", rootPath)` |
| `WebTextPanel` | 创建 temp 目录、准备图片，创建 WebView，从 `demo.html` 读取 HTML 并 `SetPage` |
| `demo.html` | 页面结构、样式、orca 图片加载、axios HTTP 请求示例 |

---

## 五、URL 规范

- 格式：`orca://{host}/{path}`
- 示例：`orca://app/img1.png` → `{rootPath}/img1.png`
- 安全：禁止 `..` 路径穿越

---

## 六、扩展方向

1. **orca://api/**：提供文件列表、配置等接口
2. **多 root**：不同 host 映射不同目录
3. **跨平台**：Windows/Linux 使用对应原生 API 实现
