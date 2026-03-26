# orca:// Scheme Demo 验证目录

本目录集中存放 orca 自定义 scheme 的 demo 验证相关文件，便于维护和扩展。

## 目录内容

| 文件 | 说明 |
|------|------|
| `demo.html` | 主页面，包含 orca 图片加载、axios HTTP、PostMessage 双向通信示例 |
| `ORCA_SCHEME_IMPLEMENTATION.md` | 实现原理、架构、解决的问题、文件结构 |
| `POSTMESSAGE_DESIGN.md` | PostMessage 通信设计（JS ↔ C++） |
| `README.md` | 本目录说明 |

## 构建说明

- `demo.html` 会在 CMake 配置阶段被复制到 `resources/web/webtext/`，供运行时加载
- 修改本目录下的 `demo.html` 后，重新运行 CMake 即可生效

## 相关 C++ 代码

- `src/slic3r/GUI/OrcaLocalHandler.hpp/cpp` - scheme handler
- `src/slic3r/GUI/WebTextPanel.hpp/cpp` - webText 标签页
- `src/slic3r/GUI/Widgets/WebView.cpp` - CreateWebViewWithLocalRoot
