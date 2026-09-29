# GPU Picker 分支 Review:大模型导入白屏卡死问题定位 + 代码审查

- **分支**:`feature_gpu_pick_clean`(9 个提交,f354cd1d3b…8578d18ae1)
- **审查范围**:`GLCanvas3D.cpp/hpp`、`GLPickingBuffer.cpp/hpp`(新增)、`3DScene.cpp/hpp`、`SceneRaycaster.cpp/hpp`、`Plater.cpp`、`GUI_ObjectList.cpp`、`GLGizmosManager.cpp` 等,共约 2000 行改动
- **重点问题**:导入大模型(大场景 3MF)后首帧卡死、渲染区域白屏;小模型正常
- **日期**:2026-09-29

---

## TL;DR

**白屏卡死不是 GL 渲染内容的问题,也不是拾取(GPU picking)逻辑的问题。**
在本机完整复现后,通过帧缓冲像素探针证实:**场景(含 400 万三角形大模型)被正确渲染进了后备缓冲区,`SwapBuffers` 成功执行,但屏幕上始终显示纯色背景**。问题出在"呈现/窗口层"(哪个 GL 表面真正对应可见窗口),属于窗口布局/时序问题。

主要嫌疑(按可能性排序):

1. **WCP 设备面板 WebView 覆盖泄漏**——`Chrome_RenderWidgetHostHWND`(带 `WS_EX_TRANSPARENT` 点击穿透)整块盖在 3D 视图上,视觉白、穿透鼠标,与"白屏但 UI 可点"完全吻合;
2. 未初始化/备用的 GL 画布窗口被留在顶层,真正渲染的画布被压在下面;
3. 模态弹窗(版本提示/载入中/崩溃恢复)与标签页布局事件交错,导致页面可见性错乱。

分支改动的角色:大量渲染改为 overlay-only 快路径、删除了多处强制整帧渲染调用,**改变了事件时序,把潜伏的布局泄漏暴露出来**——这解释了"优化前正常、优化后必现"。分支 diff 本身未触碰任何 WebView/布局代码。

---

## 一、复现与证据链

### 复现方法

- 用脚本生成 400 万三角形球体 STL(200MB,直径 60mm),命令行参数启动导入;
- PowerShell 脚本 `PrintWindow`/`BitBlt` 截图 + 像素统计判断画面;
- 在 `render()` 关键路径插入 error 级 `[DBG]` 日志(渲染入口/早退分支/picking 更新/场景缓存决策/帧缓冲像素探针);
- minidump + PDB 符号化(`dbghelp` via Python)回溯全部线程栈。

### 关键证据

| # | 证据 | 结论 |
|---|------|------|
| 1 | `RenderMainSceneContent` 之后读后备缓冲中心像素 = `(68,70,70)`(热床背景)/导入后 `(198,108,153)`(球的耗材粉色) | **模型确实渲染进了后备缓冲,内容正确** |
| 2 | `SwapBuffers` 之前同一像素仍在;渲染循环日志显示 `end (swap ok)`,每帧 CPU 侧仅几毫秒 | 渲染机制正常,非性能卡死 |
| 3 | 主线程栈(minidump 符号化)= `wxEventLoopManual::DoRun → ProcessIdle → GetNextMessage`,CPU 空闲(15s 仅 0.11s) | **无死循环、无阻塞,主循环健康** |
| 4 | 鼠标扫过 3D 区后 `on_mouse(type=0)` 日志出现、渲染恢复 | 事件能到达 View3D 画布(弹窗关闭后) |
| 5 | 屏幕实测:3D 区域平涂 `#E7E7E7`(Orca 浅色主题背景色)/部分区域纯白,连渐变背景、热床网格都没有 | **屏幕呈现内容 ≠ 后备缓冲内容,呈现层断裂** |
| 6 | 某次会话 hit-test 3D 区域命中 `Chrome_RenderWidgetHostHWND`(WCP WebView,`WS_EX_TRANSPARENT`),rect `(0,67)-(2048,1112)` 盖住左栏+3D 区 | WebView 覆盖泄漏实锤(至少部分会话) |
| 7 | 空启动(不导入任何模型)同样白屏 | **导入大小不是必要条件**,时序才是 |
| 8 | 同一二进制 12:06 会话正常(用户做了单选测试),之后会话全部白屏 | 状态/时序依赖,非构建必现 |

### 对"大模型才复现"的解释(假说,待验证)

大文件导入 → 主线程长时间占用 + "载入中..."进度弹窗 + 版本弹窗/崩溃恢复弹窗依次出现 → 模态循环与 WCP WebView 初始化、标签页布局事件交错 → 某个画布/页面(WebView 或未初始化的备用 GL 画布)被留在顶层。小模型导入快,事件顺序正常,不触发。

### 排查中发现的环境地雷(与分支无关,main 同样中招)

每次 `taskkill /F` 强杀进程后,下次启动 `atomic_replace_directory` / `remove_all` 会因
`%APPDATA%\Snapmaker_Orca\system\OrcaFilamentLibrary(.old)`、`system\Snapmaker.old`、`printers.new`
被其它进程短暂占用(嫌疑:杀软/索引服务扫描新解压文件)而抛致命异常,弹"未处理异常"框——外观上与"卡死"高度相似。

> **测试时请正常退出程序,避免 taskkill /F;强杀后先删干净 `system\OrcaFilamentLibrary*`、`system\Snapmaker*` 再启动,或等 30 秒。**

---

## 二、分支代码审查发现(按严重度)

### [高] 1. 拾取渲染在 LOD 就绪前使用全分辨率网格整帧重画

位置:`GLCanvas3D.cpp:8752`(`_render_volumes_for_picking`),commit `00a27cde83`。

拾取改用 LOD 网格,但大模型导入后的**头几分钟 Middle/Small LOD 还在后台 QEM 简化,`simple_render` 走全分辨率 fallback**。此时每个 `m_pickingBufferDirty` 帧(导入、每次 LOD 晋升、裁剪面/缩放变化都会置脏)都对全场景再画一遍全分辨率。数百万三角形时单次数百毫秒到数秒,多对象 LOD 陆续晋升会造成连续失效→连续重渲——**这是"大模型导入后卡"的主要放大器**。

**建议**:活跃 LOD 未就绪时拾取走 CPU raycast(或跳过 GPU 拾取并标记 waiting),LOD 就绪后再切 GPU 拾取;对 LOD 晋升引起的失效做合并/防抖。

### [高] 2. 拾取渲染失败会每帧重试

位置:`GLCanvas3D.cpp:3103-3109`。

```cpp
m_pickingBufferDirty = !rendered;
```

若 `EnsureSize` / `BeginRender` 持续失败(纹理超限被 Reset 后反复重建),每帧都做完整的失败尝试。

**建议**:区分"尺寸不支持"(永久放弃)与"暂时失败"(指数退避);连续失败 N 次后降级 CPU raycast。

### [中] 3. Scene Cache 首次验证后不再检查 GL 错误

位置:`GLCanvas3D.cpp:8556-8634`(`CaptureSceneCache` / `PresentSceneCache`)。

`m_sceneCacheCaptureValidated` / `m_sceneCachePresentValidated` 置位后,后续所有 `glBlitFramebuffer` 都不再 `glGetError`。驱动侧 MSAA 样本数变化、表面重建等边缘失败会**静默产生陈旧/纯色帧**——正是"画面内容与后备缓冲不一致"这类症状的温床。

**建议**:present 后低频抽查一次 `glReadPixels` 对比,或定期(如每 N 帧/每次尺寸变化)重新验证;失败时立即 `InvalidateSceneCache()` 走全渲染。

### [中] 4. 常规渲染启用背面剔除是行为变更

位置:commit `00a27cde83`,`GLVolumeCollection::render` 中 `else glEnable(GL_CULL_FACE)`。

绕向(winding)不一致的网格——大场景合并件、扫描件很常见——会**部分消失**(以前双面渲染能显示)。预计会带来"模型破面/缺面"类回归反馈。

**建议**:做成开关;或首次加载网格时统计绕向,不一致者关剔除。

### [中] 5. `_picking_pass` 每帧两次同步 `glReadPixels`

`ReadColorRect`(小矩形)+ `ReadDepthPoint`(1px)都会打断 GPU 流水线。鼠标移动频繁时建议:合并为一次 PBO 异步读;或深度读推迟到确有颜色命中时。

### [低] 6. `render()` 重入保护把 `m_dirty` 置真

`GLCanvas3D.cpp:3008`。模态弹窗泵消息期间重入后脏标记残留,弹窗关闭后多渲一帧。无害但多余。

### [低] 7. `on_idle` 的 imgui extra-frame 连渲

实测出现过连续约 50 帧 `m_dirty=true` 连渲(`wxGetApp().imgui()->requires_extra_frame()` 反复触发,如通知淡入)。有界但浪费,建议对无实际变化的 extra frame 限频。

### [低] 8. `on_mouse` 中 imgui 消费事件后双重调度

`GLCanvas3D.cpp:5409-5422`:先 `render(false, overlayOnly)` 又 `SetOverlayAsDirty()` / `m_dirty = true`,一次动作两次调度。建议删一处。

---

## 三、结论与建议的下一步

### 结论

1. 分支的 GL 管线(picking buffer / scene cache / LOD picking)**在本机被证实渲染内容正确**;白屏是呈现/窗口层的断裂,与拾取算法本身无关。
2. 分支的真实风险点是上面 8 条,其中 #1、#2 直接解释"大模型导入卡顿",#3、#4 可能解释部分"画面异常"类反馈。
3. 白屏根因最大嫌疑是 **WCP WebView 覆盖泄漏 / 画布窗口层级错乱**——均为分支未触碰的代码,由分支改变渲染调度时序后暴露。

### 验证路径(预计 10 分钟闭环)

1. 复现白屏后,用 **Spy++/Inspect** 对白色区域 hit-test:
   - 命中 `Chrome_RenderWidgetHostHWND` / 非 `wxGLCanvas` → 坐实覆盖泄漏;
   - 命中 `wxGLCanvas` → 检查该画布像素格式(`GetPixelFormat`,正常应非 0)与 `m_canvas->GetHWND()` 是否一致。
2. 临时注释 WCP WebView 创建逻辑,重跑导入流程,观察白屏是否消失。
3. 用本文档日志插桩(`[DBG]` 前缀,error 级)观察:`pixel after main scene` 有值而屏幕白 → 直接进入窗口层排查,不必再查 GL。

### 附:本次排查留下的工具(`build/` 目录)

| 文件 | 用途 |
|------|------|
| `gen_big_sphere_np.py` | 生成指定三角形数的大网格 STL |
| `screenshot3.ps1` | 枚举进程窗口(类名/hwnd/rect)+ 截图 |
| `pixstats.ps1` | 区域像素统计(判断白屏/有内容) |
| `walk_dump.py` / `sym.py` | minidump 线程栈回溯 + PDB 符号化 |
| `[DBG]` 插桩(`GLCanvas3D.cpp`,44 行,未提交) | 渲染路径日志,定位完成后可整体还原 |

---

*Review by Claude Code, 2026-09-29*
