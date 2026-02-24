# OrcaSlicer 耗材-挤出机映射与参数覆盖机制

## 概述

OrcaSlicer 支持多耗材、多挤出机系统，其中耗材（Filament）是逻辑概念，挤出机（Extruder）是物理概念。在 8 耗材、4 挤出机的配置中，耗材 5-8 通过映射机制复用物理挤出机 1-4。

```
┌─────────────────────────────────────────────────────────────┐
│                    逻辑层 (Filaments)                        │
│   F1    F2    F3    F4    F5    F6    F7    F8              │
│    │     │     │     │     │     │     │     │              │
│    │     │     │     │     │     │     │     │              │
│    ▼     ▼     ▼     ▼     │     │     │     │              │
│   ┌──────────────────┐     │     │     │     │              │
│   │   映射机制 (Map)  │◄────┘     │     │     │              │
│   └──────────────────┘           │     │     │              │
│            │                     │     │     │              │
│            ▼                     ▼     ▼     ▼              │
│   ┌─────────────────────────────────────────────┐          │
│   │              物理层 (Extruders)              │          │
│   │      E1         E2         E3         E4    │          │
│   └─────────────────────────────────────────────┘          │
└─────────────────────────────────────────────────────────────┘
```

## 核心概念

### 1. 耗材 (Filament)
- **定义**: 用户配置的材料预设，包含材料类型、颜色、打印参数等
- **数量**: 可配置多个（如 8 个）
- **标识**: `filament_idx` (0-indexed)
- **参数来源**: 耗材配置文件或用户覆盖

### 2. 挤出机 (Extruder)
- **定义**: 物理硬件，执行实际的挤出操作
- **数量**: 受硬件限制（如 4 个）
- **标识**: `extruder_idx` 或 `physical_extruder_idx` (0-indexed)
- **参数来源**: 打印机配置

### 3. 映射关系 (Filament-Extruder Map)
- **存储**: `std::unordered_map<int, int> m_filament_extruder_map`
- **含义**: `m_filament_extruder_map[filament_idx] = physical_extruder_idx`
- **示例**: 8 耗材 → 4 挤出机
  ```
  F1 → E1 (0 → 0)
  F2 → E2 (1 → 1)
  F3 → E3 (2 → 2)
  F4 → E4 (3 → 3)
  F5 → E1 (4 → 0)  // 复用 E1
  F6 → E2 (5 → 1)  // 复用 E2
  F7 → E3 (6 → 2)  // 复用 E3
  F8 → E4 (7 → 3)  // 复用 E4
  ```
- **回退策略**: 当映射不存在时，使用 `filament_idx % physical_extruder_count`

## 参数覆盖机制

### 配置参数分类

| 类型 | 存储位置 | 示例参数 |
|------|----------|----------|
| 打印机级 | PrintConfig | machine_max_speed, bed_temperature |
| 挤出机级 | PrintConfig (数组) | retraction_length, z_hop, wipe_distance |
| 耗材级 | DynamicPrintConfig | filament_diameter, filament_temperature |

### 挤出机回抽参数

以下参数既可以在挤出机级别设置，也可以在耗材级别覆盖：

```
retraction_length      - 回抽长度
z_hop                  - Z抬升高度
z_hop_types            - Z抬升类型 (Auto/Normal/Slope/Spiral)
wipe_distance          - 擦拭距离
retraction_speed       - 回抽速度
deretraction_speed     - 回填速度
retract_when_changing_layer - 换层时回抽
wipe                   - 启用擦拭
...
```

### 参数继承规则

```
┌─────────────────────────────────────────────────────────────┐
│                     参数优先级 (从高到低)                     │
├─────────────────────────────────────────────────────────────┤
│  1. 耗材覆盖 (用户勾选)     → 使用耗材自己的值               │
│  2. 映射挤出机默认值        → 从 m_filament_extruder_map 继承│
│  3. 模运算回退              → filament_idx % extruder_count  │
└─────────────────────────────────────────────────────────────┘
```

**用户勾选覆盖**：
- 在 UI 中勾选参数覆盖 checkbox
- 内部表示：`ConfigOptionVector<T>` 中对应元素非 nil

**未勾选覆盖**：
- 从映射的物理挤出机继承值
- 内部表示：元素为 nil，触发继承逻辑

## 关键实现文件

### 1. Config.hpp - 配置系统核心

```cpp
// ConfigOptionVector 基类
class ConfigOptionVectorBase {
    virtual bool nullable() const { return false; }
    virtual bool is_nil(size_t idx) const { return false; }
};

// 带映射的 apply_override 重载
template<typename T>
class ConfigOptionVector : public ConfigOptionVectorBase {
    bool apply_override(const ConfigOption *rhs, const std::vector<int>& map_indices) override {
        // 保存物理挤出机默认值
        std::vector<T> default_values = this->values;

        for (size_t i = 0; i < rhs_vec->size(); ++i) {
            if (!rhs_vec->is_nil(i)) {
                // 用户勾选覆盖：使用耗材自己的值
                this->values[i] = rhs_vec->values[i];
            } else {
                // 未勾选：从映射的物理挤出机继承
                this->values[i] = default_values[map_indices[i]];
            }
        }
        return true;
    }
};
```

### 2. PrintApply.cpp - 映射处理核心

```cpp
// 构建映射索引
std::vector<int> map_indices;
map_indices.resize(filament_count, 0);
for (size_t i = 0; i < filament_count; ++i) {
    auto it = filament_extruder_map.find((int)i);
    if (it != filament_extruder_map.end()) {
        map_indices[i] = it->second;
    } else {
        map_indices[i] = (int)(i % physical_extruder_count);  // 回退
    }
}

// 应用映射
opt_copy->apply_override(opt_new_filament, map_indices);
```

### 3. GCodeProcessor.hpp/cpp - G-code 处理

```cpp
// 获取物理挤出机 ID
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    if (it != m_filament_extruder_map.end()) {
        return it->second;
    }
    // 统一回退策略
    return filament_idx % static_cast<int>(m_physical_extruder_count);
}
```

### 4. CoolingBuffer.hpp/cpp - 冷却缓冲

```cpp
// 同样使用模运算回退
int get_physical_extruder(int filament_idx) const {
    auto it = m_filament_extruder_map.find(filament_idx);
    if (it != m_filament_extruder_map.end()) {
        return it->second;
    }
    return filament_idx % static_cast<int>(m_physical_extruder_count);
}
```

### 5. GCode.cpp - G-code 生成

```cpp
// 初始化 CoolingBuffer 的映射
m_cooling_buffer->set_filament_extruder_map(writer_mapping);
m_cooling_buffer->set_physical_extruder_count(physical_extruder_count);
```

## 数据流程图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           切片流程                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  1. 初始化阶段                                                           │
│     Print::initialize_filament_extruder_map()                           │
│     └── 分析对象使用的耗材，建立 filament_idx → extruder_idx 映射       │
│                                                                          │
│  2. 配置处理阶段 (PrintApply.cpp)                                        │
│     print_config_diffs()                                                 │
│     ├── 检测 needs_physical_mapping 条件                                │
│     │   ├── is_extruder_retract_param = true                            │
│     │   ├── has_filament_overrides = opt->nullable()                    │
│     │   └── !filament_extruder_map.empty()                              │
│     ├── 构建 map_indices 数组                                           │
│     └── 调用 apply_override(opt_filament, map_indices)                  │
│                                                                          │
│  3. 清理阶段 (PrintApply.cpp)                                            │
│     ├── 检查 new_full_config 中的原始 filament_xxx 配置                 │
│     ├── 确定 has_any_user_override                                      │
│     └── 只清除没有用户覆盖的参数                                         │
│                                                                          │
│  4. G-code 生成阶段                                                      │
│     GCode::process_layer()                                              │
│     ├── 设置 CoolingBuffer 的映射                                        │
│     └── GCodeProcessor 使用 get_physical_extruder() 获取参数            │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## 历史问题与修复

### 问题 1: is_nil() 逻辑错误

**现象**: 所有耗材都没勾选覆盖时，映射完全不生效

**原因**:
```cpp
// 错误代码
bool has_filament_overrides = !opt_new_filament->is_nil();
// is_nil() 在所有元素都为 nil 时返回 true
// 导致 !is_nil() = false，跳过映射处理
```

**修复**:
```cpp
// 正确代码
bool has_filament_overrides = opt_new_filament->nullable();
// 检查类型是否支持 nullable，而不是检查值
```

### 问题 2: 清理逻辑检查错误的数据源

**现象**: 修改一个参数（如 wipe_distance）导致另一个已勾选的参数（如 z_hop_types）被错误清除

**原因**:
```cpp
// 错误代码：检查 filament_overrides 中的值
ConfigOption* override = filament_overrides.option(key);
if (override_vec->nullable()) { ... }  // 始终为 false
// filament_overrides 存储的是 apply_override() 的结果（挤出机类型，非 nullable）
```

**修复**:
```cpp
// 正确代码：检查 new_full_config 中的原始 filament 配置
std::string filament_key = "filament_" + key;
const ConfigOption* filament_opt = new_full_config.option(filament_key);
if (filament_opt && filament_opt->nullable()) {
    // 检查原始 nil 标记
}
```

### 问题 3: 回退策略不一致

**现象**: GCodeProcessor 和 CoolingBuffer 使用不同的回退策略

**修复**: 统一使用模运算回退
```cpp
return filament_idx % static_cast<int>(m_physical_extruder_count);
```

### 问题 4: CoolingBuffer 缺少映射初始化

**现象**: CoolingBuffer 创建后未设置映射

**修复**:
```cpp
m_cooling_buffer->set_filament_extruder_map(writer_mapping);
m_cooling_buffer->set_physical_extruder_count(physical_extruder_count);
```

## 测试验证

### 测试场景

| 测试 | 配置 | 预期结果 |
|------|------|----------|
| 基础映射 | 8 耗材，F5-8 未勾选覆盖 | F5→E1, F6→E2, F7→E3, F8→E4 |
| 覆盖保留 | F1-4 勾选覆盖，修改挤出机参数 | F1-4 保持自己的值 |
| 参数独立 | 修改 wipe_distance | z_hop_types 不受影响 |
| 动态继承 | F5 未勾选，修改 E1 参数 | F5 自动继承新值 |

### 验证工具

使用 G-code 分析脚本验证：

```bash
python scripts/analyze_gcode_params.py output.gcode
```

输出示例：
```
[Mapping Verification]
  T4 (F5 → E1): Z-hop avg=0.520mm, expected≈0.4mm [OK]
  T4 (F5 → E1): Retract avg=1.226mm, expected≈1.51mm [OK]
```

## 调试指南

### 关键日志标记

| 标记 | 含义 |
|------|------|
| `DEBUG_NEEDS_MAPPING_CHECK` | 映射条件检查 |
| `DEBUG_BUILD_MAP_INDICES` | 构建 map_indices |
| `DEBUG_MAP_INDICES_RESULT` | map_indices 结果 |
| `DEBUG_APPLY_OVERRIDE_CALL` | apply_override 调用 |
| `DEBUG_APPLY_OVERRIDE_INPUT` | 输入值 |
| `DEBUG_APPLY_OVERRIDE_RESULT` | 结果值 |
| `DEBUG_USER_OVERRIDE_FOUND` | 发现用户覆盖 |
| `DEBUG_FILAMENT_OVERRIDE_KEEP` | 保留参数 |
| `DEBUG_FILAMENT_OVERRIDE_ERASE` | 清除参数 |

### 启用调试日志

代码中使用 `BOOST_LOG_TRIVIAL(error)` 输出调试信息，在日志中搜索上述标记。

## API 参考

### Print 类

```cpp
// 初始化耗材-挤出机映射
void initialize_filament_extruder_map();

// 映射存储
std::unordered_map<int, int> m_filament_extruder_map;
```

### GCodeProcessor 类

```cpp
// 设置映射
void set_filament_extruder_map(const std::unordered_map<int, int>& map);
void set_physical_extruder_count(size_t count);

// 获取物理挤出机
int get_physical_extruder(int filament_idx) const;
```

### CoolingBuffer 类

```cpp
// 设置映射
void set_filament_extruder_map(const std::unordered_map<int, int>& map);
void set_physical_extruder_count(size_t count);

// 获取物理挤出机
int get_physical_extruder(int filament_idx) const;
```

### ConfigOptionVector 模板

```cpp
// 带映射的 apply_override
bool apply_override(const ConfigOption *rhs, const std::vector<int>& map_indices);

// 检查元素是否为 nil
bool is_nil(size_t idx) const;

// 检查类型是否支持 nullable
bool nullable() const;
```

## 最佳实践

1. **始终使用模运算回退**: 确保在映射不存在时有确定的行为
2. **检查原始配置**: 在清理逻辑中检查 `new_full_config`，不是 `filament_overrides`
3. **统一初始化**: 所有使用映射的组件都必须正确初始化
4. **添加调试日志**: 关键路径添加日志便于问题排查

## 相关文件

| 文件 | 作用 |
|------|------|
| `src/libslic3r/Config.hpp` | 配置系统定义 |
| `src/libslic3r/PrintApply.cpp` | 映射处理逻辑 |
| `src/libslic3r/GCode.cpp` | G-code 生成入口 |
| `src/libslic3r/GCode/GCodeProcessor.hpp/cpp` | G-code 处理 |
| `src/libslic3r/GCode/CoolingBuffer.hpp/cpp` | 冷却控制 |
| `scripts/analyze_gcode_params.py` | G-code 验证工具 |

---

*文档版本: 2026-02-13*
*适用版本: OrcaSlicer 2.3.0+*
