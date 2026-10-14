# OrcaSlicer 多燕尾榫切割工具改动说明

## 文档目的

本文档用于说明“多燕尾榫切割工具”需求的实现内容、代码改动范围、功能影响范围和验证情况，方便需求、测试和代码评审人员理解本次变更。

## 需求背景

原有 OrcaSlicer 的切割工具在“榫槽切割”模式下，一次只能生成一个燕尾榫结构。对于尺寸较大的模型，如果只用一个燕尾榫来辅助拆件后的重组，定位和连接强度都可能不够理想。

本次需求是在保留原有单燕尾榫能力的基础上，增加“多个燕尾榫”的能力。用户可以在切割工具中设置燕尾榫数量和间距，让一次切割生成多个均匀分布的燕尾榫，方便大模型拆分后更准确地对齐、拼装和固定。

## 用户侧功能变化

在切割工具选择“榫槽切割”模式后，新增 Multiple 分组，包含以下参数：

- Count：燕尾榫数量。
- Gap：相邻燕尾榫之间的间距。
- Spacing：只读显示值，表示相邻燕尾榫中心分布相关的实际槽距，计算方式为 Gap + 单个燕尾榫外沿宽度。

当 Count 为 1 时，功能表现应与原来的单燕尾榫切割保持一致。此时 Gap 没有实际意义，因此 UI 上会置灰。

当 Count 大于 1 时，3D 预览中会显示多个燕尾榫预览平面，执行切割后会生成对应数量和位置的凸榫/凹槽结构。

## 典型使用流程

1. 导入一个尺寸较大的模型。
2. 打开切割工具。
3. 选择“榫槽切割”模式。
4. 在 Multiple 分组里设置 Count 和 Gap。
5. 在 3D 视图中确认多个燕尾榫的预览位置。
6. 执行切割。
7. 将拆分后的部件分别摆放打印。
8. 打印完成后，通过多个燕尾榫进行对齐和重组。

## 代码改动范围

本次功能主要涉及切割几何逻辑和切割工具 UI/预览逻辑。

### `src/libslic3r/CutUtils.hpp`

- 扩展 `perform_with_groove` 接口，新增多燕尾榫相关参数：
  - `groove_count`
  - `groove_gap`
  - `radius`
- 新增 `calculate_groove_width` 声明，用于计算单个燕尾榫在切割平面上的外沿宽度。
- 为 `calculate_groove_width` 增加 Doxygen 注释，说明用途、参数和返回值。

### `src/libslic3r/CutUtils.cpp`

- 将原来的单燕尾榫切割逻辑扩展为多燕尾榫循环。
- 每个燕尾榫根据 Count 和 Gap 计算 X 方向偏移量。
- 每个燕尾榫独立执行隔离切割、角度切割和公差切割。
- 使用与预览一致的单榫外沿宽度计算方式，保证 UI 预览和实际切割结果的间距一致。
- 对输入参数增加兜底处理：
  - `groove_count` 小于 1 时按 1 处理。
  - `groove_gap` 小于 0 时按 0 处理。
  - `radius` 小于等于 0 时使用默认正值兜底，避免产生异常几何。
- 修复 review 中指出的临时燕尾榫对象所有权问题：
  - 不再使用未释放的裸 `ModelObject*` 克隆对象。
  - 改为使用局部 `Model` 管理临时对象生命周期，避免对象泄漏。
- 修复 review 中指出的类型和风格问题：
  - `float(...)` 改为 `static_cast<float>(...)`。
  - `-.5` 改为 `-0.5f`。
  - 多榫循环附近的新代码改为 Allman brace 风格。

### `src/slic3r/GUI/Gizmos/GLGizmoCut.hpp`

- 新增多燕尾榫 UI 状态字段：
  - `m_groove_count`
  - `m_groove_count_init`
  - `m_groove_gap`
  - `m_groove_gap_init`
- 新增 Count、Gap、Spacing 相关 UI 渲染函数声明。
- 新增偏移索引辅助逻辑声明，用于预览多个燕尾榫位置。

### `src/slic3r/GUI/Gizmos/GLGizmoCut.cpp`

- 在榫槽切割 UI 中增加 Multiple 分组。
- 新增 Count 输入控件，限制范围为 1 到 100。
- 新增 Gap 输入控件，Count 为 1 时禁用。
- 新增只读 Spacing 显示，展示 Gap + 单榫外沿宽度。
- 预览平面由单个燕尾榫扩展为多个燕尾榫。
- 预览顶点校验逻辑同步支持多个燕尾榫，避免切割平面偏离中心后仍按单个燕尾榫位置判断，导致预览错误变红。

## 影响范围

### 受影响功能

- 切割工具中的“榫槽切割”模式。
- 榫槽切割的 3D 预览。
- 榫槽切割实际生成的模型几何。
- 与榫槽切割相关的有效性判断。

### 不应受影响功能

以下功能不是本次需求目标，理论上不应因本次改动产生行为变化：

- 普通平面切割。
- 轮廓切割。
- Dowel / Snap 等其他连接器模式。
- 非榫槽切割下的模型摆放、切片、导出流程。
- 原有 Count = 1 的单燕尾榫切割结果。

## 兼容性说明

为了降低迁移风险，本次实现保留了原有单燕尾榫路径的行为。默认 Count 为 1，因此用户不主动设置多燕尾榫时，功能应与旧版本保持一致。

多燕尾榫功能是在原有榫槽切割逻辑上扩展循环和偏移，不改变其他切割模式的数据结构和交互方式。

## Review 修复说明

AI Harness Review 提到的主要问题及处理如下：

- 临时 `groove_object` 可能泄漏：已改为局部 `Model` 托管临时对象生命周期。
- `float(...)` 类型转换风格问题：已改为 `static_cast<float>(...)`。
- `-.5` 隐式 double 到 float 收窄：已改为 `-0.5f` 并配合显式转换。
- 新增循环括号风格问题：已改为 Allman brace 风格。
- `calculate_groove_width` 缺少注释：已增加 Doxygen 注释。
- 参数缺少校验：已对 Count、Gap、Radius 做防御性兜底。
- `add_volumes_from_cut` 是否重复追加体积：当前切割 helper 每次切割前都会重置 `tmp_model_for_cut = Model()`，不会沿用上一轮临时切割结果。

其中 `perform_with_groove` 参数数量超过 5 个属于风格建议。由于该接口是当前切割调用链的一部分，若为了该建议额外引入 options struct，会扩大接口改动范围。本次为降低风险，暂不做结构性重构。

## 验证情况

已完成的本地验证：

- `libslic3r` Release 构建通过，覆盖本次核心几何逻辑改动。
- 完整 `Snapmaker_Orca_app_gui` Release 构建曾编过 `libslic3r` 和 `libslic3r_gui`，但最后 app 目标阶段遇到 VS 编译中间文件 `vc143.pdb` 写入冲突失败。

该失败属于 Windows / Visual Studio 多编译进程同时写入同一个 PDB 文件的构建环境问题，与本次功能代码语法或链接逻辑无直接关系。建议完整包验证时关闭其他 Visual Studio 构建进程，或清理对应中间文件后重新构建。

建议测试重点：

- Count = 1 时，切割结果与原单燕尾榫一致。
- Count = 2、3、5 时，预览和实际切割都生成对应数量的燕尾榫。
- 调整 Gap 后，预览间距和实际切割间距一致。
- Count = 1 时，Gap 控件置灰。
- 切割平面偏离模型中心时，多燕尾榫有效性判断正常，不应误判为红色无效预览。
- 回归普通平面切割、轮廓切割、Dowel / Snap、Undo / Redo。

## 提交信息

相关分支：

- `codex/multi-dovetail-cut-port`

相关提交：

- `f70ed6c308 feat: support multiple dovetail cuts`
- `599b9f5af8 fix: address dovetail cut review comments`

相关 MR：

- `http://gitlab.s.com/snapmaker_orca/sn_geomesh/orca/OrcaSlicer/-/merge_requests/10`
