# 实时检测高低温耗材混用 —— 实现计划

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**目标:** 当当前盘中任意耗材配置发生变更时（增/删/改槽位、切换耗材类型、修改支撑/填充/塔材分配），实时检测是否存在高低温耗材混用，若混用则在 GUI 弹出红色错误通知，阻止用户继续切片。同时移除 `Print::validate()` 中既有的高低温混用检测（它仅覆盖单喷嘴场景且有条件限制），由新的 plate-based 检测完全取代。

**架构:** 在现有的两个配置传播汇合点插入检测钩子——`Plater::on_config_change()`（覆盖全局配置变更）和 `Sidebar::update_presets()`（覆盖侧边栏耗材 combo 切换）。二者调用同一个轻量检查函数，该函数遍历当前 plate config 中所有**被实际引用**的 filament slot（wall/sparse/solid infill/support/interface/wipe tower/着色挤出机），收集对应的 `filament_type`，复用 `Print::check_multi_filaments_compatibility()` 做判定，结果通过 `NotificationManager` 呈现。最后移除 `Print::validate()` 中旧的 `check_multi_filament_valid()` 调用。

**涉及技术:** C++17, wxWidgets 事件系统, DynamicPrintConfig, NotificationManager, PresetBundle

---

## 背景分析

### 新老检测机制对比

| | 旧检测 (`Print::validate()`) | 新检测 (plate-based) |
|---|---|---|
| 触发时机 | 切片前，`Print::apply()` 之后 | 编辑阶段，配置变更后立即 |
| 触发条件 | `nozzles < 2 && extruders.size() > 1` | 无条件，任意耗材分配变更 |
| 数据来源 | `Print::extruders()`（全部可能用到的） | `p->config` 中实际被引用的 slot |
| 多喷嘴支持 | ❌ 不检测 | ✅ 支持 |
| 逐对象打印 | ❌ 不检测 | ✅ 支持 |
| 展现方式 | 切片时才报错 | 编辑阶段即弹红条 |

### 为什么需要两个 hook 点

| hook 点 | 覆盖场景 |
|---|---|
| `Plater::on_config_change()` | 新增/删除挤出机、导入项目、对象级参数变更（支撑/填充/塔材分配） |
| `Sidebar::update_presets()` | 侧边栏 filament combo 下拉切换耗材预设 |

侧边栏 combo 切换走 `Tab::select_preset()` → `Tab::on_presets_changed()` → `Sidebar::update_presets()`，**绕过了** `on_config_change`，因此必须双 hook。

### "当前盘内使用的耗材" 的精确定义

不依赖 `Print::extruders()`（它需要 `Print::apply()` 之后才有效，且返回"所有可能用到的"挤出机——当 support_filament=0 时会回退到全部 object extruders）。新检测**只收集当前盘配置中实际被引用的 slot**：

```
1-based 键: wall_filament, sparse_infill_filament, solid_infill_filament
0-based 键: support_filament, support_interface_filament, wipe_tower_filament
着色挤出机: ModelVolume::get_extruders() —— 多材料喷涂
```

对 0-based 键：值为 0 表示"沿用当前/默认材料"，**不引入新材料类型**，跳过。

### 检测逻辑

复用 `Print::check_multi_filaments_compatibility()`，它读取 `resources/info/filament_info.json` 的分类表，将每个 `filament_type` 归类为 HighTemp / LowTemp / HighLowCompatible，若 HighTemp 和 LowTemp 同时存在则返回 false。

---

## 实现步骤

### 任务 1：新增 `Plater` 检测方法

**目标:** 在 `Plater` 类中添加 `check_filament_temp_mixing()` 方法。

**文件:**
- 修改: `src/slic3r/GUI/Plater.hpp` —— 添加方法声明
- 修改: `src/slic3r/GUI/Plater.cpp` —— 添加方法实现

**步骤 1: 在 Plater.hpp 中添加声明**

在 `Plater` 类的 public 区域，与 `on_config_change` 声明附近：

```cpp
// Plater.hpp
/// 检查当前盘是否存在高低温耗材混用，基于 plate config 中实际被引用的 slot。
/// 返回 true 表示兼容（无混用）。
bool check_filament_temp_mixing();
```

**步骤 2: 在 Plater.cpp 中实现方法**

位置：`on_config_change` 附近（约 line 14565 之前或之后）：

```cpp
bool Plater::check_filament_temp_mixing()
{
    auto* ft_opt = p->config->option<ConfigOptionStrings>("filament_type");
    if (!ft_opt || ft_opt->values.empty())
        return true;

    int num_filaments = (int)ft_opt->values.size();
    std::set<int> used_slots;

    // —— 1-based 键：wall / sparse infill / solid infill ——
    static const char* keys_1based[] = {
        "wall_filament", "sparse_infill_filament", "solid_infill_filament"
    };
    for (auto key : keys_1based) {
        auto* opt = p->config->option<ConfigOptionInt>(key);
        if (opt && opt->value >= 1 && opt->value <= num_filaments)
            used_slots.insert(opt->value - 1);
    }

    // —— 0-based 键：support / support interface / wipe tower ——
    // 值为 0 表示 "沿用当前"，不引入新材料，跳过
    static const char* keys_0based[] = {
        "support_filament", "support_interface_filament", "wipe_tower_filament"
    };
    for (auto key : keys_0based) {
        auto* opt = p->config->option<ConfigOptionInt>(key);
        if (opt && opt->value >= 1 && opt->value <= num_filaments)
            used_slots.insert(opt->value - 1);
    }

    // —— ModelVolume 着色挤出机（多材料喷涂） ——
    for (const ModelObject* mo : wxGetApp().model().objects) {
        for (const ModelVolume* mv : mo->volumes) {
            for (int eid : mv->get_extruders()) {
                if (eid >= 1 && eid <= num_filaments)
                    used_slots.insert(eid - 1);
            }
        }
    }

    if (used_slots.empty())
        return true;

    // 收集对应的 filament_type
    std::vector<std::string> filament_types;
    for (int slot : used_slots)
        filament_types.push_back(ft_opt->get_at(slot));

    bool compatible = Print::check_multi_filaments_compatibility(filament_types);

    // —— 更新通知 ——
    if (!compatible) {
        StringObjectException err(
            STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP,
            _u8L("Cannot print multiple filaments which have large difference of "
                 "temperature together. Otherwise, the extruder and nozzle may "
                 "be blocked or damaged during printing.")
        );
        p->notification_manager->push_validate_error_notification(err);
    } else {
        p->notification_manager->close_notification_of_type(NotificationType::ValidateError);
    }

    return compatible;
}
```

**步骤 3: 确认依赖 include 存在**

`Plater.cpp` 中应有：
- `#include "libslic3r/Print.hpp"` — `Print::check_multi_filaments_compatibility`
- `#include "libslic3r/PrintBase.hpp"` — `STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP`
- `#include "libslic3r/Model.hpp"` — `ModelVolume::get_extruders()`

**步骤 4: 编译验证**

```bash
cmake --build build --target Snapmaker_Orca --config Release --parallel
```

---

### 任务 2：在 `Plater::on_config_change()` 末尾植入 hook

**目标:** 覆盖全局配置变更、导入项目、对象级参数变更。

**文件:** `src/slic3r/GUI/Plater.cpp`

在 `on_config_change` 函数体的最后一个 `}` 之前插入：

```cpp
    // 新增: 实时检测高低温耗材混用
    check_filament_temp_mixing();
}
```

---

### 任务 3：在 `Sidebar::update_presets()` 的 FILAMENT 分支植入 hook

**目标:** 覆盖侧边栏 filament combo 切换。

**文件:** `src/slic3r/GUI/Plater.cpp`

在 `Sidebar::update_presets()` 的 `case Preset::TYPE_FILAMENT:` 分支中，`update_dynamic_filament_list()` 之后、`break` 之前：

```cpp
        update_dynamic_filament_list();

        // 新增: 耗材切换后实时检测混用
        q->check_filament_temp_mixing();

        break;
```

（`q` 是 `Sidebar::priv` 中的 `Plater*` 指针。）

---

### 任务 4：移除 `Print::validate()` 中的旧检测

**目标:** 删除旧的高低温混用检测代码，由 plate-based 实时检测完全取代。

**文件:** `src/libslic3r/Print.cpp`

**步骤 1: 定位并删除**

找到 `Print::validate()` 中约 line 1096-1103：

```cpp
    // 删除以下整个 block ↓
    if (nozzles < 2 && extruders.size() > 1 && m_config.print_sequence != PrintSequence::ByObject) {
        auto ret = check_multi_filament_valid(*this);
        if (!ret.string.empty())
        {
            ret.type = STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP;
            return ret;
        }
    }
    // 删除结束 ↑
```

**步骤 2: 检查残留引用**

确认 `check_multi_filament_valid()` 是否还有其他调用者。如果没有，可选择保留函数本身（它只是 `check_multi_filaments_compatibility` 的薄封装，不删除也无害）。

**步骤 3: 编译验证**

```bash
cmake --build build --target Snapmaker_Orca --config Release --parallel
```

---

### 任务 5：边界情况验证

**验证清单:**

| # | 场景 | 操作 | 预期结果 |
|---|---|---|---|
| 1 | 空盘 | 不加载模型，仅切换 filament preset | 无错误通知 |
| 2 | 单材 | 只加载 PLA 模型 | 无错误通知 |
| 3 | 同类低温 | PLA + PLA-CF | 无错误通知 |
| 4 | 同类高温 | ABS + PC | 无错误通知 |
| 5 | 兼容型混低温 | PETG + PLA | 无错误通知（PETG 是 HighLowCompatible） |
| 6 | 明确混用 | PLA slot1, ABS slot2, wall→1, infill→2 | 红色错误通知 |
| 7 | 混用后修正 | PLA+ABS → 改 ABS 为 PETG | 错误通知消失 |
| 8 | 导入含混用 .3mf | 打开 PLA+ABS 项目 | 红色错误通知 |
| 9 | 支撑材混用 | 主体 PLA，support_filament→PC(slot2) | 红色错误通知 |
| 10 | wipe tower 混用 | PLA+PETG 双色，wipe_tower_filament→ABS | 红色错误通知 |
| 11 | 多材料着色混用 | 模型喷涂 PLA+ABS（不同 volume） | 红色错误通知 |
| 12 | 多喷嘴打印机混用 | 双头机，头1 PLA，头2 ABS | **红色错误通知**（新行为，旧的不检测） |

---

### 任务 6：编译完整测试

```bash
# 构建
cmake --build build --target Snapmaker_Orca --config Release --parallel

# 启动 GUI 按验证清单逐项测试
```

---

## 注意事项

1. **线程安全:** 所有调用均在 UI 线程，无需加锁。

2. **通知闪烁:** 每次 `check_filament_temp_mixing()` 返回 false 都会 push 新通知，即使之前已有。如果用户连续快速切换耗材导致闪烁，可加状态去重：

```cpp
// Plater 成员变量
bool m_last_filament_temp_mix_detected = false;

// 在 check_filament_temp_mixing() 中
if (compatible != m_last_filament_temp_mix_detected) {
    m_last_filament_temp_mix_detected = compatible;
    if (!compatible) { /* push */ }
    else            { /* close */ }
}
```

3. **CLI/Headless 路径:** 删除 `Print::validate()` 中的旧检测后，`orca-slice-engine` 的 CLI 切片将不再检查高低温混用。如果需要保留 CLI 侧的保护，可在 `Snapmaker_Orca.cpp` 的 CLI 入口处额外调用一次 `check_multi_filaments_compatibility()`，但 GUI 侧的实时检测已经能在用户切片之前阻止操作。

4. **Arrange.cpp 软约束:** `Arrange.cpp:636,653` 中的 `is_filaments_compatible` 调用**保留不动**。它用于自动排版评分，不是验证逻辑，与我们的改动不冲突。

5. **CalibrationWizardPresetPage:** 校准向导有独立的 `check_multi_filaments_compatibility` 调用 (`line 1075`)，用于校准选材阶段的校验。**保留不动**，与 plate 检测互不干扰。
