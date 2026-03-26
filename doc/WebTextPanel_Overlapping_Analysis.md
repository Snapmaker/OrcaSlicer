# WebTextPanel 页面重叠问题分析

## 现象描述

在 test 标签页中，Flutter Web 应用（Flutter Shop v1.0.0）出现多页面/UI 元素重叠显示：
- 个人中心、我的订单、彩色块、商品详情等同时可见
- 类似“重影”或“双重曝光”
- 文字有重复或黄色高亮/阴影

## 根因分析

### 1. Flutter Web 路由/状态问题（最可能）

重叠内容均来自同一 Flutter 应用的不同页面：
- 个人中心（个人中心）
- 我的订单（我的订单）
- 商城/商品详情（12340 件、立即购买）
- 彩色块（橙、蓝、红、绿）

说明 **Flutter 应用内部** 在切换路由时，旧页面未被正确隐藏或移除，导致多路由/多页面同时渲染在同一视口内。

**可能原因：**
- 使用 `Navigator.push` 而非 `pushReplacement`，历史路由未正确 pop
- 使用 `Stack` + `IndexedStack` 或类似布局时，多个子页面同时保持可见
- 路由切换时未调用 `dispose` 或未正确清理 Overlay
- CSS `position`/`z-index` 导致多个页面层叠显示

### 2. WebView2 渲染缓冲/合成问题（次要）

WebView2 在 Windows 上存在已知问题：
- **重影/撕裂**：窗口 resize 时，WebView2 异步渲染与宿主合成不同步，旧缓冲未清空
- **Airspace 问题**：WebView2 使用 HwndHost，可能渲染在父窗口之上
- **间歇性渲染失败**：DOM 正常加载但画面不更新，需重启 msedgewebview2 进程

### 3. C++ 端 Notebook 行为

- `Notebook` 使用 `wxSHOW_EFFECT_NONE`，切换时立即 Hide/Show，无动画延迟
- 非选中页面会调用 `Hide()`，理论上不会同时显示多个 tab 内容
- `WebTextPanel` 在切换到 test  tab 时 **未** 收到 `page_state_notify_webview` 通知（与 Home、Device 不同）

## 修复建议

### A. Flutter Web 端（优先）

1. **检查路由管理**
   - 从首页 → 商城 → 个人中心 → 我的订单 的跳转逻辑
   - 使用 `pushReplacement` 或 `popAndPushNamed` 替代 `push`，避免路由栈堆积
   - 确保每个路由在离开时正确 `pop` 或 `dispose`

2. **检查布局结构**
   - 若使用 `Stack`，确保非当前页面 `visibility: hidden` 或 `Offstage`
   - 检查 `IndexedStack` 是否导致多个子页面同时渲染

3. **强制单路由**
   - 在 `/bridge` 路由下，确保只渲染一个主页面
   - 使用 `Navigator.of(context).popUntil()` 在进入新页面时清空栈

### B. C++ 端（辅助）

1. **切换 tab 时通知 WebView 并刷新**
   - 在 `MainFrame` 的 `wxEVT_BOOKCTRL_PAGE_CHANGED` 中，对 `m_web_text` 增加 active/inactive 通知
   - 切换到 test tab 时，可调用 `m_web_text->reload()` 或通过 OrcaBridge 发送 `tabActive` 消息，让 Flutter 端重置路由状态

2. **WebView2 强制重绘**
   - 在 WebTextPanel 显示时（如 `EVT_SHOW`），调用一次 `Refresh()` 或通过 JavaScript 触发 `window.location.reload()` 以清空缓冲

3. **设置不透明背景**
   - 确保 `WebTextPanel` 和 `m_browser` 的 `SetBackgroundColour` 为不透明色（当前已设置 `*wxWHITE`）

## 验证步骤

1. 在 Flutter 项目中添加路由切换日志，确认是否有多路由同时存在
2. 在浏览器中直接打开 `https://orca.local/...?path=/bridge`，观察是否也有重叠（排除 WebView2 问题）
3. 在 C++ 端添加 tab 切换时的 `reload()`，观察重叠是否减轻
