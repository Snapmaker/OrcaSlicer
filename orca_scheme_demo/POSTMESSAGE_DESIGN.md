# 自定义 Scheme 模式下的 PostMessage 通信设计

## 一、通信架构

在 orca:// scheme 模式下，WebView 与 C++ 的通信沿用 wxWidgets 的标准机制：

```
┌─────────────────────────────────────────────────────────────────────────┐
│  JavaScript (demo.html)                                                  │
│  ├── window.wx.postMessage(data)     → JS 发送给 C++                     │
│  └── window.addEventListener('message', fn)  → JS 接收 C++ 发来的消息     │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED
                                    │ WebView::RunScript("window.postMessage(...)")
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  C++ (WebTextPanel)                                                       │
│  ├── OnScriptMessage(evt)           → 接收 JS 发来的消息                   │
│  └── WebView::RunScript(m_browser, "window.postMessage(...)")  → 发送给 JS │
└─────────────────────────────────────────────────────────────────────────┘
```

### 1.1 前置条件

- `CreateWebViewWithLocalRoot` 已调用 `AddScriptMessageHandler("wx")`，页面中会存在 `window.wx`
- 需要绑定 `wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED` 到 `OnScriptMessage`

---

## 二、消息格式约定

建议使用 JSON 统一格式，便于扩展：

```json
{
  "type": "command_name",
  "seq": 12345,
  "payload": { ... }
}
```

| 字段 | 说明 |
|------|------|
| `type` | 消息类型/命令名 |
| `seq` | 可选，用于请求-响应配对 |
| `payload` | 可选，业务数据 |

---

## 三、JS → C++（页面 → 原生）

### 3.1 发送

```javascript
window.wx.postMessage(JSON.stringify({
  type: 'get_app_version',
  seq: Date.now(),
  payload: {}
}));
```

### 3.2 C++ 接收

```cpp
void WebTextPanel::OnScriptMessage(wxWebViewEvent& evt) {
    std::string msg = evt.GetString().ToUTF8().data();
    // 解析 JSON，按 type 分发处理
}
```

---

## 四、C++ → JS（原生 → 页面）

### 4.1 发送

```cpp
wxString js = wxString::Format(
    "window.postMessage(JSON.stringify("
    "{\"type\":\"app_notify\",\"payload\":{\"msg\":\"%s\"}}"
    "), '*');",
    wxEscape("Hello from C++"));
WebView::RunScript(m_browser, js);
```

### 4.2 JS 接收

```javascript
window.addEventListener('message', function(e) {
  if (e.source !== window) return
  try {
    var data = JSON.parse(e.data)
    if (data.type === 'app_notify') {
      console.log('From C++:', data.payload.msg)
    }
  } catch (err) {}
})
```

---

## 五、请求-响应配对（可选）

若需要 C++ 对 JS 的请求做同步回复：

1. JS 发送时带上 `seq`
2. C++ 处理完后，通过 `RunScript` 调用 `window.postMessage`，带上 `seq` 和 `reply`
3. JS 用 `seq` 匹配并调用对应回调

---

## 六、Windows 平台说明

- WebView2 (Edge) 与 WebKit (macOS) 均支持 `AddScriptMessageHandler` 和 `ExecuteScript`
- 消息格式与平台无关，仅需确保 JSON 序列化/反序列化正确
- 避免在 C++ 回调中执行耗时操作，可用 `wxGetApp().CallAfter` 延后处理
