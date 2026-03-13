# Rib擦除塔GUI控制逻辑补充文档

## 概述
本文档补充了之前调研中遗漏的GUI层面的配置控制逻辑，特别是BambuStudio中当启用rib wall时对其他配置项的锁定/禁用机制。

---

## 关键发现：BambuStudio的GUI控制逻辑

### 1. 配置项命名差异

| BambuStudio | OrcaSlicer | 说明 |
|-------------|-----------|------|
| `prime_tower_rib_wall` (bool) | `wipe_tower_wall_type` (enum) | Bambu用bool开关，Orca用枚举（Rectangle/Cone/Rib） |
| `prime_tower_width` | `prime_tower_width` | 相同 |
| `prime_tower_rib_width` | `wipe_tower_rib_width` | 前缀不同 |
| `prime_tower_extra_rib_length` | `wipe_tower_extra_rib_length` | 前缀不同 |
| `prime_tower_fillet_wall` | `wipe_tower_fillet_wall` | 前缀不同 |

### 2. BambuStudio的关键控制逻辑

**文件**: `src/slic3r/GUI/ConfigManipulation.cpp` (lines 848-858)

```cpp
bool have_prime_tower = config->opt_bool("enable_prime_tower");
for (auto el :
     {"prime_tower_width", "prime_tower_brim_width", "prime_tower_skip_points",
      "prime_tower_rib_wall", "prime_tower_infill_gap", "prime_tower_enable_framework",
      "prime_tower_max_speed"})
    toggle_line(el, have_prime_tower);

bool have_rib_wall = config->opt_bool("prime_tower_rib_wall") && have_prime_tower;
for (auto el : {"prime_tower_extra_rib_length", "prime_tower_rib_width", "prime_tower_fillet_wall"})
    toggle_line(el, have_rib_wall);

// ⚠️ 关键：当启用rib wall时，禁用width编辑
toggle_field("prime_tower_width", !have_rib_wall);
```

**逻辑说明**:
1. 当`enable_prime_tower`为false时，隐藏所有擦除塔相关配置
2. 当`prime_tower_rib_wall`为true时：
   - 显示rib相关配置（`extra_rib_length`, `rib_width`, `fillet_wall`）
   - **禁用`prime_tower_width`编辑**（变为只读/灰色）
3. 原因：rib模式下，width由算法自动计算以保持正方形

### 3. OrcaSlicer的当前控制逻辑

**文件**: `src/slic3r/GUI/ConfigManipulation.cpp` (lines 760-781)

```cpp
bool have_prime_tower = config->opt_bool("enable_prime_tower");
for (auto el : {"prime_tower_width", "prime_tower_brim_width"})
    toggle_line(el, have_prime_tower);

// 使用不同的配置系统
for (auto el : {"wipe_tower_rotation_angle", "wipe_tower_cone_angle",
                "wipe_tower_extra_spacing", "wipe_tower_max_purge_speed",
                "wipe_tower_wall_type",
                "wipe_tower_extra_rib_length","wipe_tower_rib_width","wipe_tower_fillet_wall",
                "wipe_tower_bridging", "wipe_tower_extra_flow",
                "wipe_tower_no_sparse_layers"})
  toggle_line(el, have_prime_tower && !is_BBL_Printer);

// ⚠️ 缺失：没有根据wall_type禁用width的逻辑
```

**问题**:
- 没有根据`wipe_tower_wall_type`的值来控制其他字段
- 当选择Rib类型时，`prime_tower_width`仍然可编辑
- 缺少rib相关参数的显示/隐藏控制

---

## 需要添加的GUI控制逻辑

### 修改1: ConfigManipulation.cpp - 添加wall_type控制

**文件**: `src/slic3r/GUI/ConfigManipulation.cpp`
**位置**: 在line 781之后添加

**添加代码**:
```cpp
// 根据wall_type控制相关字段
if (have_prime_tower && !is_BBL_Printer) {
    auto wall_type = config->opt_enum<WipeTowerWallType>("wipe_tower_wall_type");

    // Rib相关参数只在Rib模式下显示
    bool is_rib_wall = (wall_type == WipeTowerWallType::Rib);
    for (auto el : {"wipe_tower_extra_rib_length", "wipe_tower_rib_width", "wipe_tower_fillet_wall"})
        toggle_line(el, is_rib_wall);

    // Cone相关参数只在Cone模式下显示
    bool is_cone_wall = (wall_type == WipeTowerWallType::Cone);
    toggle_line("wipe_tower_cone_angle", is_cone_wall);

    // ⚠️ 关键：Rib模式下禁用width编辑（自动计算）
    toggle_field("prime_tower_width", !is_rib_wall);
}
```

**影响**:
- Rib模式：显示rib参数，隐藏cone参数，**禁用width编辑**
- Cone模式：显示cone参数，隐藏rib参数，允许width编辑
- Rectangle模式：隐藏rib和cone参数，允许width编辑

---

## 配置验证和约束

### 修改2: 添加配置验证逻辑

**文件**: `src/libslic3r/PrintConfig.cpp`
**位置**: 在rib相关配置定义之后（约line 5816）

**添加验证**:
```cpp
// Rib width约束：必须 <= min(tower_depth, tower_width) / 2
def = this->add("wipe_tower_rib_width", coFloat);
def->label = L("Rib width");
def->tooltip = L("Width of diagonal rib walls. Must not exceed half of tower dimensions.");
def->sidetext = L("mm");
def->min = 0;
def->max = 300;
def->mode = comAdvanced;
def->set_default_value(new ConfigOptionFloat(10.0));

// Extra rib length约束
def = this->add("wipe_tower_extra_rib_length", coFloat);
def->label = L("Extra rib length");
def->tooltip = L("Additional length for rib extensions beyond diagonal. "
                 "Large values may cause boundary violations.");
def->sidetext = L("mm");
def->min = 0;
def->max = 300;
def->mode = comAdvanced;
def->set_default_value(new ConfigOptionFloat(0.0));
```

### 修改3: 运行时验证（WipeTower2.cpp）

**文件**: `src/libslic3r/GCode/WipeTower2.cpp`
**位置**: generate()函数中，约line 2393

**当前代码**:
```cpp
m_rib_length = std::max({m_rib_length, std::sqrt(std::pow(m_wipe_tower_depth, 2.f) + std::pow(m_wipe_tower_width, 2.f))});
m_rib_length += m_extra_rib_length;

m_rib_width = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) / 2.f);
```

**增强验证**:
```cpp
// 计算rib长度
m_rib_length = std::max({m_rib_length, std::sqrt(std::pow(m_wipe_tower_depth, 2.f) + std::pow(m_wipe_tower_width, 2.f))});
m_rib_length += m_extra_rib_length;

// 约束rib宽度：不超过塔尺寸的一半
m_rib_width = std::min(m_rib_width, std::min(m_wipe_tower_depth, m_wipe_tower_width) / 2.f);

// ⚠️ 新增：验证extra_rib_length不会导致严重越界
float max_safe_extra_length = std::min(m_wipe_tower_width, m_wipe_tower_depth) * 0.5f;
if (m_extra_rib_length > max_safe_extra_length) {
    BOOST_LOG_TRIVIAL(warning) << "wipe_tower_extra_rib_length (" << m_extra_rib_length
                               << "mm) exceeds safe limit (" << max_safe_extra_length
                               << "mm), clamping to safe value";
    m_extra_rib_length = max_safe_extra_length;
    m_rib_length = std::sqrt(std::pow(m_wipe_tower_depth, 2.f) + std::pow(m_wipe_tower_width, 2.f)) + m_extra_rib_length;
}
```

---

## 缺少的BambuStudio配置参数

### 1. Auto Brim Width（自动边缘宽度）

**BambuStudio实现**:
```cpp
// PrintConfig.cpp
def->gui_type = ConfigOptionDef::GUIType::f_enum_open;
def->min = -1;
def->enum_values.push_back("-1");
def->enum_labels.push_back(L("Auto"));
def->set_default_value(new ConfigOptionFloat(3.0));

// Print.cpp (line 2918)
if (m_config.prime_tower_brim_width < 0)
    const_cast<Print *>(this)->m_wipe_tower_data.brim_width =
        WipeTower::get_auto_brim_by_height(max_height);
```

**OrcaSlicer当前**:
```cpp
def->min = 0;  // 不支持负值
```

**需要添加**:
1. 允许`prime_tower_brim_width = -1`表示自动模式
2. 实现`WipeTower::get_auto_brim_by_height()`方法
3. 在Print.cpp中添加自动计算逻辑

### 2. 其他缺少的参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `prime_tower_skip_points` | bool | true | 跳过某些点以减少擦除塔高度 |
| `prime_tower_flat_ironing` | bool | false | 换刀前平整表面 |
| `prime_tower_infill_gap` | float | 100% | 填充间隙百分比 |
| `prime_tower_enable_framework` | bool | false | 启用框架模式（空心塔） |
| `prime_tower_max_speed` | float | 0 | 最大速度（0=无限制） |
| `prime_tower_lift_speed` | float | 0 | 抬升速度 |
| `prime_tower_lift_height` | float | -1 | 抬升高度（-1=自动） |

**优先级**: 这些参数是可选的高级功能，不影响核心rib功能。

---

## 实施优先级

### P1 - 必须实现 🔴

1. **ConfigManipulation.cpp**: 添加wall_type控制逻辑
   - 根据wall_type显示/隐藏相关参数
   - **Rib模式下禁用prime_tower_width编辑**
   - 预计工作量: 30分钟

2. **WipeTower2.cpp**: 增强运行时验证
   - 验证extra_rib_length不超过安全限制
   - 预计工作量: 20分钟

### P2 - 应该实现 🟡

3. **PrintConfig.cpp**: 完善配置定义
   - 添加tooltip说明约束条件
   - 添加min/max限制
   - 预计工作量: 15分钟

4. **Auto Brim Width**: 实现自动边缘宽度
   - 允许-1值表示自动
   - 实现get_auto_brim_by_height()
   - 预计工作量: 1小时

### P3 - 可选实现 🟢

5. **高级参数**: 添加BambuStudio的其他参数
   - skip_points, flat_ironing等
   - 预计工作量: 2-3小时

---

## 测试要求

### GUI测试
1. **Wall Type切换**
   - 切换到Rib → prime_tower_width变灰（不可编辑）
   - 切换到Rectangle → prime_tower_width可编辑
   - 切换到Cone → prime_tower_width可编辑

2. **参数显示/隐藏**
   - Rib模式：显示rib_width, extra_rib_length, fillet_wall
   - Cone模式：显示cone_angle
   - Rectangle模式：隐藏所有特殊参数

3. **验证提示**
   - 输入过大的extra_rib_length → 显示警告
   - 输入过大的rib_width → 自动限制到max值

### 功能测试
1. **Width自动计算**
   - Rib模式下，width应由算法计算
   - 验证计算结果保持正方形

2. **配置保存/加载**
   - 保存Rib配置 → 加载后正确恢复
   - 旧配置文件兼容性测试

---

## 修改文件总结

### 新增修改文件

| 文件 | 修改内容 | 优先级 | 预计工作量 |
|------|---------|--------|-----------|
| `src/slic3r/GUI/ConfigManipulation.cpp` | 添加wall_type控制逻辑 | 🔴 P1 | 30分钟 |
| `src/libslic3r/GCode/WipeTower2.cpp` | 增强运行时验证 | 🔴 P1 | 20分钟 |
| `src/libslic3r/PrintConfig.cpp` | 完善配置定义 | 🟡 P2 | 15分钟 |
| `src/libslic3r/Print.cpp` | 实现auto brim计算 | 🟡 P2 | 1小时 |

### 总工作量估算

- **P1关键修复**: 50分钟
- **P2完整实现**: 1小时15分钟
- **P3可选功能**: 2-3小时

**总计**: 约4-5小时（包含测试）

---

## Git提交策略

### Commit 1: GUI控制逻辑
```
feat: Add wall_type-based GUI control for wipe tower parameters

- Show/hide rib parameters based on wall_type selection
- Disable prime_tower_width editing in Rib mode (auto-calculated)
- Show/hide cone_angle in Cone mode
- Aligns GUI behavior with BambuStudio

Resolves: #[issue-number]
```

### Commit 2: 运行时验证增强
```
feat: Add runtime validation for rib wipe tower parameters

- Validate extra_rib_length against safe limits
- Log warnings when parameters exceed safe values
- Auto-clamp to safe values to prevent boundary violations

Resolves: #[issue-number]
```

### Commit 3: Auto Brim Width（可选）
```
feat: Implement auto brim width calculation for wipe tower

- Support prime_tower_brim_width = -1 for auto mode
- Calculate brim width based on tower height
- Aligns with BambuStudio behavior

Resolves: #[issue-number]
```

---

## 验收标准

### GUI验收
- [ ] Rib模式下prime_tower_width不可编辑（灰色/只读）
- [ ] Rectangle/Cone模式下prime_tower_width可编辑
- [ ] Rib参数只在Rib模式下显示
- [ ] Cone参数只在Cone模式下显示
- [ ] 参数切换流畅，无UI闪烁

### 功能验收
- [ ] Rib模式下width自动计算正确
- [ ] 过大的extra_rib_length被限制
- [ ] 配置保存/加载正确
- [ ] 旧配置文件兼容

### 用户体验验收
- [ ] 用户无法输入导致越界的参数
- [ ] 有清晰的tooltip说明约束
- [ ] 警告信息友好易懂

---

## 与之前文档的关系

本文档是对以下文档的补充：
- `rib_wipetower_research.md` - 核心算法调研
- `rib_wipetower_files_to_modify.md` - 核心实现文件修改清单

**完整修改清单** = 核心实现（5个修改点）+ GUI控制（4个修改点）

---

**文档版本**: 1.0
**创建日期**: 2026-03-11
**作者**: Claude (基于GUI控制逻辑调研)
