# Widget Inspector：控件点选与 AI 定位任务生成器

Widget Inspector 是一个内置调试工具，用于解决“用自然语言描述界面控件不准确”的问题。选中界面控件后，它可以生成结构化 AI Locator，并把用户任务、定位规则和控件信息组合成可直接发送给 AI/Agent 的提示词。

> 当前该能力默认不编译；只有显式传入 `-DSLIC3R_WIDGET_INSPECTOR=ON` 的 Windows 构建才会包含。

---

## 1. 开启方式

Widget Inspector 仅在编译期显式启用：

```text
-DSLIC3R_WIDGET_INSPECTOR=ON
```

普通构建默认不编译该功能。带有该开关的 Windows 构建启动后会直接安装 Widget Inspector，并可通过 `Ctrl + Shift + F12` 切换点选模式。

---

## 2. 基本使用流程

1. 启动应用。
2. 按 `Ctrl + Shift + F12` 进入点选模式。
3. 移动鼠标，目标 wxWidgets 控件会被红色边框高亮。
4. 点击目标控件。
5. 弹出 `Widget Inspector` 窗口。
6. 在左侧 `Agent task` 输入框中填写任务。
7. 点击 `Generate && Copy Agent Prompt`。
8. 将剪贴板内容直接粘贴给 Agent。

再次按任一快捷键可退出点选模式。快捷键在 Widget Inspector 启用后全局生效，即使尚未进入点选模式也会被该调试工具处理。

---

## 3. 弹窗布局

弹窗分为左右两栏。

### 左栏：Agent task

左栏只保留给 AI/Agent 使用的内容：

```text
Agent task
[任务输入框]
[Generate && Copy Agent Prompt]
```

示例任务：

```text
这个按钮什么时候可以变亮？
```

或：

```text
定位这个控件的点击事件，并解释它的数据来源。
```

点击按钮后，系统会自动生成完整提示词并复制到剪贴板。

### 右栏：Debug info

右栏面向人工排查，展示完整调试信息：

```text
Debug info
[只读调试信息]
[Copy Debug Info]
```

其中包含：

- 控件 C++ 类型
- wxWidgets 类型
- ID / name / label / tooltip / 当前文本值
- 语义 ID
- 父窗口链
- 相邻控件
- DPI 与几何信息
- 临时对象地址、事件处理器地址、HWND

---

## 4. AI Locator 包含什么

生成给 Agent 的定位信息主要包括：

```yaml
semantic:
  app_view
  navigation
  target_role
  semantic_id

identity:
  cpp_class
  wx_class
  id
  name
  label
  tooltip
  value
  child_index
  same_class_index
  same_class_count

locator:
  strategy
  chain

neighbors:
  previous_sibling
  next_sibling

geometry:
  content_scale
  screen_rect_px
  rect_relative_to_top_window_logical
  rect_relative_to_parent_logical
  normalized_rect_in_parent
```

Agent 会优先使用 `semantic_id`、`target_role`、实际 C++ 类型和业务索引定位源码，而不是依赖屏幕坐标。

---

## 5. 典型场景示例

以“预览 -> 耗材管理 -> 耗材丝”为例，选中第 4 个耗材下拉框后，生成的定位信息会包含：

```yaml
target_role: filament_preset_combo
semantic_id: plater.sidebar.filament_management.row[3].preset_combo
filament_index_0_based: 3
filament_index_1_based: 4
```

其中：

- `row[3]` 是 0-based；
- `filament_index_1_based: 4` 表示这是第 4 个耗材丝；
- 右侧相邻的颜色按钮会带有类似 `row[3].color_picker` 的语义；
- 编辑按钮会带有类似 `row[3].edit_button` 的语义。

这样可以明确区分“耗材丝 4”到底指：

- 颜色按钮；
- 耗材预设下拉框；
- 编辑按钮；
- 整行容器。

---

## 6. 使用结果截图

> 在此处补充你的使用结果截图。

---

## 7. 注意事项

1. 该能力主要用于开发与问题排查，不建议普通用户开启。
2. 注册表开关为当前用户级别，不需要管理员权限。
3. 修改开关后需要重启应用。
4. 屏幕坐标、对象地址、HWND 均为临时调试信息，不能作为稳定定位依据。
5. Debug info 会显示被选中控件的 `identity.value`，包括文本输入框中的当前内容；完整报告不会写入应用日志。仍不要在包含密钥、令牌或其他敏感信息的控件上使用该工具或复制 Debug info。
6. ImGui、WebView 内部元素、菜单项和 AUI Toolbar item 不一定都是普通 `wxWindow`：
   - AUI Toolbar 会额外输出 item ID、label、kind；
   - WebView 只能定位到外层 wxWebView；
   - ImGui 只能定位到承载它的 wxCanvas/面板。
7. 生成 Agent 提示词时，请尽量描述业务问题，例如“为什么这个按钮不可点击”、“找到点击处理函数”、“分析状态更新来源”，而不是只描述控件外观。
