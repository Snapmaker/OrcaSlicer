# macOS Flutter Web 白屏问题调查报告

## 问题描述

部分 macOS 用户在使用 Snapmaker Orca 时，首页（path=1）和设备页（path=2）出现白屏。

- macOS 26+ 用户触发概率明显更高
- 其他 macOS 版本也会出现，但频率低很多
- Safari/WKWebView 会触发，Chrome 不会
- 在 Web Inspector 中禁用缓存后重新加载可以恢复正常

## 架构背景

```
┌─────────────────────────────────────────────┐
│  Snapmaker Orca (C++ 应用)                   │
│                                              │
│  ┌──────────────┐    ┌───────────────────┐  │
│  │  HttpServer   │    │  WKWebView        │  │
│  │  (boost::asio)│    │  (wxWebViewWebKit)│  │
│  │  127.0.0.1:   │    │                   │  │
│  │  13619        │◄───│  加载 Flutter Web  │  │
│  └──────────────┘    └───────────────────┘  │
└─────────────────────────────────────────────┘
```

应用内置了一个 HTTP 服务器（`HttpServer.cpp`），在 `127.0.0.1:13619` 上为 Flutter Web 资源提供服务。WKWebView 通过 HTTP 请求加载 `index.html`、`main.dart.js`、字体文件等资源。

## 根因分析

### 直接原因

字体文件 `NanumGothic-Regular.ttf`（4MB）的 HTTP 请求 hang 住，无响应。Flutter Web 页面等待字体加载，导致白屏。

### 根本原因：HTTP/1.1 Keep-Alive 协议违规

HTTP 服务器返回 `HTTP/1.1` 响应，但**不支持 keep-alive**（每个请求处理完后立即关闭 TCP 连接）。同时，响应中**没有声明 `Connection: close`**。

根据 HTTP/1.1 规范（RFC 7230），`HTTP/1.1` 默认启用 keep-alive。服务器如果不支持持久连接，**必须**在响应中声明 `Connection: close`。

```
// HttpServer.cpp - 修复前的响应
HTTP/1.1 200 OK
Content-Type: application/x-font-ttf
Content-Length: 4091796
Access-Control-Allow-Origin: *
                                    ← 缺少 Connection: close
```

### 触发机制：TCP 关闭时序竞态

```
  浏览器                                 服务器
    |                                      |
    |---- GET main.dart.js (5MB) -------->|
    |<--- 200 OK + 5MB data -------------|
    |                                      | server.stop() → FIN 发出
    |                                      |
    |  [Flutter 执行，触发字体加载]         |
    |                                      |
    |---- GET NanumGothic.ttf ----------->|  ← 在 FIN 到达之前发出
    |<--- FIN ----------------------------|  ← FIN 到了，但请求已发到死连接
    |                                      |
    |       请求无响应，hang，白屏          |
```

**关键**：Safari/WebKit 没有收到 `Connection: close`，认为连接可复用。如果下一个请求在服务器的 FIN 包到达之前发出，请求会发到一个已关闭的连接上，永远收不到响应。

### 为什么 Chrome 不受影响

Chrome/Chromium 检测到 TCP 连接被对端关闭（RST/FIN）后，会**自动在新连接上重试**请求。Safari/WebKit 更严格，不做自动重试。

### 为什么 macOS 26 更频繁

新版 WebKit 可能更积极地复用连接或改变了连接池策略，增大了"请求在 FIN 之前发出"的概率窗口。

### 为什么只有首页和设备页

- 所有页面加载同一个 `index.html` + `main.dart.js`
- 首页和设备页渲染时需要加载 `NanumGothic-Regular.ttf`（4MB）和 `HarmonyOS_Sans_SC_Regular.ttf`（8MB）字体文件
- 默认空页面或其他简单页面不触发字体加载
- 字体请求发生在 Flutter 渲染的密集请求期，竞态窗口更大

### 缓存如何放大问题

禁用缓存后问题消失的原因：

**有缓存时**（容易触发）：
```
1. index.html → 服务器请求 → 返回 → 关连接
2. main.dart.js → 缓存命中，不走网络（0ms）
3. Flutter 瞬间执行，立刻请求字体
4. 字体请求可能发到步骤 1 的"死连接"上 → hang
```

**禁用缓存时**（不触发）：
```
1. index.html → 服务器请求 → 返回 → 关连接
2. main.dart.js → 服务器请求 → 5MB 传输，耗时较长
3. 传输期间，步骤 1 的连接早已彻底关闭
4. Flutter 执行，请求字体 → 新连接 → 正常
```

缓存让 `main.dart.js`（5MB）的传输时间"消失"了，压缩了请求间隔，使竞态更容易触发。

## 修复方案

在所有 HTTP 响应中添加 `Connection: close` 头。

### 修改文件

`src/slic3r/GUI/HttpServer.cpp` — 4 处响应构造位置：

1. **OPTIONS 响应**（约 line 144）
2. **302 Redirect 响应**（约 line 788）
3. **404 Not Found 响应**（约 line 802）
4. **200 File 响应**（约 line 859）

### 修改内容

每处响应头的 `\r\n`（空行）之前添加：

```cpp
ssOut << "Connection: close\r\n";
```

### 效果

Safari 收到 `Connection: close` 后，知道连接不可复用，每个请求都会新建 TCP 连接。由于是 `127.0.0.1` 本地回环，TCP 握手开销是微秒级，对用户无感知。

## 附：HTTP 缓存相关问题

### 现状

HTTP 服务器没有返回任何缓存控制头（无 `Cache-Control`、`ETag`、`Last-Modified`）。浏览器会使用**启发式缓存**——自行决定是否缓存资源以及缓存多久。

这意味着：
- 服务端资源更新后，浏览器可能仍然使用旧的缓存版本
- 缓存过期时间不可控，由浏览器内部策略决定
- 所有主流浏览器（Safari、Chrome/Edge、Firefox）都有此行为，符合 HTTP 规范

### 建议后续优化

在 `ResponseFile::write_response` 中添加缓存控制头：

```cpp
ssOut << "Cache-Control: no-cache\r\n";
```

`no-cache` 的含义是"可以缓存，但每次使用前必须向服务器验证"。这能让浏览器缓存生效（不用每次重新下载），同时保证资源更新时能及时获取。

但当前服务器不支持条件请求（不返回 `ETag`/`Last-Modified`，不处理 `If-None-Match`/`If-Modified-Since`），所以验证时会返回完整的 200 + 文件内容，等同于每次都重新下载。

**完整的缓存优化路径**（非本次修复范围）：
1. 响应带 `ETag`（如文件内容 hash）和 `Last-Modified`
2. 解析请求中的 `If-None-Match` / `If-Modified-Since` 头
3. 资源未变时返回 `304 Not Modified`（空 body），节省传输

## 排除的错误方向

| 方向 | 排除原因 |
|------|---------|
| Metal/GPU 着色器损坏 | Flutter Web 使用 `renderer:"html"`，纯 DOM/CSS，不涉及 GPU |
| App Transport Security (ATS) | index.html 和外部页面均可正常加载，ATS 未阻断 |
| Info.plist 未嵌入 | 同上，网络请求正常 |
| wxURI URL 编码损坏 | URL 中的字符均在允许字符集内 |
| Service Worker 阻断 | SW 的 RESOURCES key 不匹配导致 fall-through，非阻断 |
| 端口硬编码不匹配 | 独立 bug，但不是白屏的直接原因 |
