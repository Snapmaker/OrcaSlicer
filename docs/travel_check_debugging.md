# Travel移动边界检查 - 调试指南

## 当前状态

✅ **Travel检查已临时禁用** - 不影响正常切片
⚠️ **正在调试误报问题** - 标准正方体不应该报超限

---

## 问题诊断步骤

### 1. 启用详细日志模式

修改 `GCodeViewer.cpp:2403-2414`，改为诊断模式：

```cpp
// BBS: DIAGNOSTIC MODE - Check Travel moves but only log, don't fail
if (m_contained_in_bed) {
    BOOST_LOG_TRIVIAL(info) << "=== Diagnostic: Checking Travel moves ===";
    bool all_moves_valid = build_volume.all_moves_inside(gcode_result, m_paths_bounding_box);

    if (!all_moves_valid) {
        // 只记录警告，不设置为失败
        BOOST_LOG_TRIVIAL(warning) << "Travel moves detected outside build volume (diagnostic only, not failing)";
        // m_contained_in_bed = false;  // 注释掉，不影响切片
    } else {
        BOOST_LOG_TRIVIAL(info) << "All Travel moves are within bounds";
    }
}
```

### 2. 查看日志输出

切片一个标准正方体，查看日志文件（通常在 `%APPDATA%\OrcaSlicer\log\`）

关键信息：
```
Move #123 (Travel) outside bounds: pos=(x, y, z), bounds=[...]
```

### 3. 可能的原因

#### 原因A: 负Z值（床底部以下）

有些 Travel 移动可能在 Z < 0 的位置（床下方）。

**检查方法**：看日志中的 Z 坐标是否为负数

**修复方案**：
```cpp
// 在 BuildVolume.cpp 中，修改 ignore_bottom 的处理
if (ignore_bottom)
    build_volume.min.z() = -std::numeric_limits<double>::max();  // 已经这样了
```

#### 原因B: 擦料塔区域

擦料塔可能在床边界外的特殊区域。

**检查方法**：看超限位置是否在擦料塔坐标附近

**修复方案**：需要排除擦料塔区域的检查

#### 原因C: 初始G28/G29命令

归零或床调平命令可能移动到床外。

**检查方法**：看超限的行号是否在文件开头

**修复方案**：只检查打印主体部分，跳过开头的初始化命令

#### 原因D: Epsilon容差不够

浮点误差导致边界检查过于严格。

**检查方法**：看超出距离（会在日志中显示）

**修复方案**：增大 epsilon 值

---

## 快速修复方案

### 方案1: 增大容差（推荐）

修改 `BuildVolume.cpp:377`:

```cpp
// 原来
static constexpr const double epsilon = BedEpsilon;  // 3 * EPSILON ≈ 3e-5

// 改为
static constexpr const double epsilon = 0.5;  // 允许0.5mm误差
```

### 方案2: 忽略床底部以下的移动

在 `all_moves_inside()` 中添加：

```cpp
if (move_significant(move)) {
    // 跳过床底部以下的移动（可能是归零命令）
    if (ignore_bottom && move.position.z() < -epsilon) {
        continue;  // 不检查
    }

    if (!build_volume.contains(move.position.cast<double>())) {
        violation_count++;
        // ...
    }
}
```

### 方案3: 只检查打印主体

在 GCodeViewer 中，只检查 Z > 某个阈值的移动：

```cpp
bool all_moves_valid = build_volume.all_moves_inside(gcode_result, m_paths_bounding_box);
// 或者创建一个新方法： all_moves_inside_print_area()
```

---

## 临时禁用/启用

### 完全禁用Travel检查（当前状态）

文件：`GCodeViewer.cpp:2403-2414`

```cpp
// 注释掉整个检查块（已完成）
/*
if (m_contained_in_bed) {
    bool all_moves_valid = build_volume.all_moves_inside(...);
    ...
}
*/
```

### 启用但不报错（诊断模式）

```cpp
if (m_contained_in_bed) {
    bool all_moves_valid = build_volume.all_moves_inside(gcode_result, m_paths_bounding_box);
    if (!all_moves_valid) {
        BOOST_LOG_TRIVIAL(warning) << "Travel moves outside (diagnostic)";
        // 不设置 m_contained_in_bed = false
    }
}
```

### 完全启用（修复后）

```cpp
if (m_contained_in_bed) {
    bool all_moves_valid = build_volume.all_moves_inside(gcode_result, m_paths_bounding_box);
    if (!all_moves_valid) {
        m_contained_in_bed = false;  // 启用失败标志
        BOOST_LOG_TRIVIAL(warning) << "Travel moves detected outside build volume boundaries";
    }
}
```

---

## 下一步

1. **立即**：用户可以正常使用（Travel检查已禁用）
2. **诊断**：启用日志模式，切片标准正方体，查看日志
3. **修复**：根据日志内容选择上述修复方案
4. **验证**：修复后启用检查，测试不同模型
5. **发布**：确认无误报后正式启用

---

## 测试用例

修复后需要测试：

- ✅ 标准正方体 (20mm) - 床中心
- ✅ 大物体 (190×190mm) - 接近边界
- ✅ 多物体打印
- ✅ 使用擦料塔的多材料打印
- ✅ 使用Skirt/Brim的打印
- ✅ 圆形床（Delta打印机）

---

**当前版本**: Travel检查已禁用，不影响用户使用
**优先级**: P2（功能可用，待优化）
**估计修复时间**: 需要实际日志数据分析
